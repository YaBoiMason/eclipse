/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#pragma ident	"%Z%%M%	%I%	%E% SMI"

/*
 * This command is used to create or open a file or directory, when EAs
 * or an SD must be applied to the file. The functionality is similar
 * to SmbNtCreateAndx with the option to supply extended attributes or
 * a security descriptor.
 *
 * Note: we don't decode the extended attributes because we don't
 * support them at this time.
 */

#include <smbsrv/smbvar.h>
#include <smbsrv/smb_kproto.h>
#include <smbsrv/smb_fsops.h>
#include <smbsrv/ntstatus.h>
#include <smbsrv/ntaccess.h>
#include <smbsrv/nterror.h>
#include <smbsrv/ntifs.h>
#include <smbsrv/cifs.h>
#include <smbsrv/doserror.h>

/*
 * smb_nt_transact_create
 *
 * This command is used to create or open a file or directory, when EAs
 * or an SD must be applied to the file. The request parameter block
 * encoding, data block encoding and output parameter block encoding are
 * described in CIFS section 4.2.2.
 *
 * The format of the command is SmbNtTransact but it is basically the same
 * as SmbNtCreateAndx with the option to supply extended attributes or a
 * security descriptor. For information not defined in CIFS section 4.2.2
 * see section 4.2.1 (NT_CREATE_ANDX).
 */
