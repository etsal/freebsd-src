/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2024, Emil Tsalapatis <emil@etsalapatis.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/buf.h>
#include <sys/lock.h>
#include <sys/queue.h>
#include <sys/systm.h>
#include <sys/mount.h>
#include <sys/proc.h>
#include <sys/sglist.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>

#include "fuse.h"
#include "fuse_kernel.h"
#include "fuse_internal.h"
#include "fuse_ipc.h"
#include "fuse_vfsops.h"

#include <dev/virtio/fs/virtio_fs.h>

#include <compat/linux/linux_errno.h>
#include <compat/linux/linux_errno.inc>

static vfs_mount_t virtiofs_vfsop_mount;

/* Only mount/unmount is different compared to fuse. */
static struct vfsops virtiofs_vfsops = {
	.vfs_fhtovp = fuse_vfsop_fhtovp,
	.vfs_mount = virtiofs_vfsop_mount,
	.vfs_unmount = fuse_vfsop_unmount,
	.vfs_root = fuse_vfsop_root,
	.vfs_statfs = fuse_vfsop_statfs,
	.vfs_vget = fuse_vfsop_vget,
};

static struct vfsconf virtiofs_vfsconf = {
	.vfc_version = VFS_VERSION,
	.vfc_name = "virtiofs",
	.vfc_vfsops = &virtiofs_vfsops,
	.vfc_typenum = -1,
	.vfc_flags = VFCF_JAIL | VFCF_SYNTHETIC
};

static int
virtiofs_loader(struct module *m, int what, void *arg)
{
	int error = 0;

	switch (what) {
	case MOD_LOAD:
		error = vfs_modevent(NULL, what, &virtiofs_vfsconf);
		break;
	case MOD_UNLOAD:
		error = vfs_modevent(NULL, what, &virtiofs_vfsconf);
		break;
	default:
		return (EINVAL);
	}

	return (error);
}

/* Registering the module */

static moduledata_t virtiofs_moddata = {
	"virtiofs",
	virtiofs_loader,
	&virtiofs_vfsconf
};

DECLARE_MODULE(virtiofs, virtiofs_moddata, SI_SUB_VFS, SI_ORDER_MIDDLE);
MODULE_DEPEND(virtiofs, fusefs, 1, 1, 1);
MODULE_DEPEND(virtiofs, vtfs, 1, 1, 1);
MODULE_VERSION(virtiofs, 1);

/* Push the ticket to the virtiofs device. */
static int
virtiofs_enqueue(struct fuse_ticket *ftick)
{
	struct fuse_out_header *ohead = &ftick->tk_aw_ohead;
	struct fuse_data *data = ftick->tk_data;
	struct fuse_iov *riov, *wiov;
	struct sglist *sg = NULL;
	int readable, writable;
	bool urgent;
	int error;

	urgent = (fticket_opcode(ftick) == FUSE_FORGET);
	if (urgent)
		refcount_acquire(&ftick->tk_refcount);

	riov = &ftick->tk_ms_fiov;
	wiov = &ftick->tk_aw_fiov;

	/* Preallocate the response buffer. */
	fiov_adjust(wiov, fticket_out_size(ftick));

	/* Readable/writable from the host's point of view. */
	readable = sglist_count(riov->base, riov->len);

	/* Account for the out header. */
	writable = sglist_count(ohead, sizeof(*ohead)) + 
		sglist_count(wiov->base, wiov->len);

	sg = sglist_alloc(readable + writable, M_WAITOK);

	error = sglist_append(sg, riov->base, riov->len);
	if (error != 0)
		goto out;

	error = sglist_append(sg, ohead, sizeof(*ohead));
	if (error != 0)
		goto out;

	error = sglist_append(sg, wiov->base, wiov->len);
	if (error != 0)
		goto out;

	error = vtfs_enqueue(data->vtfs, ftick, sg, readable, writable, urgent);
	if (error != 0 && urgent)
		fuse_ticket_drop(ftick);

	/*
	 * The enqueue call destroys the scatter-gather array both on success and
	 * on failure, so no need to clean it up.
	 */

	return (error);

out:
	if (urgent)
		fuse_ticket_drop(ftick);

	if (sg != NULL)
		sglist_free(sg);

	return (error);
}

