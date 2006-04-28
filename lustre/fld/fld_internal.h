/* -*- MODE: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  fld/fld_internal.h
 *
 *  Copyright (C) 2006 Cluster File Systems, Inc.
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 */
#ifndef _FLD_INTERNAL_H
#define _FLD_INTERNAL_H

#define mdsno_t  __u64
#define fidseq_t __u64

#define key_cmp(e1, e2) ({                              \
        typeof(e1) __e1 = (e1);                         \
        typeof(e2) __e2 = (e2);                         \
        __e1 > __e2 ? +1 : (__e1 < __e2 ? -1 : 0);      \
})

struct fld_cache {
        struct hlist_node fld_list;
        __u64             fld_mds;
        __u64             fld_seq;
};
struct fld_cache_info {
        struct hlist_head *fld_hash;
        spinlock_t fld_lock;
        int fld_hash_mask;
};
/*XXX use linked list temp for fld in this prototype*/
struct fld_list {
        struct list_head fld_list;
        spinlock_t       fld_lock;
};
struct fld_item{
        struct list_head fld_list;
        __u64 fld_seq;
        __u64 fld_mds;
};

enum fld_op {
        FLD_CREATE = 0,
        FLD_DELETE = 1,
        FLD_GET    = 2
};

#define FLD_SERVICE_WATCHDOG_TIMEOUT (obd_timeout * 1000)


struct fld_info {
        void *fi_container;
};

int fld_handle_insert(struct fld_info *fld, fidseq_t seq_num, mdsno_t mdsno);
int fld_handle_delete(struct fld_info *fld, fidseq_t seq_num, mdsno_t mdsno);
int fld_handle_lookup(struct fld_info *fld, fidseq_t seq_num, mdsno_t *mds);

int fld_info_init(struct fld_info *fld_info);
void fld_info_fini(struct fld_info *fld_info);
#endif
