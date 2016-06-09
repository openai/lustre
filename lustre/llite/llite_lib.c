/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2014, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/llite/llite_lib.c
 *
 * Lustre Light Super operations
 */

#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/module.h>
#include <linux/statfs.h>
#include <linux/types.h>
#include <linux/version.h>
#include <linux/mm.h>
#include <linux/security.h>

#include <lustre_ioctl.h>
#include <lustre_ha.h>
#include <lustre_dlm.h>
#include <lprocfs_status.h>
#include <lustre_disk.h>
#include <lustre_param.h>
#include <lustre_log.h>
#include <cl_object.h>
#include <obd_cksum.h>
#include "llite_internal.h"

struct kmem_cache *ll_file_data_slab;

static struct list_head ll_super_blocks = LIST_HEAD_INIT(ll_super_blocks);
static DEFINE_SPINLOCK(ll_sb_lock);

#ifndef log2
#define log2(n) ffz(~(n))
#endif

static struct ll_sb_info *ll_init_sbi(void)
{
	struct ll_sb_info *sbi = NULL;
	unsigned long pages;
	unsigned long lru_page_max;
	struct sysinfo si;
	class_uuid_t uuid;
	int i;
	ENTRY;

	OBD_ALLOC_PTR(sbi);
	if (sbi == NULL)
		RETURN(NULL);

	spin_lock_init(&sbi->ll_lock);
	mutex_init(&sbi->ll_lco.lco_lock);
	spin_lock_init(&sbi->ll_pp_extent_lock);
	spin_lock_init(&sbi->ll_process_lock);
        sbi->ll_rw_stats_on = 0;

        si_meminfo(&si);
        pages = si.totalram - si.totalhigh;
	lru_page_max = pages / 2;

	/* initialize ll_cache data */
	sbi->ll_cache = cl_cache_init(lru_page_max);
	if (sbi->ll_cache == NULL) {
		OBD_FREE(sbi, sizeof(*sbi));
		RETURN(NULL);
	}

	sbi->ll_ra_info.ra_max_pages_per_file = min(pages / 32,
					   SBI_DEFAULT_READAHEAD_MAX);
	sbi->ll_ra_info.ra_max_pages = sbi->ll_ra_info.ra_max_pages_per_file;
	sbi->ll_ra_info.ra_max_read_ahead_whole_pages =
					   SBI_DEFAULT_READAHEAD_WHOLE_MAX;
	INIT_LIST_HEAD(&sbi->ll_conn_chain);
	INIT_LIST_HEAD(&sbi->ll_orphan_dentry_list);

        ll_generate_random_uuid(uuid);
        class_uuid_unparse(uuid, &sbi->ll_sb_uuid);
        CDEBUG(D_CONFIG, "generated uuid: %s\n", sbi->ll_sb_uuid.uuid);

	spin_lock(&ll_sb_lock);
	list_add_tail(&sbi->ll_list, &ll_super_blocks);
	spin_unlock(&ll_sb_lock);

        sbi->ll_flags |= LL_SBI_VERBOSE;
#ifdef ENABLE_CHECKSUM
        sbi->ll_flags |= LL_SBI_CHECKSUM;
#endif

#ifdef HAVE_LRU_RESIZE_SUPPORT
        sbi->ll_flags |= LL_SBI_LRU_RESIZE;
#endif
	sbi->ll_flags |= LL_SBI_LAZYSTATFS;

        for (i = 0; i <= LL_PROCESS_HIST_MAX; i++) {
		spin_lock_init(&sbi->ll_rw_extents_info.pp_extents[i].
			       pp_r_hist.oh_lock);
		spin_lock_init(&sbi->ll_rw_extents_info.pp_extents[i].
			       pp_w_hist.oh_lock);
        }

	/* metadata statahead is enabled by default */
	sbi->ll_sa_max = LL_SA_RPC_DEF;
	atomic_set(&sbi->ll_sa_total, 0);
	atomic_set(&sbi->ll_sa_wrong, 0);
	atomic_set(&sbi->ll_sa_running, 0);
	atomic_set(&sbi->ll_agl_total, 0);
	sbi->ll_flags |= LL_SBI_AGL_ENABLED;
	sbi->ll_flags |= LL_SBI_FAST_READ;

	/* root squash */
	sbi->ll_squash.rsi_uid = 0;
	sbi->ll_squash.rsi_gid = 0;
	INIT_LIST_HEAD(&sbi->ll_squash.rsi_nosquash_nids);
	init_rwsem(&sbi->ll_squash.rsi_sem);

	RETURN(sbi);
}

static void ll_free_sbi(struct super_block *sb)
{
	struct ll_sb_info *sbi = ll_s2sbi(sb);
	ENTRY;

	if (sbi != NULL) {
		spin_lock(&ll_sb_lock);
		list_del(&sbi->ll_list);
		spin_unlock(&ll_sb_lock);
		if (!list_empty(&sbi->ll_squash.rsi_nosquash_nids))
			cfs_free_nidlist(&sbi->ll_squash.rsi_nosquash_nids);
		if (sbi->ll_cache != NULL) {
			cl_cache_decref(sbi->ll_cache);
			sbi->ll_cache = NULL;
		}
		OBD_FREE(sbi, sizeof(*sbi));
	}
	EXIT;
}

static inline int obd_connect_has_secctx(struct obd_connect_data *data)
{
	return data->ocd_connect_flags & OBD_CONNECT_FLAGS2 &&
	       data->ocd_connect_flags2 & OBD_CONNECT2_FILE_SECCTX;
}

static int client_common_fill_super(struct super_block *sb, char *md, char *dt,
                                    struct vfsmount *mnt)
{
	struct inode *root = NULL;
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        struct obd_device *obd;
        struct obd_capa *oc = NULL;
        struct obd_statfs *osfs = NULL;
        struct ptlrpc_request *request = NULL;
        struct obd_connect_data *data = NULL;
        struct obd_uuid *uuid;
        struct md_op_data *op_data;
        struct lustre_md lmd;
	u64 valid;
        int size, err, checksum;
        ENTRY;

        obd = class_name2obd(md);
        if (!obd) {
                CERROR("MD %s: not setup or attached\n", md);
                RETURN(-EINVAL);
        }

        OBD_ALLOC_PTR(data);
        if (data == NULL)
                RETURN(-ENOMEM);

        OBD_ALLOC_PTR(osfs);
        if (osfs == NULL) {
                OBD_FREE_PTR(data);
                RETURN(-ENOMEM);
        }

        /* indicate the features supported by this client */
        data->ocd_connect_flags = OBD_CONNECT_IBITS    | OBD_CONNECT_NODEVOH  |
                                  OBD_CONNECT_ATTRFID  |
                                  OBD_CONNECT_VERSION  | OBD_CONNECT_BRW_SIZE |
                                  OBD_CONNECT_MDS_CAPA | OBD_CONNECT_OSS_CAPA |
                                  OBD_CONNECT_CANCELSET | OBD_CONNECT_FID     |
                                  OBD_CONNECT_AT       | OBD_CONNECT_LOV_V3   |
                                  OBD_CONNECT_RMT_CLIENT | OBD_CONNECT_VBR    |
                                  OBD_CONNECT_FULL20   | OBD_CONNECT_64BITHASH|
				  OBD_CONNECT_EINPROGRESS |
				  OBD_CONNECT_JOBSTATS | OBD_CONNECT_LVB_TYPE |
				  OBD_CONNECT_LAYOUTLOCK | OBD_CONNECT_PINGLESS |
				  OBD_CONNECT_MAX_EASIZE |
				  OBD_CONNECT_FLOCK_DEAD |
				  OBD_CONNECT_DISP_STRIPE | OBD_CONNECT_LFSCK |
				  OBD_CONNECT_OPEN_BY_FID |
				  OBD_CONNECT_DIR_STRIPE |
				  OBD_CONNECT_BULK_MBITS |
				  OBD_CONNECT_SUBTREE |
				  OBD_CONNECT_FLAGS2;

	data->ocd_connect_flags2 = 0;

        if (sbi->ll_flags & LL_SBI_SOM_PREVIEW)
                data->ocd_connect_flags |= OBD_CONNECT_SOM;

#ifdef HAVE_LRU_RESIZE_SUPPORT
        if (sbi->ll_flags & LL_SBI_LRU_RESIZE)
                data->ocd_connect_flags |= OBD_CONNECT_LRU_RESIZE;
#endif
#ifdef CONFIG_FS_POSIX_ACL
        data->ocd_connect_flags |= OBD_CONNECT_ACL | OBD_CONNECT_UMASK;
#endif

	if (OBD_FAIL_CHECK(OBD_FAIL_MDC_LIGHTWEIGHT))
		/* flag mdc connection as lightweight, only used for test
		 * purpose, use with care */
                data->ocd_connect_flags |= OBD_CONNECT_LIGHTWEIGHT;

        data->ocd_ibits_known = MDS_INODELOCK_FULL;
        data->ocd_version = LUSTRE_VERSION_CODE;

        if (sb->s_flags & MS_RDONLY)
                data->ocd_connect_flags |= OBD_CONNECT_RDONLY;
        if (sbi->ll_flags & LL_SBI_USER_XATTR)
                data->ocd_connect_flags |= OBD_CONNECT_XATTR;

#ifdef HAVE_MS_FLOCK_LOCK
        /* force vfs to use lustre handler for flock() calls - bug 10743 */
        sb->s_flags |= MS_FLOCK_LOCK;
#endif
#ifdef MS_HAS_NEW_AOPS
        sb->s_flags |= MS_HAS_NEW_AOPS;
#endif

        if (sbi->ll_flags & LL_SBI_FLOCK)
                sbi->ll_fop = &ll_file_operations_flock;
        else if (sbi->ll_flags & LL_SBI_LOCALFLOCK)
                sbi->ll_fop = &ll_file_operations;
        else
                sbi->ll_fop = &ll_file_operations_noflock;

        /* real client */
        data->ocd_connect_flags |= OBD_CONNECT_REAL;
        if (sbi->ll_flags & LL_SBI_RMT_CLIENT)
                data->ocd_connect_flags |= OBD_CONNECT_RMT_CLIENT_FORCE;

#ifdef HAVE_SECURITY_DENTRY_INIT_SECURITY
	data->ocd_connect_flags2 |= OBD_CONNECT2_FILE_SECCTX;
#endif /* HAVE_SECURITY_DENTRY_INIT_SECURITY */

	data->ocd_brw_size = MD_MAX_BRW_SIZE;

        err = obd_connect(NULL, &sbi->ll_md_exp, obd, &sbi->ll_sb_uuid, data, NULL);
        if (err == -EBUSY) {
                LCONSOLE_ERROR_MSG(0x14f, "An MDT (md %s) is performing "
                                   "recovery, of which this client is not a "
                                   "part. Please wait for recovery to complete,"
                                   " abort, or time out.\n", md);
                GOTO(out, err);
        } else if (err) {
                CERROR("cannot connect to %s: rc = %d\n", md, err);
                GOTO(out, err);
        }

	sbi->ll_md_exp->exp_connect_data = *data;

	err = obd_fid_init(sbi->ll_md_exp->exp_obd, sbi->ll_md_exp,
			   LUSTRE_SEQ_METADATA);
	if (err) {
		CERROR("%s: Can't init metadata layer FID infrastructure, "
		       "rc = %d\n", sbi->ll_md_exp->exp_obd->obd_name, err);
		GOTO(out_md, err);
	}

	/* For mount, we only need fs info from MDT0, and also in DNE, it
	 * can make sure the client can be mounted as long as MDT0 is
	 * avaible */
	err = obd_statfs(NULL, sbi->ll_md_exp, osfs,
			cfs_time_shift_64(-OBD_STATFS_CACHE_SECONDS),
			OBD_STATFS_FOR_MDT0);
	if (err)
		GOTO(out_md_fid, err);

	/* This needs to be after statfs to ensure connect has finished.
	 * Note that "data" does NOT contain the valid connect reply.
	 * If connecting to a 1.8 server there will be no LMV device, so
	 * we can access the MDC export directly and exp_connect_flags will
	 * be non-zero, but if accessing an upgraded 2.1 server it will
	 * have the correct flags filled in.
	 * XXX: fill in the LMV exp_connect_flags from MDC(s). */
	valid = exp_connect_flags(sbi->ll_md_exp) & CLIENT_CONNECT_MDT_REQD;
	if (exp_connect_flags(sbi->ll_md_exp) != 0 &&
	    valid != CLIENT_CONNECT_MDT_REQD) {
		char *buf;

		OBD_ALLOC_WAIT(buf, PAGE_CACHE_SIZE);
		obd_connect_flags2str(buf, PAGE_CACHE_SIZE,
				      valid ^ CLIENT_CONNECT_MDT_REQD, 0, ",");
		LCONSOLE_ERROR_MSG(0x170, "Server %s does not support "
				   "feature(s) needed for correct operation "
				   "of this client (%s). Please upgrade "
				   "server or downgrade client.\n",
				   sbi->ll_md_exp->exp_obd->obd_name, buf);
		OBD_FREE(buf, PAGE_CACHE_SIZE);
		GOTO(out_md_fid, err = -EPROTO);
	}

	size = sizeof(*data);
	err = obd_get_info(NULL, sbi->ll_md_exp, sizeof(KEY_CONN_DATA),
			   KEY_CONN_DATA,  &size, data, NULL);
	if (err) {
		CERROR("%s: Get connect data failed: rc = %d\n",
		       sbi->ll_md_exp->exp_obd->obd_name, err);
		GOTO(out_md_fid, err);
	}

	LASSERT(osfs->os_bsize);
        sb->s_blocksize = osfs->os_bsize;
        sb->s_blocksize_bits = log2(osfs->os_bsize);
        sb->s_magic = LL_SUPER_MAGIC;
        sb->s_maxbytes = MAX_LFS_FILESIZE;
        sbi->ll_namelen = osfs->os_namelen;
        sbi->ll_max_rw_chunk = LL_DEFAULT_MAX_RW_CHUNK;

        if ((sbi->ll_flags & LL_SBI_USER_XATTR) &&
            !(data->ocd_connect_flags & OBD_CONNECT_XATTR)) {
                LCONSOLE_INFO("Disabling user_xattr feature because "
                              "it is not supported on the server\n");
                sbi->ll_flags &= ~LL_SBI_USER_XATTR;
        }