static void
virtiofs_cb_forget_ticket(void *xtick, uint32_t len __unused)
{
	struct fuse_ticket *ftick = xtick;

	fuse_ticket_drop(ftick);
}

static void
virtiofs_drop_intr_tick(struct fuse_data *data, struct fuse_ticket *ftick)
{
	struct fuse_ticket *itick, *x_tick;

	TAILQ_FOREACH_SAFE(itick, &data->aw_head, tk_aw_link, x_tick) {
		if (itick->tk_unique == ftick->irq_unique) {
			fuse_aw_remove(itick);
			break;
		}
	}

	if (itick) {
		MPASS(itick->tk_refcount == 1);
		fuse_ticket_drop(itick);
	}

	ftick->irq_unique = 0;
}

static int
virtiofs_handle_async_tick(struct fuse_data *data, struct fuse_ticket *ftick, int oerror)
{
	struct mount *mp = data->mp;
	struct iovec aiov;
	struct uio uio;
	int err = 0;

	/* 
	 * Form a uio and pass it to the message handlers, because unlike other
	 * messages they do not use ftick->tk_aw_fiov to store the message body.
	 */
	aiov.iov_base = fticket_resp(ftick)->base;
	aiov.iov_len = fticket_resp(ftick)->len;

	uio.uio_iov = (struct iovec *)&aiov;
	uio.uio_iovcnt = 1;
	uio.uio_resid = aiov.iov_len;
	uio.uio_segflg = UIO_SYSSPACE;
	uio.uio_rw = UIO_WRITE;
	uio.uio_td = curthread;
	uio.uio_offset = 0;

	/* Only handle the two async messages that the FUSE device does. */
	switch (oerror) {
	case FUSE_NOTIFY_INVAL_ENTRY:
		err = fuse_internal_invalidate_entry(mp, &uio);
		break;
	case FUSE_NOTIFY_INVAL_INODE:
		err = fuse_internal_invalidate_inode(mp, &uio);
		break;
	default:
		err = ENOSYS;
	}

	if (err != 0) {
		printf("WARNING: error %d when handling async message of type %d\n",
			err, fticket_opcode(ftick));
	}

	return (err);
}

static bool
virtiofs_remove_ticket(struct fuse_data *data, struct fuse_ticket *ftick)
{
	struct fuse_ticket *tick, *x_tick;

	mtx_assert(&data->aw_mtx, MA_OWNED);

	TAILQ_FOREACH_SAFE(tick, &data->aw_head, tk_aw_link, x_tick) {
		if (tick->tk_unique != ftick->tk_aw_ohead.unique)
			continue;

		MPASS(tick == ftick);
		fuse_aw_remove(ftick);

		return (true);
	}

	return (false);
}

static void
virtiofs_cb_cancel_ticket(void *xtick, uint32_t __unused len)
{
	struct fuse_ticket *ftick = xtick;
	struct fuse_data *data = ftick->tk_data;
	bool found;

	fuse_lck_mtx_lock(data->aw_mtx);

	KASSERT(fticket_opcode(ftick) != FUSE_DESTROY, ("unsent FUSE_DESTROY ticket"));

	found = virtiofs_remove_ticket(data, ftick);
	if (found && ftick->irq_unique > 0)
		virtiofs_drop_intr_tick(data, ftick);

	fuse_lck_mtx_unlock(data->aw_mtx);

	fuse_ticket_drop(ftick);
}

static void
virtiofs_cb_destroy_ticket(struct fuse_data *data)
{
	mtx_lock(&data->virtiofs_mtx);

	KASSERT(!data->virtiofs_destroy_acked, ("virtiofs session already destroyed"));
	data->virtiofs_destroy_acked = true;

	cv_signal(&data->virtiofs_cv);

	mtx_unlock(&data->virtiofs_mtx);
}