int
smb_nt_transact_create(struct smb_request *sr, struct smb_xa *xa)
{
	struct open_param *op = &sr->arg.open;
	uint8_t			OplockLevel;
	uint8_t			DirFlag;
	uint8_t			SecurityFlags;
	uint32_t		ExtFileAttributes;
	uint32_t		sd_len;
	uint32_t		EaLength;
	uint32_t		Flags;
	uint32_t		ImpersonationLevel;
	uint32_t		RootDirFid;
	uint32_t		NameLength;
	smb_attr_t		new_attr;
	smb_node_t		*node;
	smb_sd_t		sd;
	DWORD status;
	int rc;

	rc = smb_decode_mbc(&xa->req_param_mb, "%lllqllllllllb",
	    sr,
	    &Flags,
	    &RootDirFid,
	    &op->desired_access,
	    &op->dsize,
	    &ExtFileAttributes,
	    &op->share_access,
	    &op->create_disposition,
	    &op->create_options,
	    &sd_len,
	    &EaLength,
	    &NameLength,
	    &ImpersonationLevel,
	    &SecurityFlags);

	if (rc != 0) {
		smbsr_decode_error(sr);
		/* NOTREACHED */
	}

	/*
	 * If name length is zero, interpret as "\".
	 */
	if (NameLength == 0) {
		op->fqi.path = "\\";
	} else {
		rc = smb_decode_mbc(&xa->req_param_mb, "%#u",
		    sr, NameLength, &op->fqi.path);
		if (rc != 0) {
			smbsr_decode_error(sr);
			/* NOTREACHED */
		}
	}

	if ((op->create_options & FILE_DELETE_ON_CLOSE) &&
	    !(op->desired_access & DELETE)) {
		smbsr_error(sr, NT_STATUS_INVALID_PARAMETER, 0, 0);
		/* NOTREACHED */
	}

	if (sd_len) {
		status = smb_decode_sd(xa, &sd);
		if (status != NT_STATUS_SUCCESS) {
			smbsr_error(sr, status, 0, 0);
			/* NOTREACHED */
		}
		op->sd = &sd;
	} else {
		op->sd = NULL;
	}

	op->fqi.srch_attr = 0;
	op->omode = 0;

	op->utime.tv_sec = op->utime.tv_nsec = 0;
	op->my_flags = 0;

	op->dattr = ExtFileAttributes;

	if (Flags) {
		if (Flags & NT_CREATE_FLAG_REQUEST_OPLOCK) {
			if (Flags & NT_CREATE_FLAG_REQUEST_OPBATCH) {
				op->my_flags = MYF_BATCH_OPLOCK;
			} else {
				op->my_flags = MYF_EXCLUSIVE_OPLOCK;
			}
		}
		if (Flags & NT_CREATE_FLAG_OPEN_TARGET_DIR)
			op->my_flags |= MYF_MUST_BE_DIRECTORY;
	}

	if (ExtFileAttributes & FILE_FLAG_WRITE_THROUGH)
		op->create_options |= FILE_WRITE_THROUGH;

	if (ExtFileAttributes & FILE_FLAG_DELETE_ON_CLOSE)
		op->create_options |= FILE_DELETE_ON_CLOSE;

	if (RootDirFid == 0) {
		op->fqi.dir_snode = sr->tid_tree->t_snode;
	} else {
		sr->smb_fid = (ushort_t)RootDirFid;
		sr->fid_ofile = smb_ofile_lookup_by_fid(sr->tid_tree,
		    sr->smb_fid);
		/*
		 * XXX: ASSERT() for now but we should understand if the test
		 *	of the return value is missing because it cannot happen.
		 */
		ASSERT(sr->fid_ofile != NULL);
		op->fqi.dir_snode = sr->fid_ofile->f_node;
		smbsr_disconnect_file(sr);
	}

	status = smb_open_subr(sr);
	if (op->sd)
		smb_sd_term(op->sd);

	if (status != NT_STATUS_SUCCESS) {
		if (status == NT_STATUS_SHARING_VIOLATION)
			smbsr_error(sr, NT_STATUS_SHARING_VIOLATION,
			    ERRDOS, ERROR_SHARING_VIOLATION);
		else
			smbsr_error(sr, status, 0, 0);

		/* NOTREACHED */
	}

	if (STYPE_ISDSK(sr->tid_tree->t_res_type)) {
		switch (MYF_OPLOCK_TYPE(op->my_flags)) {
		case MYF_EXCLUSIVE_OPLOCK :
			OplockLevel = 1;
			break;
		case MYF_BATCH_OPLOCK :
			OplockLevel = 2;
			break;
		case MYF_LEVEL_II_OPLOCK :
			OplockLevel = 3;
			break;
		case MYF_OPLOCK_NONE :
		default:
			OplockLevel = 0;
			break;
		}

		if (op->create_options & FILE_DELETE_ON_CLOSE)
			smb_preset_delete_on_close(sr->fid_ofile);

		/*
		 * Set up the directory flag and ensure that
		 * we don't return a stale file size.
		 */
		node = sr->fid_ofile->f_node;
		if (node->attr.sa_vattr.va_type == VDIR) {
			DirFlag = 1;
			new_attr.sa_vattr.va_size = 0;
		} else {
			DirFlag = 0;
			new_attr.sa_mask = SMB_AT_SIZE;
			(void) smb_fsop_getattr(sr, kcred, node, &new_attr);
			node->attr.sa_vattr.va_size = new_attr.sa_vattr.va_size;
		}

		(void) smb_encode_mbc(&xa->rep_param_mb, "b.wllTTTTlqqwwb",
		    OplockLevel,
		    sr->smb_fid,
		    op->action_taken,
		    0,	/* EaErrorOffset */
		    &node->attr.sa_crtime,
		    &node->attr.sa_vattr.va_atime,
		    &node->attr.sa_vattr.va_mtime,
		    &node->attr.sa_vattr.va_ctime,
		    op->dattr & FILE_ATTRIBUTE_MASK,
		    new_attr.sa_vattr.va_size,
		    new_attr.sa_vattr.va_size,
		    op->ftype,
		    op->devstate,
		    DirFlag);
	} else {
		/* Named PIPE */
		(void) smb_encode_mbc(&xa->rep_param_mb, "b.wllTTTTlqqwwb",
		    0,
		    sr->smb_fid,
		    op->action_taken,
		    0,	/* EaErrorOffset */
		    0LL,
		    0LL,
		    0LL,
		    0LL,
		    op->dattr,
		    0x1000LL,
		    0LL,
		    op->ftype,
		    op->devstate,
		    0);
	}

	return (SDRC_NORMAL_REPLY);
}