        if (data->ocd_connect_flags & OBD_CONNECT_ACL) {
#ifdef MS_POSIXACL
                sb->s_flags |= MS_POSIXACL;
#endif
                sbi->ll_flags |= LL_SBI_ACL;
        } else {
                LCONSOLE_INFO("client wants to enable acl, but mdt not!\n");
#ifdef MS_POSIXACL
                sb->s_flags &= ~MS_POSIXACL;
#endif
                sbi->ll_flags &= ~LL_SBI_ACL;
        }

        if (data->ocd_connect_flags & OBD_CONNECT_RMT_CLIENT) {
                if (!(sbi->ll_flags & LL_SBI_RMT_CLIENT)) {
                        sbi->ll_flags |= LL_SBI_RMT_CLIENT;
                        LCONSOLE_INFO("client is set as remote by default.\n");
                }
        } else {
                if (sbi->ll_flags & LL_SBI_RMT_CLIENT) {
                        sbi->ll_flags &= ~LL_SBI_RMT_CLIENT;
                        LCONSOLE_INFO("client claims to be remote, but server "
                                      "rejected, forced to be local.\n");
                }
        }

        if (data->ocd_connect_flags & OBD_CONNECT_MDS_CAPA) {
                LCONSOLE_INFO("client enabled MDS capability!\n");
                sbi->ll_flags |= LL_SBI_MDS_CAPA;
        }

        if (data->ocd_connect_flags & OBD_CONNECT_OSS_CAPA) {
                LCONSOLE_INFO("client enabled OSS capability!\n");
                sbi->ll_flags |= LL_SBI_OSS_CAPA;
        }

        if (data->ocd_connect_flags & OBD_CONNECT_64BITHASH)
                sbi->ll_flags |= LL_SBI_64BIT_HASH;

	if (data->ocd_connect_flags & OBD_CONNECT_BRW_SIZE)
		sbi->ll_md_brw_pages = data->ocd_brw_size >> PAGE_CACHE_SHIFT;
	else
		sbi->ll_md_brw_pages = 1;

	if (data->ocd_connect_flags & OBD_CONNECT_LAYOUTLOCK)
		sbi->ll_flags |= LL_SBI_LAYOUT_LOCK;

	if (obd_connect_has_secctx(data))
		sbi->ll_flags |= LL_SBI_FILE_SECCTX;

	if (data->ocd_ibits_known & MDS_INODELOCK_XATTR) {
		if (!(data->ocd_connect_flags & OBD_CONNECT_MAX_EASIZE)) {
			LCONSOLE_INFO("%s: disabling xattr cache due to "
				      "unknown maximum xattr size.\n", dt);
		} else {
			sbi->ll_flags |= LL_SBI_XATTR_CACHE;
			sbi->ll_xattr_cache_enabled = 1;
		}
	}

	obd = class_name2obd(dt);
	if (!obd) {
		CERROR("DT %s: not setup or attached\n", dt);
		GOTO(out_md_fid, err = -ENODEV);
	}

	/* pass client page size via ocd_grant_blkbits, the server should report
	 * back its backend blocksize for grant calculation purpose */
	data->ocd_grant_blkbits = PAGE_SHIFT;

        data->ocd_connect_flags = OBD_CONNECT_GRANT     | OBD_CONNECT_VERSION  |
				  OBD_CONNECT_REQPORTAL | OBD_CONNECT_BRW_SIZE |
                                  OBD_CONNECT_CANCELSET | OBD_CONNECT_FID      |
                                  OBD_CONNECT_SRVLOCK   | OBD_CONNECT_TRUNCLOCK|
                                  OBD_CONNECT_AT | OBD_CONNECT_RMT_CLIENT |
                                  OBD_CONNECT_OSS_CAPA | OBD_CONNECT_VBR|
                                  OBD_CONNECT_FULL20 | OBD_CONNECT_64BITHASH |
                                  OBD_CONNECT_MAXBYTES |
				  OBD_CONNECT_EINPROGRESS |
				  OBD_CONNECT_JOBSTATS | OBD_CONNECT_LVB_TYPE |
				  OBD_CONNECT_LAYOUTLOCK |
				  OBD_CONNECT_PINGLESS | OBD_CONNECT_LFSCK |
				  OBD_CONNECT_BULK_MBITS;

	if (sb->s_flags & MS_RDONLY)
		data->ocd_connect_flags |= OBD_CONNECT_RDONLY;
        if (sbi->ll_flags & LL_SBI_SOM_PREVIEW)
                data->ocd_connect_flags |= OBD_CONNECT_SOM;

	data->ocd_connect_flags2 = 0;

	if (!OBD_FAIL_CHECK(OBD_FAIL_OSC_CONNECT_GRANT_PARAM))
		data->ocd_connect_flags |= OBD_CONNECT_GRANT_PARAM;

	if (!OBD_FAIL_CHECK(OBD_FAIL_OSC_CONNECT_CKSUM)) {
		/* OBD_CONNECT_CKSUM should always be set, even if checksums are
		 * disabled by default, because it can still be enabled on the
		 * fly via /proc. As a consequence, we still need to come to an
		 * agreement on the supported algorithms at connect time */
		data->ocd_connect_flags |= OBD_CONNECT_CKSUM;

		if (OBD_FAIL_CHECK(OBD_FAIL_OSC_CKSUM_ADLER_ONLY))
			data->ocd_cksum_types = OBD_CKSUM_ADLER;
		else
			data->ocd_cksum_types = cksum_types_supported_client();
	}

#ifdef HAVE_LRU_RESIZE_SUPPORT
	data->ocd_connect_flags |= OBD_CONNECT_LRU_RESIZE;
#endif
	if (sbi->ll_flags & LL_SBI_RMT_CLIENT)
		data->ocd_connect_flags |= OBD_CONNECT_RMT_CLIENT_FORCE;

        CDEBUG(D_RPCTRACE, "ocd_connect_flags: "LPX64" ocd_version: %d "
               "ocd_grant: %d\n", data->ocd_connect_flags,
               data->ocd_version, data->ocd_grant);

	obd->obd_upcall.onu_owner = &sbi->ll_lco;
	obd->obd_upcall.onu_upcall = cl_ocd_update;

	data->ocd_brw_size = DT_MAX_BRW_SIZE;

	err = obd_connect(NULL, &sbi->ll_dt_exp, obd, &sbi->ll_sb_uuid, data,
			  NULL);
	if (err == -EBUSY) {
		LCONSOLE_ERROR_MSG(0x150, "An OST (dt %s) is performing "
				   "recovery, of which this client is not a "
				   "part.  Please wait for recovery to "
				   "complete, abort, or time out.\n", dt);
		GOTO(out_md, err);
	} else if (err) {
		CERROR("%s: Cannot connect to %s: rc = %d\n",
		       sbi->ll_dt_exp->exp_obd->obd_name, dt, err);
		GOTO(out_md, err);
	}

	sbi->ll_dt_exp->exp_connect_data = *data;

	err = obd_fid_init(sbi->ll_dt_exp->exp_obd, sbi->ll_dt_exp,
			   LUSTRE_SEQ_METADATA);
	if (err) {
		CERROR("%s: Can't init data layer FID infrastructure, "
		       "rc = %d\n", sbi->ll_dt_exp->exp_obd->obd_name, err);
		GOTO(out_dt, err);
	}

	mutex_lock(&sbi->ll_lco.lco_lock);
	sbi->ll_lco.lco_flags = data->ocd_connect_flags;
	sbi->ll_lco.lco_md_exp = sbi->ll_md_exp;
	sbi->ll_lco.lco_dt_exp = sbi->ll_dt_exp;
	mutex_unlock(&sbi->ll_lco.lco_lock);

	fid_zero(&sbi->ll_root_fid);
	err = md_get_root(sbi->ll_md_exp, get_mount_fileset(sb),
			   &sbi->ll_root_fid, &oc);
	if (err) {
		CERROR("cannot mds_connect: rc = %d\n", err);
		GOTO(out_lock_cn_cb, err);
	}
	if (!fid_is_sane(&sbi->ll_root_fid)) {
		CERROR("%s: Invalid root fid "DFID" during mount\n",
		       sbi->ll_md_exp->exp_obd->obd_name,
		       PFID(&sbi->ll_root_fid));
		GOTO(out_lock_cn_cb, err = -EINVAL);
	}
	CDEBUG(D_SUPER, "rootfid "DFID"\n", PFID(&sbi->ll_root_fid));

	sb->s_op = &lustre_super_operations;
#if THREAD_SIZE >= 8192 /*b=17630*/
	sb->s_export_op = &lustre_export_operations;
#endif

	/* make root inode
	 * XXX: move this to after cbd setup? */
	valid = OBD_MD_FLGETATTR | OBD_MD_FLBLOCKS | OBD_MD_FLMDSCAPA |
		OBD_MD_FLMODEASIZE;
	if (sbi->ll_flags & LL_SBI_RMT_CLIENT)
		valid |= OBD_MD_FLRMTPERM;
	else if (sbi->ll_flags & LL_SBI_ACL)
		valid |= OBD_MD_FLACL;

	OBD_ALLOC_PTR(op_data);
	if (op_data == NULL)
		GOTO(out_lock_cn_cb, err = -ENOMEM);

	op_data->op_fid1 = sbi->ll_root_fid;
	op_data->op_mode = 0;
	op_data->op_capa1 = oc;
	op_data->op_valid = valid;

	err = md_getattr(sbi->ll_md_exp, op_data, &request);
	if (oc)
		capa_put(oc);
	OBD_FREE_PTR(op_data);
	if (err) {
		CERROR("%s: md_getattr failed for root: rc = %d\n",
		       sbi->ll_md_exp->exp_obd->obd_name, err);
		GOTO(out_lock_cn_cb, err);
	}

	err = md_get_lustre_md(sbi->ll_md_exp, request, sbi->ll_dt_exp,
			       sbi->ll_md_exp, &lmd);
	if (err) {
		CERROR("failed to understand root inode md: rc = %d\n", err);
		ptlrpc_req_finished(request);
		GOTO(out_lock_cn_cb, err);
	}

	LASSERT(fid_is_sane(&sbi->ll_root_fid));
	root = ll_iget(sb, cl_fid_build_ino(&sbi->ll_root_fid,
					    sbi->ll_flags & LL_SBI_32BIT_API),
		       &lmd);
	md_free_lustre_md(sbi->ll_md_exp, &lmd);
	ptlrpc_req_finished(request);

	if (IS_ERR(root)) {
		if (lmd.lsm)
			obd_free_memmd(sbi->ll_dt_exp, &lmd.lsm);
#ifdef CONFIG_FS_POSIX_ACL
		if (lmd.posix_acl) {
			posix_acl_release(lmd.posix_acl);
			lmd.posix_acl = NULL;
		}
#endif
		err = IS_ERR(root) ? PTR_ERR(root) : -EBADF;
		root = NULL;
		CERROR("lustre_lite: bad iget4 for root\n");
		GOTO(out_root, err);
	}

        err = ll_close_thread_start(&sbi->ll_lcq);
        if (err) {
                CERROR("cannot start close thread: rc %d\n", err);
                GOTO(out_root, err);
        }

#ifdef CONFIG_FS_POSIX_ACL
	if (sbi->ll_flags & LL_SBI_RMT_CLIENT) {
		rct_init(&sbi->ll_rct);
		et_init(&sbi->ll_et);
	}
#endif

	checksum = sbi->ll_flags & LL_SBI_CHECKSUM;
	err = obd_set_info_async(NULL, sbi->ll_dt_exp, sizeof(KEY_CHECKSUM),
				 KEY_CHECKSUM, sizeof(checksum), &checksum,
				 NULL);
	cl_sb_init(sb);

	err = obd_set_info_async(NULL, sbi->ll_dt_exp, sizeof(KEY_CACHE_SET),
				 KEY_CACHE_SET, sizeof(*sbi->ll_cache),
				 sbi->ll_cache, NULL);

	sb->s_root = d_make_root(root);
	if (sb->s_root == NULL) {
		CERROR("%s: can't make root dentry\n",
			ll_get_fsname(sb, NULL, 0));
		GOTO(out_root, err = -ENOMEM);
	}
#ifdef HAVE_DCACHE_LOCK
	sb->s_root->d_op = &ll_d_ops;
#endif

	sbi->ll_sdev_orig = sb->s_dev;

	/* We set sb->s_dev equal on all lustre clients in order to support
	 * NFS export clustering.  NFSD requires that the FSID be the same
	 * on all clients. */
	/* s_dev is also used in lt_compare() to compare two fs, but that is
	 * only a node-local comparison. */
	uuid = obd_get_uuid(sbi->ll_md_exp);
	if (uuid != NULL)
		sb->s_dev = get_uuid2int(uuid->uuid, strlen(uuid->uuid));

	if (data != NULL)
		OBD_FREE_PTR(data);
	if (osfs != NULL)
		OBD_FREE_PTR(osfs);
	if (proc_lustre_fs_root != NULL) {
		err = lprocfs_register_mountpoint(proc_lustre_fs_root, sb,
						  dt, md);
		if (err < 0) {
			CERROR("%s: could not register mount in lprocfs: "
			       "rc = %d\n", ll_get_fsname(sb, NULL, 0), err);
			err = 0;
		}
	}

	RETURN(err);
out_root:
	if (root)
		iput(root);
out_lock_cn_cb:
	obd_fid_fini(sbi->ll_dt_exp->exp_obd);
out_dt:
	obd_disconnect(sbi->ll_dt_exp);
	sbi->ll_dt_exp = NULL;
out_md_fid:
	obd_fid_fini(sbi->ll_md_exp->exp_obd);
out_md:
	obd_disconnect(sbi->ll_md_exp);
	sbi->ll_md_exp = NULL;
out:
	if (data != NULL)
		OBD_FREE_PTR(data);
	if (osfs != NULL)
		OBD_FREE_PTR(osfs);
	return err;
}

int ll_get_max_mdsize(struct ll_sb_info *sbi, int *lmmsize)
{
	int size, rc;

	*lmmsize = obd_size_diskmd(sbi->ll_dt_exp, NULL);
	size = sizeof(int);
	rc = obd_get_info(NULL, sbi->ll_md_exp, sizeof(KEY_MAX_EASIZE),
			  KEY_MAX_EASIZE, &size, lmmsize, NULL);
	if (rc)
		CERROR("Get max mdsize error rc %d\n", rc);

	RETURN(rc);
}