static void
virtiofs_cb_complete_ticket(void *xtick, uint32_t len)
{
	struct fuse_ticket *ftick = xtick;
	struct fuse_data *data = ftick->tk_data;
	struct fuse_out_header *ohead = &ftick->tk_aw_ohead;
	bool found;
	int err;

	/* 
	 * The ticket that got acknowledged is FUSE_DESTROY.
	 * Notify the module that we are ready to unload.
	 * FUSE_DESTROY doesn't require a response from the
	 * server, so what we are handling here is not a
	 * response but an acknowledgement of receipt.
	 */
	if (fticket_opcode(ftick) == FUSE_DESTROY) {
		fuse_ticket_drop(ftick);
		virtiofs_cb_destroy_ticket(data);
		return;
	}

	/* Validate the length field of the out header. */
	if (len != ohead->len) {
		err = EINVAL;
		goto done;
	}

	/* Error responses to tickets do not have a body. */
	if (len > sizeof(*ohead) && ohead->unique != 0 && ohead->error) {
		err = EINVAL;
		goto done;
	}

	/* Ensure that out headers that return an error are valid. */
	if (data->linux_errnos != 0 && ohead->error != 0) {
		err = -ohead->error;
		if (err < 0 || err >= nitems(linux_to_bsd_errtbl))
			goto done;

		/* '-', because it will get flipped again below */
		ohead->error = -linux_to_bsd_errtbl[err];
	}

	/* Remove the ticket from the answer queue. */
	fuse_lck_mtx_lock(data->aw_mtx);

	found = virtiofs_remove_ticket(data, ftick);

	/*
	 * We should not be able to find a non-unique ticket, and
	 * all unique tickets should still be in the queue.
	 */
	KASSERT(found == (ohead->unique != 0),
		("inconsistency in answer queue:"
		"found %d unique %lu", found, ohead->unique));

	/* Drop any pending interrupts for the completed ticket. */
	if (found && ftick->irq_unique > 0)
		virtiofs_drop_intr_tick(data, ftick);

	fuse_lck_mtx_unlock(data->aw_mtx);

	/* If the operation was successful, ensure the size is valid. */
	if (ohead->error == 0 && ohead->unique != 0) {
		err = fuse_body_audit(ftick, len - sizeof(*ohead));
		if (err)
			goto done;

		fiov_adjust(fticket_resp(ftick), len - sizeof(*ohead));
	}

	if (found && ftick->tk_aw_handler) {
		/* Sanitize the linuxism of negative errnos */
		ohead->error *= -1;

		/* Illegal error code, treat it as EIO. */
		if (ohead->error < 0 || ohead->error > ELAST) {
			ohead->error = EIO;
			ftick->tk_aw_handler(ftick, NULL);
			err = EINVAL;
		} else {
			err = ftick->tk_aw_handler(ftick, NULL);
		}

	} else if (!found && ohead->unique == 0) {
		err = virtiofs_handle_async_tick(data, ftick, ohead->error);
	}

done:
	fuse_ticket_drop(ftick);

	/* 
	 * If something goes wrong, err on the side of caution and kill the session
	 * because the FUSE server in the host is misbehaving.
	 */
	if (err != 0 && err != ENOSYS)
		fdata_set_dead(data);

	return;
}

