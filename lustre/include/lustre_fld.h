/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2006 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */

#ifndef __LINUX_FLD_H
#define __LINUX_FLD_H

#include <lustre/lustre_idl.h>
#include <dt_object.h>

#include <libcfs/list.h>
#include <libcfs/kp30.h>

struct lu_client_fld;
struct lu_server_fld;

/*
 * FLD (Fid Location Database) interface.
 */
enum {
        LUSTRE_CLI_FLD_HASH_DHT = 0,
        LUSTRE_CLI_FLD_HASH_RRB
};

struct lu_server_fld;

struct lu_fld_target {
        struct list_head         ft_chain;
        struct obd_export       *ft_exp;
        struct lu_server_fld    *ft_srv;
        __u64                    ft_idx;
};

typedef int
(*fld_hash_func_t) (struct lu_client_fld *, __u64);

typedef struct lu_fld_target *
(*fld_scan_func_t) (struct lu_client_fld *, __u64);

struct lu_fld_hash {
        const char              *fh_name;
        fld_hash_func_t          fh_hash_func;
        fld_scan_func_t          fh_scan_func;
};

struct lu_server_fld {
        /* service proc entry */
        cfs_proc_dir_entry_t    *lsf_proc_entry;

        /* fld dir proc entry */
        cfs_proc_dir_entry_t    *lsf_proc_dir;

        /* pointer to started server service */
        struct ptlrpc_service   *lsf_service;

        /* /fld file object device */
        struct dt_object        *lsf_obj;

        /* fld service name in form "fld-MDTXXX" */
        char                     lsf_name[80];
};

struct fld_cache_entry {
        struct hlist_node        fce_list;
        struct list_head         fce_lru;
        mdsno_t                  fce_mds;
        seqno_t                  fce_seq;
};

struct fld_cache_info {
        /* 
         * cache guard, protects fci_hash mostly because others immutable after
         * init is finished.
         */
        spinlock_t               fci_lock;

        /* cache shrink threshold */
        int                      fci_threshold;
        
        /* prefered number of cached entries */
        int                      fci_cache_size;

        /* current number of cached entries. Protected by @fci_lock */
        int                      fci_cache_count;

        /* hash table size (number of collision lists) */
        int                      fci_hash_size;

        /* hash table mask */
        int                      fci_hash_mask;

        /* hash table for all collision lists */
        struct hlist_head       *fci_hash_table;

        /* lru list */
        struct list_head         fci_lru;
};

struct lu_client_fld {
        /* client side proc entry */
        cfs_proc_dir_entry_t    *lcf_proc_dir;

        /* list of exports client FLD knows about */
        struct list_head         lcf_targets;

        /* current hash to be used to chose an export */
        struct lu_fld_hash      *lcf_hash;

        /* exports count */
        int                      lcf_count;

        /* lock protecting exports list and fld_hash */
        spinlock_t               lcf_lock;

        /* client FLD cache */
        struct fld_cache_info   *lcf_cache;

        /* client fld proc entry name */
        char                     lcf_name[80];

        const struct lu_context       *lcf_ctx;
};

/* server methods */
int fld_server_init(struct lu_server_fld *fld,
                    const struct lu_context *ctx,
                    struct dt_device *dt,
                    const char *uuid);

void fld_server_fini(struct lu_server_fld *fld,
                     const struct lu_context *ctx);

int fld_server_create(struct lu_server_fld *fld,
                      const struct lu_context *ctx,
                      seqno_t seq, mdsno_t mds);

int fld_server_delete(struct lu_server_fld *fld,
                      const struct lu_context *ctx,
                      seqno_t seq);

int fld_server_lookup(struct lu_server_fld *fld,
                      const struct lu_context *ctx,
                      seqno_t seq, mdsno_t *mds);

/* client methods */
int fld_client_init(struct lu_client_fld *fld,
                    const char *prefix, int hash,
                    const struct lu_context *ctx);

void fld_client_fini(struct lu_client_fld *fld);

int fld_client_lookup(struct lu_client_fld *fld,
                      seqno_t seq, mdsno_t *mds);

int fld_client_create(struct lu_client_fld *fld,
                      seqno_t seq, mdsno_t mds);

int fld_client_delete(struct lu_client_fld *fld,
                      seqno_t seq);

int fld_client_add_target(struct lu_client_fld *fld,
                          struct lu_fld_target *tar);

int fld_client_del_target(struct lu_client_fld *fld,
                          __u64 idx);

/* cache methods */
struct fld_cache_info *fld_cache_init(int hash_size,
                                      int cache_size,
                                      int cache_threshold);

void fld_cache_fini(struct fld_cache_info *cache);

int fld_cache_insert(struct fld_cache_info *cache,
                     seqno_t seq, mdsno_t mds);

void fld_cache_delete(struct fld_cache_info *cache,
                      seqno_t seq);

int
fld_cache_lookup(struct fld_cache_info *cache,
                 seqno_t seq, mdsno_t *mds);

#endif