/**
 * Get the value of the default_easize parameter.
 *
 * \see client_obd::cl_default_mds_easize
 *
 * \param[in] sbi	superblock info for this filesystem
 * \param[out] lmmsize	pointer to storage location for value
 *
 * \retval 0		on success
 * \retval negative	negated errno on failure
 */
int ll_get_default_mdsize(struct ll_sb_info *sbi, int *lmmsize)
{
	int size, rc;

	size = sizeof(int);
	rc = obd_get_info(NULL, sbi->ll_md_exp, sizeof(KEY_DEFAULT_EASIZE),
			 KEY_DEFAULT_EASIZE, &size, lmmsize, NULL);
	if (rc)
		CERROR("Get default mdsize error rc %d\n", rc);

	RETURN(rc);
}

/**
 * Set the default_easize parameter to the given value.
 *
 * \see client_obd::cl_default_mds_easize
 *
 * \param[in] sbi	superblock info for this filesystem
 * \param[in] lmmsize	the size to set
 *
 * \retval 0		on success
 * \retval negative	negated errno on failure
 */
int ll_set_default_mdsize(struct ll_sb_info *sbi, int lmmsize)
{
	int rc;

	if (lmmsize < sizeof(struct lov_mds_md) ||
	    lmmsize > OBD_MAX_DEFAULT_EA_SIZE)
		return -EINVAL;

	rc = obd_set_info_async(NULL, sbi->ll_md_exp,
				sizeof(KEY_DEFAULT_EASIZE), KEY_DEFAULT_EASIZE,
				sizeof(int), &lmmsize, NULL);

	RETURN(rc);
}

int ll_get_max_cookiesize(struct ll_sb_info *sbi, int *lmmsize)
{
	int size, rc;

	size = sizeof(int);
	rc = obd_get_info(NULL, sbi->ll_md_exp, sizeof(KEY_MAX_COOKIESIZE),
			  KEY_MAX_COOKIESIZE, &size, lmmsize, NULL);
	if (rc)
		CERROR("Get max cookiesize error rc %d\n", rc);

	RETURN(rc);
}

int ll_get_default_cookiesize(struct ll_sb_info *sbi, int *lmmsize)
{
	int size, rc;

	size = sizeof(int);
	rc = obd_get_info(NULL, sbi->ll_md_exp, sizeof(KEY_DEFAULT_COOKIESIZE),
			  KEY_DEFAULT_COOKIESIZE, &size, lmmsize, NULL);
	if (rc)
		CERROR("Get default cookiesize error rc %d\n", rc);

	RETURN(rc);
}

static void ll_dump_inode(struct inode *inode)
{
	struct ll_d_hlist_node *tmp;
	int dentry_count = 0;

	LASSERT(inode != NULL);

	ll_d_hlist_for_each(tmp, &inode->i_dentry)
		dentry_count++;

	CERROR("%s: inode %p dump: dev=%s fid="DFID
	       " mode=%o count=%u, %d dentries\n",
	       ll_get_fsname(inode->i_sb, NULL, 0), inode,
	       ll_i2mdexp(inode)->exp_obd->obd_name, PFID(ll_inode2fid(inode)),
	       inode->i_mode, atomic_read(&inode->i_count), dentry_count);
}

void lustre_dump_dentry(struct dentry *dentry, int recur)
{
        struct list_head *tmp;
        int subdirs = 0;

        LASSERT(dentry != NULL);

        list_for_each(tmp, &dentry->d_subdirs)
                subdirs++;

        CERROR("dentry %p dump: name=%.*s parent=%.*s (%p), inode=%p, count=%u,"
               " flags=0x%x, fsdata=%p, %d subdirs\n", dentry,
               dentry->d_name.len, dentry->d_name.name,
               dentry->d_parent->d_name.len, dentry->d_parent->d_name.name,
	       dentry->d_parent, dentry->d_inode, ll_d_count(dentry),
               dentry->d_flags, dentry->d_fsdata, subdirs);
        if (dentry->d_inode != NULL)
                ll_dump_inode(dentry->d_inode);

        if (recur == 0)
                return;

	list_for_each(tmp, &dentry->d_subdirs) {
		struct dentry *d = list_entry(tmp, struct dentry, d_child);
		lustre_dump_dentry(d, recur - 1);
	}
}

static void client_common_put_super(struct super_block *sb)
{
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        ENTRY;

#ifdef CONFIG_FS_POSIX_ACL
        if (sbi->ll_flags & LL_SBI_RMT_CLIENT) {
                et_fini(&sbi->ll_et);
                rct_fini(&sbi->ll_rct);
        }
#endif

        ll_close_thread_shutdown(sbi->ll_lcq);

        cl_sb_fini(sb);

	list_del(&sbi->ll_conn_chain);

	obd_fid_fini(sbi->ll_dt_exp->exp_obd);
        obd_disconnect(sbi->ll_dt_exp);
        sbi->ll_dt_exp = NULL;

        lprocfs_unregister_mountpoint(sbi);

	obd_fid_fini(sbi->ll_md_exp->exp_obd);
        obd_disconnect(sbi->ll_md_exp);
        sbi->ll_md_exp = NULL;

        EXIT;
}

void ll_kill_super(struct super_block *sb)
{
	struct ll_sb_info *sbi;
	ENTRY;

        /* not init sb ?*/
	if (!(sb->s_flags & MS_ACTIVE))
		return;

	sbi = ll_s2sbi(sb);
	/* we need restore s_dev from changed for clustred NFS before put_super
	 * because new kernels have cached s_dev and change sb->s_dev in
	 * put_super not affected real removing devices */
	if (sbi) {
		sb->s_dev = sbi->ll_sdev_orig;
		sbi->ll_umounting = 1;

		/* wait running statahead threads to quit */
		while (atomic_read(&sbi->ll_sa_running) > 0)
			schedule_timeout_and_set_state(TASK_UNINTERRUPTIBLE,
				msecs_to_jiffies(MSEC_PER_SEC >> 3));
	}

	EXIT;
}

static inline int ll_set_opt(const char *opt, char *data, int fl)
{
        if (strncmp(opt, data, strlen(opt)) != 0)
                return(0);
        else
                return(fl);
}

/* non-client-specific mount options are parsed in lmd_parse */
static int ll_options(char *options, int *flags)
{
        int tmp;
        char *s1 = options, *s2;
        ENTRY;

        if (!options)
                RETURN(0);

        CDEBUG(D_CONFIG, "Parsing opts %s\n", options);

        while (*s1) {
                CDEBUG(D_SUPER, "next opt=%s\n", s1);
                tmp = ll_set_opt("nolock", s1, LL_SBI_NOLCK);
                if (tmp) {
                        *flags |= tmp;
                        goto next;
                }
                tmp = ll_set_opt("flock", s1, LL_SBI_FLOCK);
                if (tmp) {
                        *flags |= tmp;
                        goto next;
                }
                tmp = ll_set_opt("localflock", s1, LL_SBI_LOCALFLOCK);
                if (tmp) {
                        *flags |= tmp;
                        goto next;
                }
                tmp = ll_set_opt("noflock", s1, LL_SBI_FLOCK|LL_SBI_LOCALFLOCK);
                if (tmp) {
                        *flags &= ~tmp;
                        goto next;
                }
                tmp = ll_set_opt("user_xattr", s1, LL_SBI_USER_XATTR);
                if (tmp) {
                        *flags |= tmp;
                        goto next;
                }
                tmp = ll_set_opt("nouser_xattr", s1, LL_SBI_USER_XATTR);
                if (tmp) {
                        *flags &= ~tmp;
                        goto next;
                }
		tmp = ll_set_opt("context", s1, 1);
		if (tmp)
			goto next;
		tmp = ll_set_opt("fscontext", s1, 1);
		if (tmp)
			goto next;
		tmp = ll_set_opt("defcontext", s1, 1);
		if (tmp)
			goto next;
		tmp = ll_set_opt("rootcontext", s1, 1);
		if (tmp)
			goto next;
		tmp = ll_set_opt("remote_client", s1, LL_SBI_RMT_CLIENT);
		if (tmp) {
			*flags |= tmp;
			goto next;
		}
		tmp = ll_set_opt("user_fid2path", s1, LL_SBI_USER_FID2PATH);
		if (tmp) {
			*flags |= tmp;
			goto next;
		}
		tmp = ll_set_opt("nouser_fid2path", s1, LL_SBI_USER_FID2PATH);
		if (tmp) {
			*flags &= ~tmp;
			goto next;
		}

                tmp = ll_set_opt("checksum", s1, LL_SBI_CHECKSUM);
                if (tmp) {
                        *flags |= tmp;
                        goto next;
                }
                tmp = ll_set_opt("nochecksum", s1, LL_SBI_CHECKSUM);
                if (tmp) {
                        *flags &= ~tmp;
                        goto next;
                }
                tmp = ll_set_opt("lruresize", s1, LL_SBI_LRU_RESIZE);
                if (tmp) {
                        *flags |= tmp;
                        goto next;
                }
                tmp = ll_set_opt("nolruresize", s1, LL_SBI_LRU_RESIZE);
                if (tmp) {
                        *flags &= ~tmp;
                        goto next;
                }
                tmp = ll_set_opt("lazystatfs", s1, LL_SBI_LAZYSTATFS);
                if (tmp) {
                        *flags |= tmp;
                        goto next;
                }
                tmp = ll_set_opt("nolazystatfs", s1, LL_SBI_LAZYSTATFS);
                if (tmp) {
                        *flags &= ~tmp;
                        goto next;
                }
                tmp = ll_set_opt("som_preview", s1, LL_SBI_SOM_PREVIEW);
                if (tmp) {
                        *flags |= tmp;
                        goto next;
                }
                tmp = ll_set_opt("32bitapi", s1, LL_SBI_32BIT_API);
                if (tmp) {
                        *flags |= tmp;
                        goto next;
                }
                tmp = ll_set_opt("verbose", s1, LL_SBI_VERBOSE);
                if (tmp) {
                        *flags |= tmp;
                        goto next;
                }
                tmp = ll_set_opt("noverbose", s1, LL_SBI_VERBOSE);
                if (tmp) {
                        *flags &= ~tmp;
                        goto next;
                }
                LCONSOLE_ERROR_MSG(0x152, "Unknown option '%s', won't mount.\n",
                                   s1);
                RETURN(-EINVAL);

next:
                /* Find next opt */
                s2 = strchr(s1, ',');
                if (s2 == NULL)
                        break;
                s1 = s2 + 1;
        }
        RETURN(0);
}

void ll_lli_init(struct ll_inode_info *lli)
{
	lli->lli_inode_magic = LLI_INODE_MAGIC;
	lli->lli_flags = 0;
	lli->lli_ioepoch = 0;
	lli->lli_maxbytes = MAX_LFS_FILESIZE;
	spin_lock_init(&lli->lli_lock);
	lli->lli_posix_acl = NULL;
	lli->lli_remote_perms = NULL;
	mutex_init(&lli->lli_rmtperm_mutex);
	/* Do not set lli_fid, it has been initialized already. */
	fid_zero(&lli->lli_pfid);
	INIT_LIST_HEAD(&lli->lli_close_list);
	INIT_LIST_HEAD(&lli->lli_oss_capas);
	atomic_set(&lli->lli_open_count, 0);
	lli->lli_mds_capa = NULL;
	lli->lli_rmtperm_time = 0;
	lli->lli_pending_och = NULL;
	lli->lli_mds_read_och = NULL;
        lli->lli_mds_write_och = NULL;
        lli->lli_mds_exec_och = NULL;
        lli->lli_open_fd_read_count = 0;
        lli->lli_open_fd_write_count = 0;
        lli->lli_open_fd_exec_count = 0;
	mutex_init(&lli->lli_och_mutex);
	spin_lock_init(&lli->lli_agl_lock);
	lli->lli_has_smd = false;
	spin_lock_init(&lli->lli_layout_lock);
	ll_layout_version_set(lli, LL_LAYOUT_GEN_NONE);
	lli->lli_clob = NULL;

	init_rwsem(&lli->lli_xattrs_list_rwsem);
	mutex_init(&lli->lli_xattrs_enq_lock);

	LASSERT(lli->lli_vfs_inode.i_mode != 0);
	if (S_ISDIR(lli->lli_vfs_inode.i_mode)) {
		mutex_init(&lli->lli_readdir_mutex);
		lli->lli_opendir_key = NULL;
		lli->lli_sai = NULL;
		spin_lock_init(&lli->lli_sa_lock);
		lli->lli_opendir_pid = 0;
		lli->lli_sa_enabled = 0;
		lli->lli_def_stripe_offset = -1;
	} else {
		mutex_init(&lli->lli_size_mutex);
		lli->lli_symlink_name = NULL;
		init_rwsem(&lli->lli_trunc_sem);
		range_lock_tree_init(&lli->lli_write_tree);
		init_rwsem(&lli->lli_glimpse_sem);
		lli->lli_glimpse_time = 0;
		INIT_LIST_HEAD(&lli->lli_agl_list);
		lli->lli_agl_index = 0;
		lli->lli_async_rc = 0;
	}
	mutex_init(&lli->lli_layout_mutex);
}

static inline int ll_bdi_register(struct backing_dev_info *bdi)
{
	static atomic_t ll_bdi_num = ATOMIC_INIT(0);

	bdi->name = "lustre";
	return bdi_register(bdi, NULL, "lustre-%d",
			    atomic_inc_return(&ll_bdi_num));
}

