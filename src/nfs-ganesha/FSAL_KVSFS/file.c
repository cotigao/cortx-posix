/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) Panasas Inc., 2011
 * Author: Jim Lieb jlieb@panasas.com
 *
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * -------------
 */

/* file.c
 * File I/O methods for KVSFS module
 */

#include "config.h"

#include <assert.h>
#include "fsal.h"
#include "fsal_internal.h"
#include "FSAL/access_check.h"
#include "fsal_convert.h"
#include <unistd.h>
#include <fcntl.h>
#include "FSAL/fsal_commonlib.h"
#include "kvsfs_methods.h"
#include <stdbool.h>

/** kvsfs_open
 * called with appropriate locks taken at the cache inode level
 */

fsal_status_t kvsfs_open(struct fsal_obj_handle *obj_hdl,
			fsal_openflags_t openflags)
{
	struct kvsfs_fsal_obj_handle *myself;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	int rc = 0;
	kvsns_cred_t cred;
	kvsns_fs_ctx_t fs_ctx = KVSNS_NULL_FS_CTX;

	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	myself = container_of(obj_hdl,
			      struct kvsfs_fsal_obj_handle, obj_handle);

	assert(myself->u.file.openflags == FSAL_O_CLOSED);

	rc = kvsfs_obj_to_kvsns_ctx(obj_hdl, &fs_ctx);
	if (rc) {
		LogCrit(COMPONENT_FSAL, "Unable to get fs_handle: %d", rc);
		goto errout;
	}

	rc = kvsns2_open(fs_ctx, &cred, &myself->handle->kvsfs_handle, O_RDWR,
			 0777, &myself->u.file.fd);

	if (rc)
		goto errout;

	/* >> fill output struct << */
	myself->u.file.openflags = openflags;

	/* save the stat */
	rc = kvsns_getattr(&cred, &myself->handle->kvsfs_handle,
			   &myself->u.file.saved_stat);

errout:
	if (rc) {
		fsal_error = posix2fsal_error(-rc);
		return fsalstat(fsal_error, -rc);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* kvsfs_status
 * Let the caller peek into the file's open/close state.
 */

fsal_openflags_t kvsfs_status(struct fsal_obj_handle *obj_hdl)
{
	struct kvsfs_fsal_obj_handle *myself;

	myself = container_of(obj_hdl,
			      struct kvsfs_fsal_obj_handle,
			      obj_handle);
	return myself->u.file.openflags;
}

/* kvsfs_read
 * concurrency (locks) is managed in cache_inode_*
 */

fsal_status_t kvsfs_read(struct fsal_obj_handle *obj_hdl,
			uint64_t offset,
			size_t buffer_size, void *buffer, size_t *read_amount,
			bool *end_of_file)
{
	struct kvsfs_fsal_obj_handle *myself;
	kvsns_fs_ctx_t fs_ctx = KVSNS_NULL_FS_CTX;
	int retval = 0;
	kvsns_cred_t cred;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;

	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	retval = kvsfs_obj_to_kvsns_ctx(obj_hdl, &fs_ctx);
	if (retval) {
		LogCrit(COMPONENT_FSAL, "Unable to get fs_handle: %d", retval);
		goto errout;
	}
	myself = container_of(obj_hdl,
			      struct kvsfs_fsal_obj_handle, obj_handle);

	assert(myself->u.file.openflags != FSAL_O_CLOSED);

	retval = kvsns2_read(fs_ctx, &cred, &myself->u.file.fd,
			     buffer, buffer_size, offset);


	/* With FSAL_ZFS, "end of file" is always returned via a last call,
	 * once every data is read. The result is a last,
	 * empty call which set end_of_file to true */
	if (retval < 0) {
		goto errout;
	} else if (retval == 0) {
		*end_of_file = true;
		*read_amount = 0;
	} else {
		*end_of_file = false;
		*read_amount = retval;
	}

errout:
	if (retval < 0) {
		fsal_error = posix2fsal_error(-retval);
		return fsalstat(fsal_error, -retval);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* kvsfs_write
 * concurrency (locks) is managed in cache_inode_*
 */

fsal_status_t kvsfs_write(struct fsal_obj_handle *obj_hdl,
			 uint64_t offset,
			 size_t buffer_size, void *buffer,
			 size_t *write_amount, bool *fsal_stable)
{
	struct kvsfs_fsal_obj_handle *myself;
	kvsns_fs_ctx_t fs_ctx = KVSNS_NULL_FS_CTX;
	kvsns_cred_t cred;
	int retval = 0;

	cred.uid = op_ctx->creds->caller_uid;
	cred.gid = op_ctx->creds->caller_gid;

	retval = kvsfs_obj_to_kvsns_ctx(obj_hdl, &fs_ctx);
	if (retval) {
		LogCrit(COMPONENT_FSAL, "Unable to get fs_handle: %d", retval);
		goto errout;
	}

	myself = container_of(obj_hdl,
			      struct kvsfs_fsal_obj_handle, obj_handle);

	assert(myself->u.file.openflags != FSAL_O_CLOSED);

	retval = kvsns2_write(fs_ctx, &cred, &myself->u.file.fd,
			      buffer, buffer_size, offset);
	if (retval < 0)
		goto errout;
	*write_amount = retval;
	*fsal_stable = false;

errout:
	if (retval < 0) {
		return fsalstat(posix2fsal_error(-retval), -retval);
	}

	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* kvsfs_commit
 * Commit a file range to storage.
 * for right now, fsync will have to do.
 */

fsal_status_t kvsfs_commit(struct fsal_obj_handle *obj_hdl,	/* sync */
			  off_t offset, size_t len)
{
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

/* kvsfs_close
 * Close the file if it is still open.
 * Yes, we ignor lock status.  Closing a file in POSIX
 * releases all locks but that is state and cache inode's problem.
 */

fsal_status_t kvsfs_close(struct fsal_obj_handle *obj_hdl)
{
	struct kvsfs_fsal_obj_handle *myself;
	kvsns_fs_ctx_t fs_ctx = KVSNS_NULL_FS_CTX;
	int retval = 0;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;

	assert(obj_hdl->type == REGULAR_FILE);
	myself = container_of(obj_hdl,
			      struct kvsfs_fsal_obj_handle, obj_handle);

	retval = kvsfs_obj_to_kvsns_ctx(obj_hdl, &fs_ctx);
	if (retval) {
		fsal_error = posix2fsal_error(-retval);
		LogCrit(COMPONENT_FSAL, "Unable to get fs_handle: %d", retval);
		goto errout;
	}

	if (myself->u.file.openflags != FSAL_O_CLOSED) {
		retval = kvsns2_close(fs_ctx, &myself->u.file.fd);
		if (retval < 0)
			fsal_error = posix2fsal_error(-retval);

		myself->u.file.openflags = FSAL_O_CLOSED;
	}

errout:
	return fsalstat(fsal_error, -retval);
}

/* kvsfs_lru_cleanup
 * free non-essential resources at the request of cache inode's
 * LRU processing identifying this handle as stale enough for resource
 * trimming.
 */

fsal_status_t kvsfs_lru_cleanup(struct fsal_obj_handle *obj_hdl,
			       lru_actions_t requests)
{
	return fsalstat(ERR_FSAL_NO_ERROR, 0);
}

fsal_status_t kvsfs_lock_op(struct fsal_obj_handle *obj_hdl,
			    void *p_owner,
			    fsal_lock_op_t lock_op,
			    fsal_lock_param_t *request_lock,
			    fsal_lock_param_t *conflicting_lock)
{
	struct kvsfs_fsal_obj_handle *myself;
	kvsns_fs_ctx_t fs_ctx = KVSNS_NULL_FS_CTX;
	int retval = 0;
	fsal_errors_t fsal_error = ERR_FSAL_NO_ERROR;
	kvsns_lock_t req_lock, conflict_lock;
	kvsns_lock_op_t kvsns_lock_opt;

	assert(obj_hdl->type == REGULAR_FILE);
	myself = container_of(obj_hdl,
			      struct kvsfs_fsal_obj_handle, obj_handle);

	retval = kvsfs_obj_to_kvsns_ctx(obj_hdl, &fs_ctx);
	if (retval) {
		fsal_error = posix2fsal_error(-retval);
		LogCrit(COMPONENT_FSAL, "Unable to get fs_handle: %d", retval);
		goto errout;
	}

	if (myself->u.file.fd.owner.pid < 0 ||
	    myself->u.file.openflags == FSAL_O_CLOSED) {
		LogDebug(COMPONENT_FSAL,
			 "Attempting to lock with no file descriptor open, fd %d",
			 myself->u.file.fd.owner.pid);
		return fsalstat(ERR_FSAL_FAULT, 0);
	}

	if (conflicting_lock == NULL && lock_op == FSAL_OP_LOCKT) {
		LogDebug(COMPONENT_FSAL,
			 "conflicting_lock argument can't be NULL with lock_op	= LOCKT");
		return fsalstat(ERR_FSAL_FAULT, 0);
	}

	if (lock_op == FSAL_OP_LOCKT) {
		kvsns_lock_opt = KVSNS_OP_LOCKT;
	} else if (lock_op == FSAL_OP_LOCK) {
		kvsns_lock_opt = KVSNS_OP_LOCK;
	} else if (lock_op == FSAL_OP_UNLOCK) {
		kvsns_lock_opt = KVSNS_OP_UNLOCK;
	} else {
		LogDebug(COMPONENT_FSAL,
			 "ERROR: Unsupported lock operation %d\n", lock_op);
		return fsalstat(ERR_FSAL_NOTSUPP, 0);
	}

	if (request_lock->lock_type == FSAL_LOCK_R) {
		req_lock.l_type = LCK_RDONLY;
	} else if (request_lock->lock_type == FSAL_LOCK_W) {
		req_lock.l_type = LCK_RW;
	} else {
		LogDebug(COMPONENT_FSAL,
			 "ERROR: The requested lock type was not read or write.");
		return fsalstat(ERR_FSAL_NOTSUPP, 0);
	}

	retval = kvsfs_obj_to_kvsns_ctx(obj_hdl, &fs_ctx);
	if (retval) {
		LogCrit(COMPONENT_FSAL, "Unable to get fs_handle: %d", retval);
		goto errout;
	}

	req_lock.start = request_lock->lock_start;
	req_lock.end = request_lock->lock_start + request_lock->lock_length;
	retval = kvsns_lock_op(fs_ctx, kvsns_lock_opt, &req_lock, &conflict_lock);
	if (retval)
		goto errout;

	LogFullDebug(COMPONENT_FSAL,
		     "Locking: op:%d type:%d claim:%d start:%" PRIu64
		     " length:%lu ", lock_op, request_lock->lock_type,
		     request_lock->lock_reclaim, request_lock->lock_start,
		     request_lock->lock_length);
	return fsalstat(ERR_FSAL_NO_ERROR, 0);

errout:
	return fsalstat(posix2fsal_error(-retval), -retval);
}
