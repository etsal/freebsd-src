/*
 * SPDX-License-Identifier: BSD-2-Clause-FreeBSD
 *
 * Copyright (c) 2026, Emil Tsalapatis <emil@etsalapatis.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef _VIRTIO_FS_INTERNAL_
#define _VIRTIO_FS_INTERNAL_

/* Protocol-specified file system tag size. */
#define TAG_SIZE (36)
#define FSQ_NAME_SIZE (16)

/* Struct for the file system instance. Provided by the host. */
struct vtfs_config {
	/* UTF-8 File system tag. */
	uint8_t tag[TAG_SIZE];
	/* Number of request queues. */
	uint32_t num_request_queues;
	/* Minimum # bytes for each buffer in the notification queue. */
	uint32_t notify_buf_size;
} __packed;

/* A queue structure belonging to a virtio fs device. */
struct vtfs_fsq {
	struct mtx		vtfsq_mtx;
	struct cv		vtfsq_cv;
	struct virtqueue	*vtfsq_vq;
	struct taskqueue	*vtfsq_tq;
	struct sglist 		*vtfsq_sg;
	char			vtfsq_name[FSQ_NAME_SIZE];
	struct vtfs_softc	*vtfsq_sc;
	vtfs_fuse_cb		vtfsq_cb;
};

/* A single virtio fs device instance. */
struct vtfs_softc {
	device_t	vtfs_dev;
	struct mtx	vtfs_mtx;
	bool		vtfs_attached;
	struct vtfs_fsq	*vtfs_fsqs;
	bool		vtfs_inuse;	/* protected by the vtfs modulelock */
	vtfs_teardown_cb	vtfs_detach_cb;
	void		*vtfs_detach_cb_arg;
	/* Host-provided config state. */
	uint8_t 	vtfs_tag[TAG_SIZE + 1];
	uint32_t 	vtfs_nqs;
	vtfs_fuse_cb	cancel_cb;
	LIST_ENTRY(vtfs_softc) vtfs_link;
};

#define VTFS_FORGET_FSQ (0)
#define VTFS_REGULAR_FSQ (1)

#define VTFS_MAXSEGS (16)
#define VTFS_TQTHREAD (4)

#endif /* _VIRTIO_FS_INTERNAL_ */