int ll_fill_super(struct super_block *sb, struct vfsmount *mnt)
{
        struct lustre_profile *lprof = NULL;
        struct lustre_sb_info *lsi = s2lsi(sb);
        struct ll_sb_info *sbi;
        char  *dt = NULL, *md = NULL;
        char  *profilenm = get_profile_name(sb);
        struct config_llog_instance *cfg;
        /* %p for void* in printf needs 16+2 characters: 0xffffffffffffffff */
        const int instlen = sizeof(cfg->cfg_instance) * 2 + 2;
        int    err;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op: sb %p\n", sb);

        OBD_ALLOC_PTR(cfg);
        if (cfg == NULL)
                RETURN(-ENOMEM);

	try_module_get(THIS_MODULE);

	/* client additional sb info */
	lsi->lsi_llsbi = sbi = ll_init_sbi();
	if (!sbi) {
		module_put(THIS_MODULE);
		OBD_FREE_PTR(cfg);
		RETURN(-ENOMEM);
	}

        err = ll_options(lsi->lsi_lmd->lmd_opts, &sbi->ll_flags);
        if (err)
                GOTO(out_free, err);

	err = bdi_init(&lsi->lsi_bdi);
	if (err)
		GOTO(out_free, err);
	lsi->lsi_flags |= LSI_BDI_INITIALIZED;
#ifdef HAVE_BDI_CAP_MAP_COPY
	lsi->lsi_bdi.capabilities = BDI_CAP_MAP_COPY;
#else
	lsi->lsi_bdi.capabilities = 0;
#endif
	err = ll_bdi_register(&lsi->lsi_bdi);
	if (err)
		GOTO(out_free, err);

        sb->s_bdi = &lsi->lsi_bdi;
#ifndef HAVE_DCACHE_LOCK
	/* kernel >= 2.6.38 store dentry operations in sb->s_d_op. */
	sb->s_d_op = &ll_d_ops;
#endif

        /* Generate a string unique to this super, in case some joker tries
           to mount the same fs at two mount points.
           Use the address of the super itself.*/
        cfg->cfg_instance = sb;
        cfg->cfg_uuid = lsi->lsi_llsbi->ll_sb_uuid;
	cfg->cfg_callback = class_config_llog_handler;
        /* set up client obds */
        err = lustre_process_log(sb, profilenm, cfg);
	if (err < 0)
		GOTO(out_free, err);

        /* Profile set with LCFG_MOUNTOPT so we can find our mdc and osc obds */
        lprof = class_get_profile(profilenm);
        if (lprof == NULL) {
                LCONSOLE_ERROR_MSG(0x156, "The client profile '%s' could not be"
                                   " read from the MGS.  Does that filesystem "
                                   "exist?\n", profilenm);
                GOTO(out_free, err = -EINVAL);
        }
        CDEBUG(D_CONFIG, "Found profile %s: mdc=%s osc=%s\n", profilenm,
               lprof->lp_md, lprof->lp_dt);

        OBD_ALLOC(dt, strlen(lprof->lp_dt) + instlen + 2);
        if (!dt)
                GOTO(out_free, err = -ENOMEM);
        sprintf(dt, "%s-%p", lprof->lp_dt, cfg->cfg_instance);

        OBD_ALLOC(md, strlen(lprof->lp_md) + instlen + 2);
        if (!md)
                GOTO(out_free, err = -ENOMEM);
        sprintf(md, "%s-%p", lprof->lp_md, cfg->cfg_instance);

        /* connections, registrations, sb setup */
        err = client_common_fill_super(sb, md, dt, mnt);

out_free:
        if (md)
                OBD_FREE(md, strlen(lprof->lp_md) + instlen + 2);
        if (dt)
                OBD_FREE(dt, strlen(lprof->lp_dt) + instlen + 2);
        if (err)
                ll_put_super(sb);
        else if (sbi->ll_flags & LL_SBI_VERBOSE)
                LCONSOLE_WARN("Mounted %s\n", profilenm);

        OBD_FREE_PTR(cfg);
        RETURN(err);
} /* ll_fill_super */

void ll_put_super(struct super_block *sb)
{
	struct config_llog_instance cfg, params_cfg;
        struct obd_device *obd;
        struct lustre_sb_info *lsi = s2lsi(sb);
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        char *profilenm = get_profile_name(sb);
	long ccc_count;
	int next, force = 1, rc = 0;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op: sb %p - %s\n", sb, profilenm);

        ll_print_capa_stat(sbi);

        cfg.cfg_instance = sb;
        lustre_end_log(sb, profilenm, &cfg);

	params_cfg.cfg_instance = sb;
	lustre_end_log(sb, PARAMS_FILENAME, &params_cfg);

        if (sbi->ll_md_exp) {
                obd = class_exp2obd(sbi->ll_md_exp);
                if (obd)
                        force = obd->obd_force;
        }

	/* Wait for unstable pages to be committed to stable storage */
	if (force == 0) {
		struct l_wait_info lwi = LWI_INTR(LWI_ON_SIGNAL_NOOP, NULL);
		rc = l_wait_event(sbi->ll_cache->ccc_unstable_waitq,
			atomic_long_read(&sbi->ll_cache->ccc_unstable_nr) == 0,
			&lwi);
	}

	ccc_count = atomic_long_read(&sbi->ll_cache->ccc_unstable_nr);
	if (force == 0 && rc != -EINTR)
		LASSERTF(ccc_count == 0, "count: %li\n", ccc_count);


        /* We need to set force before the lov_disconnect in
           lustre_common_put_super, since l_d cleans up osc's as well. */
        if (force) {
                next = 0;
                while ((obd = class_devices_in_group(&sbi->ll_sb_uuid,
                                                     &next)) != NULL) {
                        obd->obd_force = force;
                }
        }

        if (sbi->ll_lcq) {
                /* Only if client_common_fill_super succeeded */
                client_common_put_super(sb);
        }

        next = 0;
        while ((obd = class_devices_in_group(&sbi->ll_sb_uuid, &next)) !=NULL) {
                class_manual_cleanup(obd);
        }

        if (sbi->ll_flags & LL_SBI_VERBOSE)
                LCONSOLE_WARN("Unmounted %s\n", profilenm ? profilenm : "");

        if (profilenm)
                class_del_profile(profilenm);

	if (lsi->lsi_flags & LSI_BDI_INITIALIZED) {
		bdi_destroy(&lsi->lsi_bdi);
		lsi->lsi_flags &= ~LSI_BDI_INITIALIZED;
	}

        ll_free_sbi(sb);
        lsi->lsi_llsbi = NULL;

	lustre_common_put_super(sb);

	cl_env_cache_purge(~0);

	module_put(THIS_MODULE);

	EXIT;
} /* client_put_super */

struct inode *ll_inode_from_resource_lock(struct ldlm_lock *lock)
{
	struct inode *inode = NULL;

	/* NOTE: we depend on atomic igrab() -bzzz */
	lock_res_and_lock(lock);
	if (lock->l_resource->lr_lvb_inode) {
		struct ll_inode_info * lli;
		lli = ll_i2info(lock->l_resource->lr_lvb_inode);
		if (lli->lli_inode_magic == LLI_INODE_MAGIC) {
			inode = igrab(lock->l_resource->lr_lvb_inode);
		} else {
			inode = lock->l_resource->lr_lvb_inode;
			LDLM_DEBUG_LIMIT(inode->i_state & I_FREEING ?  D_INFO :
					 D_WARNING, lock, "lr_lvb_inode %p is "
					 "bogus: magic %08x",
					 lock->l_resource->lr_lvb_inode,
					 lli->lli_inode_magic);
			inode = NULL;
		}
	}
	unlock_res_and_lock(lock);
	return inode;
}

static void ll_dir_clear_lsm_md(struct inode *inode)
{
	struct ll_inode_info *lli = ll_i2info(inode);

	LASSERT(S_ISDIR(inode->i_mode));

	if (lli->lli_lsm_md != NULL) {
		lmv_free_memmd(lli->lli_lsm_md);
		lli->lli_lsm_md = NULL;
	}
}

static struct inode *ll_iget_anon_dir(struct super_block *sb,
				      const struct lu_fid *fid,
				      struct lustre_md *md)
{
	struct ll_sb_info	*sbi = ll_s2sbi(sb);
	struct mdt_body		*body = md->body;
	struct inode		*inode;
	ino_t			ino;
	ENTRY;

	ino = cl_fid_build_ino(fid, sbi->ll_flags & LL_SBI_32BIT_API);
	inode = iget_locked(sb, ino);
	if (inode == NULL) {
		CERROR("%s: failed get simple inode "DFID": rc = -ENOENT\n",
		       ll_get_fsname(sb, NULL, 0), PFID(fid));
		RETURN(ERR_PTR(-ENOENT));
	}

	if (inode->i_state & I_NEW) {
		struct ll_inode_info *lli = ll_i2info(inode);
		struct lmv_stripe_md *lsm = md->lmv;

		inode->i_mode = (inode->i_mode & ~S_IFMT) |
				(body->mbo_mode & S_IFMT);
		LASSERTF(S_ISDIR(inode->i_mode), "Not slave inode "DFID"\n",
			 PFID(fid));

		LTIME_S(inode->i_mtime) = 0;
		LTIME_S(inode->i_atime) = 0;
		LTIME_S(inode->i_ctime) = 0;
		inode->i_rdev = 0;

#ifdef HAVE_BACKING_DEV_INFO
		/* initializing backing dev info. */
		inode->i_mapping->backing_dev_info =
						&s2lsi(inode->i_sb)->lsi_bdi;
#endif
		inode->i_op = &ll_dir_inode_operations;
		inode->i_fop = &ll_dir_operations;
		lli->lli_fid = *fid;
		ll_lli_init(lli);

		LASSERT(lsm != NULL);
		/* master object FID */
		lli->lli_pfid = body->mbo_fid1;
		CDEBUG(D_INODE, "lli %p slave "DFID" master "DFID"\n",
		       lli, PFID(fid), PFID(&lli->lli_pfid));
		unlock_new_inode(inode);
	}

	RETURN(inode);
}

static int ll_init_lsm_md(struct inode *inode, struct lustre_md *md)
{
	struct lu_fid *fid;
	struct lmv_stripe_md *lsm = md->lmv;
	int i;

	LASSERT(lsm != NULL);
	/* XXX sigh, this lsm_root initialization should be in
	 * LMV layer, but it needs ll_iget right now, so we
	 * put this here right now. */
	for (i = 0; i < lsm->lsm_md_stripe_count; i++) {
		fid = &lsm->lsm_md_oinfo[i].lmo_fid;
		LASSERT(lsm->lsm_md_oinfo[i].lmo_root == NULL);
		/* Unfortunately ll_iget will call ll_update_inode,
		 * where the initialization of slave inode is slightly
		 * different, so it reset lsm_md to NULL to avoid
		 * initializing lsm for slave inode. */
		/* For migrating inode, master stripe and master object will
		 * be same, so we only need assign this inode */
		if (lsm->lsm_md_hash_type & LMV_HASH_FLAG_MIGRATION && i == 0)
			lsm->lsm_md_oinfo[i].lmo_root = inode;
		else
			lsm->lsm_md_oinfo[i].lmo_root =
				ll_iget_anon_dir(inode->i_sb, fid, md);

		if (IS_ERR(lsm->lsm_md_oinfo[i].lmo_root)) {
			int rc = PTR_ERR(lsm->lsm_md_oinfo[i].lmo_root);

			lsm->lsm_md_oinfo[i].lmo_root = NULL;
			return rc;
		}
	}

	return 0;
}

static inline int lli_lsm_md_eq(const struct lmv_stripe_md *lsm_md1,
				const struct lmv_stripe_md *lsm_md2)
{
	return lsm_md1->lsm_md_magic == lsm_md2->lsm_md_magic &&
	       lsm_md1->lsm_md_stripe_count == lsm_md2->lsm_md_stripe_count &&
	       lsm_md1->lsm_md_master_mdt_index ==
					lsm_md2->lsm_md_master_mdt_index &&
	       lsm_md1->lsm_md_hash_type == lsm_md2->lsm_md_hash_type &&
	       lsm_md1->lsm_md_layout_version ==
					lsm_md2->lsm_md_layout_version &&
	       strcmp(lsm_md1->lsm_md_pool_name,
		      lsm_md2->lsm_md_pool_name) == 0;
}

static int ll_update_lsm_md(struct inode *inode, struct lustre_md *md)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	struct lmv_stripe_md *lsm = md->lmv;
	int	rc;
	ENTRY;

	LASSERT(S_ISDIR(inode->i_mode));
	CDEBUG(D_INODE, "update lsm %p of "DFID"\n", lli->lli_lsm_md,
	       PFID(ll_inode2fid(inode)));

	/* no striped information from request. */
	if (lsm == NULL) {
		if (lli->lli_lsm_md == NULL) {
			RETURN(0);
		} else if (lli->lli_lsm_md->lsm_md_hash_type &
						LMV_HASH_FLAG_MIGRATION) {
			/* migration is done, the temporay MIGRATE layout has
			 * been removed */
			CDEBUG(D_INODE, DFID" finish migration.\n",
			       PFID(ll_inode2fid(inode)));
			lmv_free_memmd(lli->lli_lsm_md);
			lli->lli_lsm_md = NULL;
			RETURN(0);
		} else {
			/* The lustre_md from req does not include stripeEA,
			 * see ll_md_setattr */
			RETURN(0);
		}
	}

	/* set the directory layout */
	if (lli->lli_lsm_md == NULL) {

		rc = ll_init_lsm_md(inode, md);
		if (rc != 0)
			RETURN(rc);

		lli->lli_lsm_md = lsm;
		/* set lsm_md to NULL, so the following free lustre_md
		 * will not free this lsm */
		md->lmv = NULL;
		CDEBUG(D_INODE, "Set lsm %p magic %x to "DFID"\n", lsm,
		       lsm->lsm_md_magic, PFID(ll_inode2fid(inode)));
		RETURN(0);
	}

	/* Compare the old and new stripe information */
	if (!lsm_md_eq(lli->lli_lsm_md, lsm)) {
		struct lmv_stripe_md	*old_lsm = lli->lli_lsm_md;
		int			idx;

		CERROR("%s: inode "DFID"(%p)'s lmv layout mismatch (%p)/(%p)"
		       "magic:0x%x/0x%x stripe count: %d/%d master_mdt: %d/%d"
		       "hash_type:0x%x/0x%x layout: 0x%x/0x%x pool:%s/%s\n",
		       ll_get_fsname(inode->i_sb, NULL, 0), PFID(&lli->lli_fid),
		       inode, lsm, old_lsm,
		       lsm->lsm_md_magic, old_lsm->lsm_md_magic,
		       lsm->lsm_md_stripe_count,
		       old_lsm->lsm_md_stripe_count,
		       lsm->lsm_md_master_mdt_index,
		       old_lsm->lsm_md_master_mdt_index,
		       lsm->lsm_md_hash_type, old_lsm->lsm_md_hash_type,
		       lsm->lsm_md_layout_version,
		       old_lsm->lsm_md_layout_version,
		       lsm->lsm_md_pool_name,
		       old_lsm->lsm_md_pool_name);

		for (idx = 0; idx < old_lsm->lsm_md_stripe_count; idx++) {
			CERROR("%s: sub FIDs in old lsm idx %d, old: "DFID"\n",
			       ll_get_fsname(inode->i_sb, NULL, 0), idx,
			       PFID(&old_lsm->lsm_md_oinfo[idx].lmo_fid));
		}

		for (idx = 0; idx < lsm->lsm_md_stripe_count; idx++) {
			CERROR("%s: sub FIDs in new lsm idx %d, new: "DFID"\n",
			       ll_get_fsname(inode->i_sb, NULL, 0), idx,
			       PFID(&lsm->lsm_md_oinfo[idx].lmo_fid));
		}

		RETURN(-EIO);
	}

	RETURN(0);
}