static int
virtiofs_vfsop_mount(struct mount *mp)
{
	/* Turn interrupts on by default, existing virtiofsd servers use them anyway. */
	const uint64_t mntopts = FSESS_VIRTIOFS | FSESS_DAEMON_CAN_SPY;
	struct thread *td = curthread;
	struct vfsoptlist *opts;
	struct fuse_data *data;
	vtfs_instance vtfs;
	uint32_t max_read;
	char *from;
	int error;

	opts = mp->mnt_optnew;
	if (opts == NULL)
		return (EINVAL);

	/* `fspath' contains the mount point (eg. /mnt/guestfs); REQUIRED */
	if (!vfs_getopts(opts, "fspath", &error))
		return (error);

	max_read = maxbcachebuf;
	(void)vfs_scanopt(opts, "max_read=", "%u", &max_read);

	/* XXX Remounts not handled for now, but should be easy to code in. */
	if (mp->mnt_flag & MNT_UPDATE)
		return (EOPNOTSUPP);

	/* `from' contains the virtio from; REQUIRED */
	from = vfs_getopts(opts, "from", &error);
	if (!from)
		return (error);

	error = vtfs_find(from, &vtfs);
	if (error != 0)
		return (error);

	data = fdata_alloc(NULL, td->td_ucred);

	vtfs_register_cb(vtfs, virtiofs_cb_forget_ticket, virtiofs_cb_complete_ticket,
			virtiofs_cb_cancel_ticket, virtiofs_teardown, data);

	FUSE_LOCK();
	KASSERT(!fdata_get_dead(data), ("allocated dead session"));

	data->vtfs = vtfs;
	data->virtiofs_enqueue_cb = virtiofs_enqueue;
	data->virtiofs_unmount_cb = virtiofs_unmount;

	mtx_init(&data->virtiofs_mtx, "virtiofs destroy mtx", NULL, MTX_DEF);
	cv_init(&data->virtiofs_cv, "virtiofs destroy cv");
	data->virtiofs_destroy_acked = false;

	data->mp = mp;
	/* 
	 * XXX We currently do not support any mount options. This is due because it is
	 * hard to test for it, even though most FUSE options should be trivially easy
	 * to add. Deliberately defer enabling them until we can reuse the FUSE test
	 * suite for virtiofs.
	 */
	data->dataflags |= mntopts;
	data->max_read = max_read;
	data->daemon_timeout = FUSE_MIN_DAEMON_TIMEOUT;
	data->linux_errnos = 1;
	data->mnt_flag = mp->mnt_flag & MNT_UPDATEMASK;
	FUSE_UNLOCK();

	KASSERT(!fdata_get_dead(data), ("newly created fuse session is dead"));

	vfs_getnewfsid(mp);
	MNT_ILOCK(mp);
	mp->mnt_data = data;
	mp->mnt_flag &= ~MNT_LOCAL;
	mp->mnt_kern_flag |= MNTK_USES_BCACHE;
	/* 
	 * The FS is remote by default. Disable nullfs caching to avoid
	 * the extra coherence cost, same as FUSE.
	 */
	mp->mnt_kern_flag |= MNTK_NULL_NOCACHE;
	MNT_IUNLOCK(mp);

	mp->mnt_stat.f_iosize = maxbcachebuf;
	strlcpy(mp->mnt_stat.f_fstypename, "virtiofs", MFSNAMELEN);
	memset(mp->mnt_stat.f_mntfromname, 0, MNAMELEN);
	strlcpy(mp->mnt_stat.f_mntfromname, from, MNAMELEN);
	mp->mnt_iosize_max = maxphys;

	fsess_set_notimpl(data->mp, FUSE_READDIRPLUS);

	/* FUSE_INTERRUPT is not implemented in Linux virtiofs either. */
	fsess_set_notimpl(data->mp, FUSE_INTERRUPT);

	/* 
	 * vnop_fsync() has an issue where the syncer triggering
	 * a VOP_SYNC with MNT_LAZY set, leading to two FUSE_FSYNC 
	 * tickets in the answer queue for the same vnode. This
	 * causes an assertion failure, so disable it for now.
	 */
	fsess_set_notimpl(data->mp, FUSE_FSYNC);

	/* Now handshaking with daemon */
	fuse_internal_send_init(data, td);

	return (0);
}

void
virtiofs_teardown(void *xdata)
{
	struct fuse_data *data = (struct fuse_data *)xdata;
	vtfs_instance vtfs = data->vtfs;

	/* Mark the session as dead to prevent new requests. */
	fdata_set_dead(data);

	/*
	 * Turn off the device and handle all received
	 * requests. After this there are no guest-bound
	 * requests in flight, completing virtiofs teardown.
	 */
	vtfs_drain(vtfs);

	vtfs_unregister_cb(vtfs);
	vtfs_release(vtfs);
}

void
virtiofs_unmount(struct mount *mp, struct fuse_data *data)
{
	struct thread *td = curthread;
	struct fuse_dispatcher fdi;

	if (!fsess_maybe_impl(mp, FUSE_DESTROY))
		goto destroy_acked;

	fdisp_init(&fdi, 0);
	fdisp_make(&fdi, FUSE_DESTROY, mp, 0, td, NULL);
	fuse_insert_message(fdi.tick, 0);

	mtx_lock(&data->virtiofs_mtx);
	while (!data->virtiofs_destroy_acked)
		cv_wait(&data->virtiofs_cv, &data->virtiofs_mtx);
	mtx_unlock(&data->virtiofs_mtx);

destroy_acked:
	virtiofs_teardown(data);

	mtx_destroy(&data->virtiofs_mtx);
	cv_destroy(&data->virtiofs_cv);
}