void ll_clear_inode(struct inode *inode)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:inode="DFID"(%p)\n",
	       PFID(ll_inode2fid(inode)), inode);

        if (S_ISDIR(inode->i_mode)) {
                /* these should have been cleared in ll_file_release */
                LASSERT(lli->lli_opendir_key == NULL);
                LASSERT(lli->lli_sai == NULL);
                LASSERT(lli->lli_opendir_pid == 0);
        }

	spin_lock(&lli->lli_lock);
        ll_i2info(inode)->lli_flags &= ~LLIF_MDS_SIZE_LOCK;
	spin_unlock(&lli->lli_lock);
	md_null_inode(sbi->ll_md_exp, ll_inode2fid(inode));

        LASSERT(!lli->lli_open_fd_write_count);
        LASSERT(!lli->lli_open_fd_read_count);
        LASSERT(!lli->lli_open_fd_exec_count);

        if (lli->lli_mds_write_och)
                ll_md_real_close(inode, FMODE_WRITE);
        if (lli->lli_mds_exec_och)
                ll_md_real_close(inode, FMODE_EXEC);
        if (lli->lli_mds_read_och)
                ll_md_real_close(inode, FMODE_READ);

        if (S_ISLNK(inode->i_mode) && lli->lli_symlink_name) {
                OBD_FREE(lli->lli_symlink_name,
                         strlen(lli->lli_symlink_name) + 1);
                lli->lli_symlink_name = NULL;
        }

	ll_xattr_cache_destroy(inode);

	if (sbi->ll_flags & LL_SBI_RMT_CLIENT) {
		LASSERT(lli->lli_posix_acl == NULL);
		if (lli->lli_remote_perms) {
			free_rmtperm_hash(lli->lli_remote_perms);
			lli->lli_remote_perms = NULL;
		}
	}
#ifdef CONFIG_FS_POSIX_ACL
	else if (lli->lli_posix_acl) {
		LASSERT(atomic_read(&lli->lli_posix_acl->a_refcount) == 1);
		LASSERT(lli->lli_remote_perms == NULL);
		posix_acl_release(lli->lli_posix_acl);
		lli->lli_posix_acl = NULL;
	}
#endif
	lli->lli_inode_magic = LLI_INODE_DEAD;

	ll_clear_inode_capas(inode);
	if (S_ISDIR(inode->i_mode))
		ll_dir_clear_lsm_md(inode);
	else if (S_ISREG(inode->i_mode) && !is_bad_inode(inode))
		LASSERT(list_empty(&lli->lli_agl_list));

	/*
	 * XXX This has to be done before lsm is freed below, because
	 * cl_object still uses inode lsm.
	 */
	cl_inode_fini(inode);
	lli->lli_has_smd = false;

	EXIT;
}

static int ll_md_setattr(struct dentry *dentry, struct md_op_data *op_data,
			 struct md_open_data **mod)
{
        struct lustre_md md;
        struct inode *inode = dentry->d_inode;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ptlrpc_request *request = NULL;
        int rc, ia_valid;
        ENTRY;

        op_data = ll_prep_md_op_data(op_data, inode, NULL, NULL, 0, 0,
                                     LUSTRE_OPC_ANY, NULL);
        if (IS_ERR(op_data))
                RETURN(PTR_ERR(op_data));

        rc = md_setattr(sbi->ll_md_exp, op_data, NULL, 0, NULL, 0,
                        &request, mod);
	if (rc) {
		ptlrpc_req_finished(request);
		if (rc == -ENOENT) {
			clear_nlink(inode);
			/* Unlinked special device node? Or just a race?
			 * Pretend we done everything. */
			if (!S_ISREG(inode->i_mode) &&
			    !S_ISDIR(inode->i_mode)) {
				ia_valid = op_data->op_attr.ia_valid;
				op_data->op_attr.ia_valid &= ~TIMES_SET_FLAGS;
				rc = simple_setattr(dentry, &op_data->op_attr);
				op_data->op_attr.ia_valid = ia_valid;
			}
		} else if (rc != -EPERM && rc != -EACCES && rc != -ETXTBSY) {
			CERROR("md_setattr fails: rc = %d\n", rc);
		}
		RETURN(rc);
	}

        rc = md_get_lustre_md(sbi->ll_md_exp, request, sbi->ll_dt_exp,
                              sbi->ll_md_exp, &md);
        if (rc) {
                ptlrpc_req_finished(request);
                RETURN(rc);
        }

	ia_valid = op_data->op_attr.ia_valid;
	/* inode size will be in ll_setattr_ost, can't do it now since dirty
	 * cache is not cleared yet. */
	op_data->op_attr.ia_valid &= ~(TIMES_SET_FLAGS | ATTR_SIZE);
	rc = simple_setattr(dentry, &op_data->op_attr);
	op_data->op_attr.ia_valid = ia_valid;

        /* Extract epoch data if obtained. */
	op_data->op_handle = md.body->mbo_handle;
	op_data->op_ioepoch = md.body->mbo_ioepoch;

	rc = ll_update_inode(inode, &md);
	ptlrpc_req_finished(request);

	RETURN(rc);
}

/* Close IO epoch and send Size-on-MDS attribute update. */
static int ll_setattr_done_writing(struct inode *inode,
                                   struct md_op_data *op_data,
                                   struct md_open_data *mod)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        int rc = 0;
        ENTRY;

        LASSERT(op_data != NULL);
        if (!S_ISREG(inode->i_mode))
                RETURN(0);

        CDEBUG(D_INODE, "Epoch "LPU64" closed on "DFID" for truncate\n",
               op_data->op_ioepoch, PFID(&lli->lli_fid));

        op_data->op_flags = MF_EPOCH_CLOSE;
        ll_done_writing_attr(inode, op_data);
        ll_pack_inode2opdata(inode, op_data, NULL);

        rc = md_done_writing(ll_i2sbi(inode)->ll_md_exp, op_data, mod);
        if (rc == -EAGAIN) {
                /* MDS has instructed us to obtain Size-on-MDS attribute
                 * from OSTs and send setattr to back to MDS. */
                rc = ll_som_update(inode, op_data);
        } else if (rc) {
		CERROR("%s: inode "DFID" mdc truncate failed: rc = %d\n",
		       ll_i2sbi(inode)->ll_md_exp->exp_obd->obd_name,
		       PFID(ll_inode2fid(inode)), rc);
        }
        RETURN(rc);
}

static int ll_setattr_ost(struct inode *inode, struct iattr *attr)
{
        struct obd_capa *capa;
        int rc;

        if (attr->ia_valid & ATTR_SIZE)
                capa = ll_osscapa_get(inode, CAPA_OPC_OSS_TRUNC);
        else
                capa = ll_mdscapa_get(inode);

        rc = cl_setattr_ost(inode, attr, capa);

        if (attr->ia_valid & ATTR_SIZE)
                ll_truncate_free_capa(capa);
        else
                capa_put(capa);

        return rc;
}

/* If this inode has objects allocated to it (lsm != NULL), then the OST
 * object(s) determine the file size and mtime.  Otherwise, the MDS will
 * keep these values until such a time that objects are allocated for it.
 * We do the MDS operations first, as it is checking permissions for us.
 * We don't to the MDS RPC if there is nothing that we want to store there,
 * otherwise there is no harm in updating mtime/atime on the MDS if we are
 * going to do an RPC anyways.
 *
 * If we are doing a truncate, we will send the mtime and ctime updates
 * to the OST with the punch RPC, otherwise we do an explicit setattr RPC.
 * I don't believe it is possible to get e.g. ATTR_MTIME_SET and ATTR_SIZE
 * at the same time.
 *
 * In case of HSMimport, we only set attr on MDS.
 */
int ll_setattr_raw(struct dentry *dentry, struct iattr *attr, bool hsm_import)
{
        struct inode *inode = dentry->d_inode;
        struct ll_inode_info *lli = ll_i2info(inode);
        struct md_op_data *op_data = NULL;
        struct md_open_data *mod = NULL;
	bool file_is_released = false;
	int rc = 0, rc1 = 0;
	ENTRY;

	CDEBUG(D_VFSTRACE, "%s: setattr inode "DFID"(%p) from %llu to %llu, "
	       "valid %x, hsm_import %d\n",
	       ll_get_fsname(inode->i_sb, NULL, 0), PFID(&lli->lli_fid),
	       inode, i_size_read(inode), attr->ia_size, attr->ia_valid,
	       hsm_import);

	if (attr->ia_valid & ATTR_SIZE) {
                /* Check new size against VFS/VM file size limit and rlimit */
                rc = inode_newsize_ok(inode, attr->ia_size);
                if (rc)
                        RETURN(rc);

                /* The maximum Lustre file size is variable, based on the
                 * OST maximum object size and number of stripes.  This
                 * needs another check in addition to the VFS check above. */
                if (attr->ia_size > ll_file_maxbytes(inode)) {
                        CDEBUG(D_INODE,"file "DFID" too large %llu > "LPU64"\n",
                               PFID(&lli->lli_fid), attr->ia_size,
                               ll_file_maxbytes(inode));
                        RETURN(-EFBIG);
                }

                attr->ia_valid |= ATTR_MTIME | ATTR_CTIME;
        }

	/* POSIX: check before ATTR_*TIME_SET set (from inode_change_ok) */
	if (attr->ia_valid & TIMES_SET_FLAGS) {
		if ((!uid_eq(current_fsuid(), inode->i_uid)) &&
		    !cfs_capable(CFS_CAP_FOWNER))
			RETURN(-EPERM);
	}

        /* We mark all of the fields "set" so MDS/OST does not re-set them */
        if (attr->ia_valid & ATTR_CTIME) {
                attr->ia_ctime = CFS_CURRENT_TIME;
                attr->ia_valid |= ATTR_CTIME_SET;
        }
	if (!(attr->ia_valid & ATTR_ATIME_SET) &&
	    (attr->ia_valid & ATTR_ATIME)) {
                attr->ia_atime = CFS_CURRENT_TIME;
                attr->ia_valid |= ATTR_ATIME_SET;
        }
	if (!(attr->ia_valid & ATTR_MTIME_SET) &&
	    (attr->ia_valid & ATTR_MTIME)) {
                attr->ia_mtime = CFS_CURRENT_TIME;
                attr->ia_valid |= ATTR_MTIME_SET;
        }

        if (attr->ia_valid & (ATTR_MTIME | ATTR_CTIME))
                CDEBUG(D_INODE, "setting mtime %lu, ctime %lu, now = %lu\n",
                       LTIME_S(attr->ia_mtime), LTIME_S(attr->ia_ctime),
                       cfs_time_current_sec());

        /* We always do an MDS RPC, even if we're only changing the size;
         * only the MDS knows whether truncate() should fail with -ETXTBUSY */

        OBD_ALLOC_PTR(op_data);
        if (op_data == NULL)
                RETURN(-ENOMEM);

	if (!S_ISDIR(inode->i_mode)) {
		if (attr->ia_valid & ATTR_SIZE)
			inode_dio_write_done(inode);
		mutex_unlock(&inode->i_mutex);
	}

	/* truncate on a released file must failed with -ENODATA,
	 * so size must not be set on MDS for released file
	 * but other attributes must be set
	 */
	if (S_ISREG(inode->i_mode)) {
		struct lov_stripe_md *lsm;
		__u32 gen;

		ll_layout_refresh(inode, &gen);
		lsm = ccc_inode_lsm_get(inode);
		if (lsm && lsm->lsm_pattern & LOV_PATTERN_F_RELEASED)
			file_is_released = true;
		ccc_inode_lsm_put(inode, lsm);

		if (!hsm_import && attr->ia_valid & ATTR_SIZE) {
			if (file_is_released) {
				rc = ll_layout_restore(inode, 0, attr->ia_size);
				if (rc < 0)
					GOTO(out, rc);

				file_is_released = false;
				ll_layout_refresh(inode, &gen);
			}

			/* If we are changing file size, file content is
			 * modified, flag it. */
			attr->ia_valid |= MDS_OPEN_OWNEROVERRIDE;
			spin_lock(&lli->lli_lock);
			lli->lli_flags |= LLIF_DATA_MODIFIED;
			spin_unlock(&lli->lli_lock);
			op_data->op_bias |= MDS_DATA_MODIFIED;
		}
	}

	memcpy(&op_data->op_attr, attr, sizeof(*attr));

	/* Open epoch for truncate. */
	if (exp_connect_som(ll_i2mdexp(inode)) && !hsm_import &&
	    (attr->ia_valid & (ATTR_SIZE | ATTR_MTIME | ATTR_MTIME_SET)))
		op_data->op_flags = MF_EPOCH_OPEN;

	rc = ll_md_setattr(dentry, op_data, &mod);
	if (rc)
		GOTO(out, rc);

	/* RPC to MDT is sent, cancel data modification flag */
	if (rc == 0 && (op_data->op_bias & MDS_DATA_MODIFIED)) {
		spin_lock(&lli->lli_lock);
		lli->lli_flags &= ~LLIF_DATA_MODIFIED;
		spin_unlock(&lli->lli_lock);
	}

	ll_ioepoch_open(lli, op_data->op_ioepoch);
	if (!S_ISREG(inode->i_mode) || file_is_released)
		GOTO(out, rc = 0);

	if (attr->ia_valid & (ATTR_SIZE |
			      ATTR_ATIME | ATTR_ATIME_SET |
			      ATTR_MTIME | ATTR_MTIME_SET)) {
		/* For truncate and utimes sending attributes to OSTs, setting
		 * mtime/atime to the past will be performed under PW [0:EOF]
		 * extent lock (new_size:EOF for truncate).  It may seem
		 * excessive to send mtime/atime updates to OSTs when not
		 * setting times to past, but it is necessary due to possible
		 * time de-synchronization between MDT inode and OST objects */
		if (attr->ia_valid & ATTR_SIZE)
			down_write(&lli->lli_trunc_sem);
		rc = ll_setattr_ost(inode, attr);
		if (attr->ia_valid & ATTR_SIZE)
			up_write(&lli->lli_trunc_sem);
	}
	EXIT;
out:
	if (op_data) {
		if (op_data->op_ioepoch) {
			rc1 = ll_setattr_done_writing(inode, op_data, mod);
			if (!rc)
				rc = rc1;
		}
		ll_finish_md_op_data(op_data);
	}
	if (!S_ISDIR(inode->i_mode)) {
		mutex_lock(&inode->i_mutex);
		if ((attr->ia_valid & ATTR_SIZE) && !hsm_import)
			inode_dio_wait(inode);
	}

	ll_stats_ops_tally(ll_i2sbi(inode), (attr->ia_valid & ATTR_SIZE) ?
			LPROC_LL_TRUNC : LPROC_LL_SETATTR, 1);

	return rc;
}

int ll_setattr(struct dentry *de, struct iattr *attr)
{
	int mode = de->d_inode->i_mode;

	if ((attr->ia_valid & (ATTR_CTIME|ATTR_SIZE|ATTR_MODE)) ==
			      (ATTR_CTIME|ATTR_SIZE|ATTR_MODE))
		attr->ia_valid |= MDS_OPEN_OWNEROVERRIDE;

	if (((attr->ia_valid & (ATTR_MODE|ATTR_FORCE|ATTR_SIZE)) ==
			       (ATTR_SIZE|ATTR_MODE)) &&
	    (((mode & S_ISUID) && !(attr->ia_mode & S_ISUID)) ||
	     (((mode & (S_ISGID|S_IXGRP)) == (S_ISGID|S_IXGRP)) &&
	      !(attr->ia_mode & S_ISGID))))
		attr->ia_valid |= ATTR_FORCE;

	if ((attr->ia_valid & ATTR_MODE) &&
	    (mode & S_ISUID) &&
	    !(attr->ia_mode & S_ISUID) &&
	    !(attr->ia_valid & ATTR_KILL_SUID))
		attr->ia_valid |= ATTR_KILL_SUID;

	if ((attr->ia_valid & ATTR_MODE) &&
	    ((mode & (S_ISGID|S_IXGRP)) == (S_ISGID|S_IXGRP)) &&
	    !(attr->ia_mode & S_ISGID) &&
	    !(attr->ia_valid & ATTR_KILL_SGID))
		attr->ia_valid |= ATTR_KILL_SGID;

	return ll_setattr_raw(de, attr, false);
}

int ll_statfs_internal(struct super_block *sb, struct obd_statfs *osfs,
                       __u64 max_age, __u32 flags)
{
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        struct obd_statfs obd_osfs;
        int rc;
        ENTRY;

        rc = obd_statfs(NULL, sbi->ll_md_exp, osfs, max_age, flags);
        if (rc) {
                CERROR("md_statfs fails: rc = %d\n", rc);
                RETURN(rc);
        }

        osfs->os_type = sb->s_magic;

        CDEBUG(D_SUPER, "MDC blocks "LPU64"/"LPU64" objects "LPU64"/"LPU64"\n",
               osfs->os_bavail, osfs->os_blocks, osfs->os_ffree,osfs->os_files);

        if (sbi->ll_flags & LL_SBI_LAZYSTATFS)
                flags |= OBD_STATFS_NODELAY;

        rc = obd_statfs_rqset(sbi->ll_dt_exp, &obd_osfs, max_age, flags);
        if (rc) {
                CERROR("obd_statfs fails: rc = %d\n", rc);
                RETURN(rc);
        }

        CDEBUG(D_SUPER, "OSC blocks "LPU64"/"LPU64" objects "LPU64"/"LPU64"\n",
               obd_osfs.os_bavail, obd_osfs.os_blocks, obd_osfs.os_ffree,
               obd_osfs.os_files);

        osfs->os_bsize = obd_osfs.os_bsize;
        osfs->os_blocks = obd_osfs.os_blocks;
        osfs->os_bfree = obd_osfs.os_bfree;
        osfs->os_bavail = obd_osfs.os_bavail;

        /* If we don't have as many objects free on the OST as inodes
         * on the MDS, we reduce the total number of inodes to
         * compensate, so that the "inodes in use" number is correct.
         */
        if (obd_osfs.os_ffree < osfs->os_ffree) {
                osfs->os_files = (osfs->os_files - osfs->os_ffree) +
                        obd_osfs.os_ffree;
                osfs->os_ffree = obd_osfs.os_ffree;
        }

        RETURN(rc);
}
int ll_statfs(struct dentry *de, struct kstatfs *sfs)
{
	struct super_block *sb = de->d_sb;
	struct obd_statfs osfs;
	__u64 fsid = huge_encode_dev(sb->s_dev);
	int rc;

        CDEBUG(D_VFSTRACE, "VFS Op: at "LPU64" jiffies\n", get_jiffies_64());
        ll_stats_ops_tally(ll_s2sbi(sb), LPROC_LL_STAFS, 1);

        /* Some amount of caching on the client is allowed */
        rc = ll_statfs_internal(sb, &osfs,
                                cfs_time_shift_64(-OBD_STATFS_CACHE_SECONDS),
                                0);
        if (rc)
                return rc;

        statfs_unpack(sfs, &osfs);

        /* We need to downshift for all 32-bit kernels, because we can't
         * tell if the kernel is being called via sys_statfs64() or not.
         * Stop before overflowing f_bsize - in which case it is better
         * to just risk EOVERFLOW if caller is using old sys_statfs(). */
        if (sizeof(long) < 8) {
                while (osfs.os_blocks > ~0UL && sfs->f_bsize < 0x40000000) {
                        sfs->f_bsize <<= 1;

                        osfs.os_blocks >>= 1;
                        osfs.os_bfree >>= 1;
                        osfs.os_bavail >>= 1;
                }
        }

        sfs->f_blocks = osfs.os_blocks;
        sfs->f_bfree = osfs.os_bfree;
        sfs->f_bavail = osfs.os_bavail;
	sfs->f_fsid.val[0] = (__u32)fsid;
	sfs->f_fsid.val[1] = (__u32)(fsid >> 32);
	return 0;
}

void ll_inode_size_lock(struct inode *inode)
{
	struct ll_inode_info *lli;

	LASSERT(!S_ISDIR(inode->i_mode));

	lli = ll_i2info(inode);
	mutex_lock(&lli->lli_size_mutex);
}

void ll_inode_size_unlock(struct inode *inode)
{
	struct ll_inode_info *lli;

	lli = ll_i2info(inode);
	mutex_unlock(&lli->lli_size_mutex);
}

int ll_update_inode(struct inode *inode, struct lustre_md *md)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	struct mdt_body *body = md->body;
	struct lov_stripe_md *lsm = md->lsm;
	struct ll_sb_info *sbi = ll_i2sbi(inode);

	LASSERT((lsm != NULL) == ((body->mbo_valid & OBD_MD_FLEASIZE) != 0));
	if (lsm != NULL) {
		if (!lli->lli_has_smd &&
		    !(sbi->ll_flags & LL_SBI_LAYOUT_LOCK))
			cl_file_inode_init(inode, md);

		lli->lli_maxbytes = lsm->lsm_maxbytes;
		if (lli->lli_maxbytes > MAX_LFS_FILESIZE)
			lli->lli_maxbytes = MAX_LFS_FILESIZE;
	}

	if (S_ISDIR(inode->i_mode)) {
		int	rc;

		rc = ll_update_lsm_md(inode, md);
		if (rc != 0)
			return rc;
	}

	if (sbi->ll_flags & LL_SBI_RMT_CLIENT) {
		if (body->mbo_valid & OBD_MD_FLRMTPERM)
			ll_update_remote_perm(inode, md->remote_perm);
	}
#ifdef CONFIG_FS_POSIX_ACL
	else if (body->mbo_valid & OBD_MD_FLACL) {
		spin_lock(&lli->lli_lock);
		if (lli->lli_posix_acl)
			posix_acl_release(lli->lli_posix_acl);
		lli->lli_posix_acl = md->posix_acl;
		spin_unlock(&lli->lli_lock);
	}
#endif
	inode->i_ino = cl_fid_build_ino(&body->mbo_fid1,
					sbi->ll_flags & LL_SBI_32BIT_API);
	inode->i_generation = cl_fid_build_gen(&body->mbo_fid1);

	if (body->mbo_valid & OBD_MD_FLATIME) {
		if (body->mbo_atime > LTIME_S(inode->i_atime))
			LTIME_S(inode->i_atime) = body->mbo_atime;
		lli->lli_atime = body->mbo_atime;
	}

	if (body->mbo_valid & OBD_MD_FLMTIME) {
		if (body->mbo_mtime > LTIME_S(inode->i_mtime)) {
			CDEBUG(D_INODE, "setting ino %lu mtime from %lu "
			       "to "LPU64"\n", inode->i_ino,
			       LTIME_S(inode->i_mtime), body->mbo_mtime);
			LTIME_S(inode->i_mtime) = body->mbo_mtime;
		}
		lli->lli_mtime = body->mbo_mtime;
	}

	if (body->mbo_valid & OBD_MD_FLCTIME) {
		if (body->mbo_ctime > LTIME_S(inode->i_ctime))
			LTIME_S(inode->i_ctime) = body->mbo_ctime;
		lli->lli_ctime = body->mbo_ctime;
	}

	if (body->mbo_valid & OBD_MD_FLMODE)
		inode->i_mode = (inode->i_mode & S_IFMT) |
				(body->mbo_mode & ~S_IFMT);

	if (body->mbo_valid & OBD_MD_FLTYPE)
		inode->i_mode = (inode->i_mode & ~S_IFMT) |
				(body->mbo_mode & S_IFMT);

	LASSERT(inode->i_mode != 0);
	if (S_ISREG(inode->i_mode))
		inode->i_blkbits = min(PTLRPC_MAX_BRW_BITS + 1,
				       LL_MAX_BLKSIZE_BITS);
	else
		inode->i_blkbits = inode->i_sb->s_blocksize_bits;

	if (body->mbo_valid & OBD_MD_FLUID)
		inode->i_uid = make_kuid(&init_user_ns, body->mbo_uid);
	if (body->mbo_valid & OBD_MD_FLGID)
		inode->i_gid = make_kgid(&init_user_ns, body->mbo_gid);
	if (body->mbo_valid & OBD_MD_FLFLAGS)
		inode->i_flags = ll_ext_to_inode_flags(body->mbo_flags);
	if (body->mbo_valid & OBD_MD_FLNLINK)
		set_nlink(inode, body->mbo_nlink);
	if (body->mbo_valid & OBD_MD_FLRDEV)
		inode->i_rdev = old_decode_dev(body->mbo_rdev);

	if (body->mbo_valid & OBD_MD_FLID) {
		/* FID shouldn't be changed! */
		if (fid_is_sane(&lli->lli_fid)) {
			LASSERTF(lu_fid_eq(&lli->lli_fid, &body->mbo_fid1),
				 "Trying to change FID "DFID
				 " to the "DFID", inode "DFID"(%p)\n",
				 PFID(&lli->lli_fid), PFID(&body->mbo_fid1),
				 PFID(ll_inode2fid(inode)), inode);
		} else {
			lli->lli_fid = body->mbo_fid1;
		}
	}

	LASSERT(fid_seq(&lli->lli_fid) != 0);

	if (body->mbo_valid & OBD_MD_FLSIZE) {
                if (exp_connect_som(ll_i2mdexp(inode)) &&
		    S_ISREG(inode->i_mode)) {
                        struct lustre_handle lockh;
                        ldlm_mode_t mode;

                        /* As it is possible a blocking ast has been processed
                         * by this time, we need to check there is an UPDATE
                         * lock on the client and set LLIF_MDS_SIZE_LOCK holding
                         * it. */
                        mode = ll_take_md_lock(inode, MDS_INODELOCK_UPDATE,
					       &lockh, LDLM_FL_CBPENDING,
					       LCK_CR | LCK_CW |
					       LCK_PR | LCK_PW);
                        if (mode) {
                                if (lli->lli_flags & (LLIF_DONE_WRITING |
                                                      LLIF_EPOCH_PENDING |
                                                      LLIF_SOM_DIRTY)) {
					CERROR("%s: inode "DFID" flags %u still"
					       " has size authority! do not "
					       "trust the size from MDS\n",
					       sbi->ll_md_exp->exp_obd->obd_name,
					       PFID(ll_inode2fid(inode)),
					       lli->lli_flags);
                                } else {
                                        /* Use old size assignment to avoid
                                         * deadlock bz14138 & bz14326 */
					i_size_write(inode, body->mbo_size);
					spin_lock(&lli->lli_lock);
                                        lli->lli_flags |= LLIF_MDS_SIZE_LOCK;
					spin_unlock(&lli->lli_lock);
                                }
                                ldlm_lock_decref(&lockh, mode);
                        }
                } else {
                        /* Use old size assignment to avoid
                         * deadlock bz14138 & bz14326 */
			i_size_write(inode, body->mbo_size);

			CDEBUG(D_VFSTRACE,
			       "inode="DFID", updating i_size %llu\n",
			       PFID(ll_inode2fid(inode)),
			       (unsigned long long)body->mbo_size);
		}

		if (body->mbo_valid & OBD_MD_FLBLOCKS)
			inode->i_blocks = body->mbo_blocks;
	}

	if (body->mbo_valid & OBD_MD_FLMDSCAPA) {
		LASSERT(md->mds_capa);
		ll_add_capa(inode, md->mds_capa);
	}

	if (body->mbo_valid & OBD_MD_FLOSSCAPA) {
		LASSERT(md->oss_capa);
		ll_add_capa(inode, md->oss_capa);
	}

	if (body->mbo_valid & OBD_MD_TSTATE) {
		if (body->mbo_t_state & MS_RESTORE)
			lli->lli_flags |= LLIF_FILE_RESTORING;
		else
			lli->lli_flags &= ~LLIF_FILE_RESTORING;
	}

	return 0;
}

int ll_read_inode2(struct inode *inode, void *opaque)
{
        struct lustre_md *md = opaque;
        struct ll_inode_info *lli = ll_i2info(inode);
	int	rc;
        ENTRY;

        CDEBUG(D_VFSTRACE, "VFS Op:inode="DFID"(%p)\n",
               PFID(&lli->lli_fid), inode);

	LASSERT(!lli->lli_has_smd);

        /* Core attributes from the MDS first.  This is a new inode, and
         * the VFS doesn't zero times in the core inode so we have to do
         * it ourselves.  They will be overwritten by either MDS or OST
         * attributes - we just need to make sure they aren't newer. */
        LTIME_S(inode->i_mtime) = 0;
        LTIME_S(inode->i_atime) = 0;
        LTIME_S(inode->i_ctime) = 0;
        inode->i_rdev = 0;
	rc = ll_update_inode(inode, md);
	if (rc != 0)
		RETURN(rc);

        /* OIDEBUG(inode); */

#ifdef HAVE_BACKING_DEV_INFO
	/* initializing backing dev info. */
	inode->i_mapping->backing_dev_info = &s2lsi(inode->i_sb)->lsi_bdi;
#endif
        if (S_ISREG(inode->i_mode)) {
                struct ll_sb_info *sbi = ll_i2sbi(inode);
                inode->i_op = &ll_file_inode_operations;
                inode->i_fop = sbi->ll_fop;
                inode->i_mapping->a_ops = (struct address_space_operations *)&ll_aops;
                EXIT;
        } else if (S_ISDIR(inode->i_mode)) {
                inode->i_op = &ll_dir_inode_operations;
                inode->i_fop = &ll_dir_operations;
                EXIT;
        } else if (S_ISLNK(inode->i_mode)) {
                inode->i_op = &ll_fast_symlink_inode_operations;
                EXIT;
        } else {
                inode->i_op = &ll_special_inode_operations;

		init_special_inode(inode, inode->i_mode,
				   inode->i_rdev);

                EXIT;
        }

	return 0;
}

void ll_delete_inode(struct inode *inode)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	ENTRY;

	if (S_ISREG(inode->i_mode) && lli->lli_clob != NULL)
		/* It is last chance to write out dirty pages,
		 * otherwise we may lose data while umount */
		cl_sync_file_range(inode, 0, OBD_OBJECT_EOF, CL_FSYNC_LOCAL, 1);

	truncate_inode_pages_final(&inode->i_data);

	LASSERTF(inode->i_data.nrpages == 0, "inode="DFID"(%p) nrpages=%lu, "
		 "see https://jira.hpdd.intel.com/browse/LU-118\n",
		 PFID(ll_inode2fid(inode)), inode, inode->i_data.nrpages);

#ifdef HAVE_SBOPS_EVICT_INODE
	ll_clear_inode(inode);
#endif
	clear_inode(inode);

        EXIT;
}

int ll_iocontrol(struct inode *inode, struct file *file,
                 unsigned int cmd, unsigned long arg)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ptlrpc_request *req = NULL;
        int rc, flags = 0;
        ENTRY;

        switch(cmd) {
        case FSFILT_IOC_GETFLAGS: {
                struct mdt_body *body;
                struct md_op_data *op_data;

                op_data = ll_prep_md_op_data(NULL, inode, NULL, NULL,
                                             0, 0, LUSTRE_OPC_ANY,
                                             NULL);
                if (IS_ERR(op_data))
                        RETURN(PTR_ERR(op_data));

                op_data->op_valid = OBD_MD_FLFLAGS;
                rc = md_getattr(sbi->ll_md_exp, op_data, &req);
                ll_finish_md_op_data(op_data);
                if (rc) {
			CERROR("%s: failure inode "DFID": rc = %d\n",
			       sbi->ll_md_exp->exp_obd->obd_name,
			       PFID(ll_inode2fid(inode)), rc);
                        RETURN(-abs(rc));
                }

                body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);

		flags = body->mbo_flags;

                ptlrpc_req_finished(req);

		RETURN(put_user(flags, (int __user *)arg));
        }
        case FSFILT_IOC_SETFLAGS: {
		struct lov_stripe_md *lsm;
                struct obd_info oinfo = { { { 0 } } };
                struct md_op_data *op_data;

		if (get_user(flags, (int __user *)arg))
			RETURN(-EFAULT);

                op_data = ll_prep_md_op_data(NULL, inode, NULL, NULL, 0, 0,
                                             LUSTRE_OPC_ANY, NULL);
                if (IS_ERR(op_data))
                        RETURN(PTR_ERR(op_data));

		op_data->op_attr_flags = flags;
                op_data->op_attr.ia_valid |= ATTR_ATTR_FLAG;
                rc = md_setattr(sbi->ll_md_exp, op_data,
                                NULL, 0, NULL, 0, &req, NULL);
                ll_finish_md_op_data(op_data);
                ptlrpc_req_finished(req);
		if (rc)
			RETURN(rc);

		inode->i_flags = ll_ext_to_inode_flags(flags);

		lsm = ccc_inode_lsm_get(inode);
		if (!lsm_has_objects(lsm)) {
			ccc_inode_lsm_put(inode, lsm);
			RETURN(0);
		}

		OBDO_ALLOC(oinfo.oi_oa);
		if (!oinfo.oi_oa) {
			ccc_inode_lsm_put(inode, lsm);
			RETURN(-ENOMEM);
		}
		oinfo.oi_md = lsm;
		oinfo.oi_oa->o_oi = lsm->lsm_oi;
                oinfo.oi_oa->o_flags = flags;
                oinfo.oi_oa->o_valid = OBD_MD_FLID | OBD_MD_FLFLAGS |
                                       OBD_MD_FLGROUP;
                oinfo.oi_capa = ll_mdscapa_get(inode);
                obdo_set_parent_fid(oinfo.oi_oa, &ll_i2info(inode)->lli_fid);
                rc = obd_setattr_rqset(sbi->ll_dt_exp, &oinfo, NULL);
                capa_put(oinfo.oi_capa);
                OBDO_FREE(oinfo.oi_oa);
		ccc_inode_lsm_put(inode, lsm);

		if (rc && rc != -EPERM && rc != -EACCES)
			CERROR("osc_setattr_async fails: rc = %d\n", rc);

		RETURN(rc);
        }
        default:
                RETURN(-ENOSYS);
        }

        RETURN(0);
}

int ll_flush_ctx(struct inode *inode)
{
	struct ll_sb_info  *sbi = ll_i2sbi(inode);

	CDEBUG(D_SEC, "flush context for user %d\n",
	       from_kuid(&init_user_ns, current_uid()));

	obd_set_info_async(NULL, sbi->ll_md_exp,
			   sizeof(KEY_FLUSH_CTX), KEY_FLUSH_CTX,
			   0, NULL, NULL);
	obd_set_info_async(NULL, sbi->ll_dt_exp,
			   sizeof(KEY_FLUSH_CTX), KEY_FLUSH_CTX,
			   0, NULL, NULL);
	return 0;
}

/* umount -f client means force down, don't save state */
void ll_umount_begin(struct super_block *sb)
{
	struct ll_sb_info *sbi = ll_s2sbi(sb);
	struct obd_device *obd;
	struct obd_ioctl_data *ioc_data;
	ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op: superblock %p count %d active %d\n", sb,
	       sb->s_count, atomic_read(&sb->s_active));

	obd = class_exp2obd(sbi->ll_md_exp);
	if (obd == NULL) {
		CERROR("Invalid MDC connection handle "LPX64"\n",
		       sbi->ll_md_exp->exp_handle.h_cookie);
		EXIT;
		return;
	}
	obd->obd_force = 1;

        obd = class_exp2obd(sbi->ll_dt_exp);
        if (obd == NULL) {
                CERROR("Invalid LOV connection handle "LPX64"\n",
                       sbi->ll_dt_exp->exp_handle.h_cookie);
                EXIT;
                return;
        }
        obd->obd_force = 1;

        OBD_ALLOC_PTR(ioc_data);
	if (ioc_data) {
		obd_iocontrol(IOC_OSC_SET_ACTIVE, sbi->ll_md_exp,
			      sizeof *ioc_data, ioc_data, NULL);

		obd_iocontrol(IOC_OSC_SET_ACTIVE, sbi->ll_dt_exp,
			      sizeof *ioc_data, ioc_data, NULL);

		OBD_FREE_PTR(ioc_data);
	}

	/* Really, we'd like to wait until there are no requests outstanding,
	 * and then continue.  For now, we just invalidate the requests,
	 * schedule() and sleep one second if needed, and hope.
	 */
	schedule();
	EXIT;
}

int ll_remount_fs(struct super_block *sb, int *flags, char *data)
{
        struct ll_sb_info *sbi = ll_s2sbi(sb);
        char *profilenm = get_profile_name(sb);
        int err;
        __u32 read_only;

        if ((*flags & MS_RDONLY) != (sb->s_flags & MS_RDONLY)) {
                read_only = *flags & MS_RDONLY;
                err = obd_set_info_async(NULL, sbi->ll_md_exp,
                                         sizeof(KEY_READ_ONLY),
                                         KEY_READ_ONLY, sizeof(read_only),
                                         &read_only, NULL);
                if (err) {
                        LCONSOLE_WARN("Failed to remount %s %s (%d)\n",
                                      profilenm, read_only ?
                                      "read-only" : "read-write", err);
                        return err;
                }

                if (read_only)
                        sb->s_flags |= MS_RDONLY;
                else
                        sb->s_flags &= ~MS_RDONLY;

                if (sbi->ll_flags & LL_SBI_VERBOSE)
                        LCONSOLE_WARN("Remounted %s %s\n", profilenm,
                                      read_only ?  "read-only" : "read-write");
        }
        return 0;
}

int ll_prep_inode(struct inode **inode, struct ptlrpc_request *req,
		  struct super_block *sb, struct lookup_intent *it)
{
	struct ll_sb_info *sbi = NULL;
	struct lustre_md md = { NULL };
	int rc;
	ENTRY;

        LASSERT(*inode || sb);
        sbi = sb ? ll_s2sbi(sb) : ll_i2sbi(*inode);
        rc = md_get_lustre_md(sbi->ll_md_exp, req, sbi->ll_dt_exp,
                              sbi->ll_md_exp, &md);
        if (rc)
                RETURN(rc);

	if (*inode) {
		rc = ll_update_inode(*inode, &md);
		if (rc != 0)
			GOTO(out, rc);
	} else {
		LASSERT(sb != NULL);

		/*
		 * At this point server returns to client's same fid as client
		 * generated for creating. So using ->fid1 is okay here.
		 */
		if (!fid_is_sane(&md.body->mbo_fid1)) {
			CERROR("%s: Fid is insane "DFID"\n",
				ll_get_fsname(sb, NULL, 0),
				PFID(&md.body->mbo_fid1));
			GOTO(out, rc = -EINVAL);
		}

		*inode = ll_iget(sb, cl_fid_build_ino(&md.body->mbo_fid1,
					     sbi->ll_flags & LL_SBI_32BIT_API),
				 &md);
		if (IS_ERR(*inode)) {
#ifdef CONFIG_FS_POSIX_ACL
                        if (md.posix_acl) {
                                posix_acl_release(md.posix_acl);
                                md.posix_acl = NULL;
                        }
#endif
                        rc = IS_ERR(*inode) ? PTR_ERR(*inode) : -ENOMEM;
                        *inode = NULL;
                        CERROR("new_inode -fatal: rc %d\n", rc);
                        GOTO(out, rc);
                }
        }

	/* Handling piggyback layout lock.
	 * Layout lock can be piggybacked by getattr and open request.
	 * The lsm can be applied to inode only if it comes with a layout lock
	 * otherwise correct layout may be overwritten, for example:
	 * 1. proc1: mdt returns a lsm but not granting layout
	 * 2. layout was changed by another client
	 * 3. proc2: refresh layout and layout lock granted
	 * 4. proc1: to apply a stale layout */
	if (it != NULL && it->d.lustre.it_lock_mode != 0) {
		struct lustre_handle lockh;
		struct ldlm_lock *lock;

		lockh.cookie = it->d.lustre.it_lock_handle;
		lock = ldlm_handle2lock(&lockh);
		LASSERT(lock != NULL);
		if (ldlm_has_layout(lock)) {
			struct cl_object_conf conf;

			memset(&conf, 0, sizeof(conf));
			conf.coc_opc = OBJECT_CONF_SET;
			conf.coc_inode = *inode;
			conf.coc_lock = lock;
			conf.u.coc_md = &md;
			(void)ll_layout_conf(*inode, &conf);
		}
		LDLM_LOCK_PUT(lock);
	}

out:
	if (md.lsm != NULL)
		obd_free_memmd(sbi->ll_dt_exp, &md.lsm);
	md_free_lustre_md(sbi->ll_md_exp, &md);
	RETURN(rc);
}

int ll_obd_statfs(struct inode *inode, void __user *arg)
{
        struct ll_sb_info *sbi = NULL;
        struct obd_export *exp;
        char *buf = NULL;
        struct obd_ioctl_data *data = NULL;
        __u32 type;
	__u32 __user flags;	/* not user, but obd_iocontrol is abused */
        int len = 0, rc;

        if (!inode || !(sbi = ll_i2sbi(inode)))
                GOTO(out_statfs, rc = -EINVAL);

        rc = obd_ioctl_getdata(&buf, &len, arg);
        if (rc)
                GOTO(out_statfs, rc);

        data = (void*)buf;
        if (!data->ioc_inlbuf1 || !data->ioc_inlbuf2 ||
            !data->ioc_pbuf1 || !data->ioc_pbuf2)
                GOTO(out_statfs, rc = -EINVAL);

        if (data->ioc_inllen1 != sizeof(__u32) ||
            data->ioc_inllen2 != sizeof(__u32) ||
            data->ioc_plen1 != sizeof(struct obd_statfs) ||
            data->ioc_plen2 != sizeof(struct obd_uuid))
                GOTO(out_statfs, rc = -EINVAL);

        memcpy(&type, data->ioc_inlbuf1, sizeof(__u32));
	if (type & LL_STATFS_LMV)
                exp = sbi->ll_md_exp;
	else if (type & LL_STATFS_LOV)
                exp = sbi->ll_dt_exp;
        else
                GOTO(out_statfs, rc = -ENODEV);

	flags = (type & LL_STATFS_NODELAY) ? OBD_STATFS_NODELAY : 0;
	rc = obd_iocontrol(IOC_OBD_STATFS, exp, len, buf, &flags);
        if (rc)
                GOTO(out_statfs, rc);
out_statfs:
        if (buf)
                obd_ioctl_freedata(buf, len);
        return rc;
}

int ll_process_config(struct lustre_cfg *lcfg)
{
	struct super_block *sb;
	unsigned long x;
	int rc = 0;
	char *ptr;

	/* The instance name contains the sb: lustre-client-aacfe000 */
	ptr = strrchr(lustre_cfg_string(lcfg, 0), '-');
	if (!ptr || !*(++ptr))
		return -EINVAL;
	if (sscanf(ptr, "%lx", &x) != 1)
		return -EINVAL;
	sb = (struct super_block *)x;
	/* This better be a real Lustre superblock! */
	LASSERT(s2lsi(sb)->lsi_lmd->lmd_magic == LMD_MAGIC);

	/* Note we have not called client_common_fill_super yet, so
	   proc fns must be able to handle that! */
	rc = class_process_proc_param(PARAM_LLITE, lprocfs_llite_obd_vars,
				      lcfg, sb);
	if (rc > 0)
		rc = 0;
	return rc;
}

/* this function prepares md_op_data hint for passing ot down to MD stack. */
struct md_op_data * ll_prep_md_op_data(struct md_op_data *op_data,
				       struct inode *i1, struct inode *i2,
				       const char *name, size_t namelen,
				       __u32 mode, __u32 opc, void *data)
{
        LASSERT(i1 != NULL);

	if (name == NULL) {
		/* Do not reuse namelen for something else. */
		if (namelen != 0)
			return ERR_PTR(-EINVAL);
	} else {
		if (namelen > ll_i2sbi(i1)->ll_namelen)
			return ERR_PTR(-ENAMETOOLONG);

		if (!lu_name_is_valid_2(name, namelen))
			return ERR_PTR(-EINVAL);
	}

        if (op_data == NULL)
                OBD_ALLOC_PTR(op_data);

        if (op_data == NULL)
                return ERR_PTR(-ENOMEM);

	ll_i2gids(op_data->op_suppgids, i1, i2);
	op_data->op_fid1 = *ll_inode2fid(i1);
	op_data->op_capa1 = ll_mdscapa_get(i1);
	op_data->op_default_stripe_offset = -1;
	if (S_ISDIR(i1->i_mode)) {
		op_data->op_mea1 = ll_i2info(i1)->lli_lsm_md;
		op_data->op_default_stripe_offset =
			   ll_i2info(i1)->lli_def_stripe_offset;
	}

	if (i2) {
		op_data->op_fid2 = *ll_inode2fid(i2);
		op_data->op_capa2 = ll_mdscapa_get(i2);
		if (S_ISDIR(i2->i_mode))
			op_data->op_mea2 = ll_i2info(i2)->lli_lsm_md;
	} else {
		fid_zero(&op_data->op_fid2);
		op_data->op_capa2 = NULL;
	}

	if (ll_i2sbi(i1)->ll_flags & LL_SBI_64BIT_HASH)
		op_data->op_cli_flags |= CLI_HASH64;

	if (ll_need_32bit_api(ll_i2sbi(i1)))
		op_data->op_cli_flags |= CLI_API32;

	op_data->op_name = name;
	op_data->op_namelen = namelen;
	op_data->op_mode = mode;
	op_data->op_mod_time = cfs_time_current_sec();
	op_data->op_fsuid = from_kuid(&init_user_ns, current_fsuid());
	op_data->op_fsgid = from_kgid(&init_user_ns, current_fsgid());
	op_data->op_cap = cfs_curproc_cap_pack();
	op_data->op_bias = 0;
	op_data->op_cli_flags = 0;
	if ((opc == LUSTRE_OPC_CREATE) && (name != NULL) &&
	     filename_is_volatile(name, namelen, &op_data->op_mds)) {
		op_data->op_bias |= MDS_CREATE_VOLATILE;
	} else {
		op_data->op_mds = 0;
	}
	op_data->op_data = data;

	/* When called by ll_setattr_raw, file is i1. */
	if (LLIF_DATA_MODIFIED & ll_i2info(i1)->lli_flags)
		op_data->op_bias |= MDS_DATA_MODIFIED;

	return op_data;
}

void ll_finish_md_op_data(struct md_op_data *op_data)
{
        capa_put(op_data->op_capa1);
        capa_put(op_data->op_capa2);
	security_release_secctx(op_data->op_file_secctx,
				op_data->op_file_secctx_size);
        OBD_FREE_PTR(op_data);
}

#ifdef HAVE_SUPEROPS_USE_DENTRY
int ll_show_options(struct seq_file *seq, struct dentry *dentry)
#else
int ll_show_options(struct seq_file *seq, struct vfsmount *vfs)
#endif
{
        struct ll_sb_info *sbi;

#ifdef HAVE_SUPEROPS_USE_DENTRY
	LASSERT((seq != NULL) && (dentry != NULL));
	sbi = ll_s2sbi(dentry->d_sb);
#else
	LASSERT((seq != NULL) && (vfs != NULL));
	sbi = ll_s2sbi(vfs->mnt_sb);
#endif

        if (sbi->ll_flags & LL_SBI_NOLCK)
                seq_puts(seq, ",nolock");

        if (sbi->ll_flags & LL_SBI_FLOCK)
                seq_puts(seq, ",flock");

        if (sbi->ll_flags & LL_SBI_LOCALFLOCK)
                seq_puts(seq, ",localflock");

        if (sbi->ll_flags & LL_SBI_USER_XATTR)
                seq_puts(seq, ",user_xattr");

        if (sbi->ll_flags & LL_SBI_LAZYSTATFS)
                seq_puts(seq, ",lazystatfs");

	if (sbi->ll_flags & LL_SBI_USER_FID2PATH)
		seq_puts(seq, ",user_fid2path");

        RETURN(0);
}

/**
 * Get obd name by cmd, and copy out to user space
 */
int ll_get_obd_name(struct inode *inode, unsigned int cmd, unsigned long arg)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct obd_device *obd;
        ENTRY;

        if (cmd == OBD_IOC_GETDTNAME)
                obd = class_exp2obd(sbi->ll_dt_exp);
        else if (cmd == OBD_IOC_GETMDNAME)
                obd = class_exp2obd(sbi->ll_md_exp);
        else
                RETURN(-EINVAL);

        if (!obd)
                RETURN(-ENOENT);

	if (copy_to_user((void __user *)arg, obd->obd_name,
			 strlen(obd->obd_name) + 1))
		RETURN(-EFAULT);

	RETURN(0);
}

/**
 * Get lustre file system name by \a sbi. If \a buf is provided(non-NULL), the
 * fsname will be returned in this buffer; otherwise, a static buffer will be
 * used to store the fsname and returned to caller.
 */
char *ll_get_fsname(struct super_block *sb, char *buf, int buflen)
{
	static char fsname_static[MTI_NAME_MAXLEN];
	struct lustre_sb_info *lsi = s2lsi(sb);
	char *ptr;
	int len;

	if (buf == NULL) {
		/* this means the caller wants to use static buffer
		 * and it doesn't care about race. Usually this is
		 * in error reporting path */
		buf = fsname_static;
		buflen = sizeof(fsname_static);
	}

	len = strlen(lsi->lsi_lmd->lmd_profile);
	ptr = strrchr(lsi->lsi_lmd->lmd_profile, '-');
	if (ptr && (strcmp(ptr, "-client") == 0))
		len -= 7;

	if (unlikely(len >= buflen))
		len = buflen - 1;
	strncpy(buf, lsi->lsi_lmd->lmd_profile, len);
	buf[len] = '\0';

	return buf;
}

static char* ll_d_path(struct dentry *dentry, char *buf, int bufsize)
{
	char *path = NULL;

	struct path p;

	p.dentry = dentry;
	p.mnt = current->fs->root.mnt;
	path_get(&p);
	path = d_path(&p, buf, bufsize);
	path_put(&p);
	return path;
}

void ll_dirty_page_discard_warn(struct page *page, int ioret)
{
	char *buf, *path = NULL;
	struct dentry *dentry = NULL;
	struct vvp_object *obj = cl_inode2vvp(page->mapping->host);

	/* this can be called inside spin lock so use GFP_ATOMIC. */
	buf = (char *)__get_free_page(GFP_ATOMIC);
	if (buf != NULL) {
		dentry = d_find_alias(page->mapping->host);
		if (dentry != NULL)
			path = ll_d_path(dentry, buf, PAGE_SIZE);
	}

	CDEBUG(D_WARNING,
	       "%s: dirty page discard: %s/fid: "DFID"/%s may get corrupted "
	       "(rc %d)\n", ll_get_fsname(page->mapping->host->i_sb, NULL, 0),
	       s2lsi(page->mapping->host->i_sb)->lsi_lmd->lmd_dev,
	       PFID(&obj->vob_header.coh_lu.loh_fid),
	       (path && !IS_ERR(path)) ? path : "", ioret);

	if (dentry != NULL)
		dput(dentry);

	if (buf != NULL)
		free_page((unsigned long)buf);
}

ssize_t ll_copy_user_md(const struct lov_user_md __user *md,
			struct lov_user_md **kbuf)
{
	struct lov_user_md	lum;
	ssize_t			lum_size;
	ENTRY;

	if (copy_from_user(&lum, md, sizeof(lum)))
		RETURN(-EFAULT);

	lum_size = ll_lov_user_md_size(&lum);
	if (lum_size < 0)
		RETURN(lum_size);

	OBD_ALLOC(*kbuf, lum_size);
	if (*kbuf == NULL)
		RETURN(-ENOMEM);

	if (copy_from_user(*kbuf, md, lum_size) != 0) {
		OBD_FREE(*kbuf, lum_size);
		RETURN(-EFAULT);
	}

	RETURN(lum_size);
}

/*
 * Compute llite root squash state after a change of root squash
 * configuration setting or add/remove of a lnet nid
 */
void ll_compute_rootsquash_state(struct ll_sb_info *sbi)
{
	struct root_squash_info *squash = &sbi->ll_squash;
	int i;
	bool matched;
	lnet_process_id_t id;

	/* Update norootsquash flag */
	down_write(&squash->rsi_sem);
	if (list_empty(&squash->rsi_nosquash_nids))
		sbi->ll_flags &= ~LL_SBI_NOROOTSQUASH;
	else {
		/* Do not apply root squash as soon as one of our NIDs is
		 * in the nosquash_nids list */
		matched = false;
		i = 0;
		while (LNetGetId(i++, &id) != -ENOENT) {
			if (LNET_NETTYP(LNET_NIDNET(id.nid)) == LOLND)
				continue;
			if (cfs_match_nid(id.nid, &squash->rsi_nosquash_nids)) {
				matched = true;
				break;
			}
		}
		if (matched)
			sbi->ll_flags |= LL_SBI_NOROOTSQUASH;
		else
			sbi->ll_flags &= ~LL_SBI_NOROOTSQUASH;
	}
	up_write(&squash->rsi_sem);
}

/**
 * Parse linkea content to extract information about a given hardlink
 *
 * \param[in]   ldata      - Initialized linkea data
 * \param[in]   linkno     - Link identifier
 * \param[out]  parent_fid - The entry's parent FID
 * \param[out]  ln         - Entry name destination buffer
 *
 * \retval 0 on success
 * \retval Appropriate negative error code on failure
 */
static int ll_linkea_decode(struct linkea_data *ldata, unsigned int linkno,
			    struct lu_fid *parent_fid, struct lu_name *ln)
{
	unsigned int	idx;
	int		rc;
	ENTRY;

	rc = linkea_init(ldata);
	if (rc < 0)
		RETURN(rc);

	if (linkno >= ldata->ld_leh->leh_reccount)
		/* beyond last link */
		RETURN(-ENODATA);

	linkea_first_entry(ldata);
	for (idx = 0; ldata->ld_lee != NULL; idx++) {
		linkea_entry_unpack(ldata->ld_lee, &ldata->ld_reclen, ln,
				    parent_fid);
		if (idx == linkno)
			break;

		linkea_next_entry(ldata);
	}

	if (idx < linkno)
		RETURN(-ENODATA);

	RETURN(0);
}

/**
 * Get parent FID and name of an identified link. Operation is performed for
 * a given link number, letting the caller iterate over linkno to list one or
 * all links of an entry.
 *
 * \param[in]     file - File descriptor against which to perform the operation
 * \param[in,out] arg  - User-filled structure containing the linkno to operate
 *                       on and the available size. It is eventually filled with
 *                       the requested information or left untouched on error
 *
 * \retval - 0 on success
 * \retval - Appropriate negative error code on failure
 */
int ll_getparent(struct file *file, struct getparent __user *arg)
{
	struct dentry		*dentry = file->f_path.dentry;
	struct inode		*inode = dentry->d_inode;
	struct linkea_data	*ldata;
	struct lu_buf		 buf = LU_BUF_NULL;
	struct lu_name		 ln;
	struct lu_fid		 parent_fid;
	__u32			 linkno;
	__u32			 name_size;
	int			 rc;

	ENTRY;

	if (!cfs_capable(CFS_CAP_DAC_READ_SEARCH) &&
	    !(ll_i2sbi(inode)->ll_flags & LL_SBI_USER_FID2PATH))
		RETURN(-EPERM);

	if (get_user(name_size, &arg->gp_name_size))
		RETURN(-EFAULT);

	if (get_user(linkno, &arg->gp_linkno))
		RETURN(-EFAULT);

	if (name_size > PATH_MAX)
		RETURN(-EINVAL);

	OBD_ALLOC(ldata, sizeof(*ldata));
	if (ldata == NULL)
		RETURN(-ENOMEM);

	rc = linkea_data_new(ldata, &buf);
	if (rc < 0)
		GOTO(ldata_free, rc);

	rc = ll_getxattr(dentry, XATTR_NAME_LINK, buf.lb_buf, buf.lb_len);
	if (rc < 0)
		GOTO(lb_free, rc);

	rc = ll_linkea_decode(ldata, linkno, &parent_fid, &ln);
	if (rc < 0)
		GOTO(lb_free, rc);

	if (ln.ln_namelen >= name_size)
		GOTO(lb_free, rc = -EOVERFLOW);

	if (copy_to_user(&arg->gp_fid, &parent_fid, sizeof(arg->gp_fid)))
		GOTO(lb_free, rc = -EFAULT);

	if (copy_to_user(&arg->gp_name, ln.ln_name, ln.ln_namelen))
		GOTO(lb_free, rc = -EFAULT);

	if (put_user('\0', arg->gp_name + ln.ln_namelen))
		GOTO(lb_free, rc = -EFAULT);

lb_free:
	lu_buf_free(&buf);
ldata_free:
	OBD_FREE(ldata, sizeof(*ldata));

	RETURN(rc);
}
