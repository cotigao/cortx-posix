/*
 * Filename: fs.c
 * Description: EFS Filesystem functions API.
 *
 * Do NOT modify or remove this copyright and confidentiality notice!
 * Copyright (c) 2020, Seagate Technology, LLC.
 * The code contained herein is CONFIDENTIAL to Seagate Technology, LLC.
 * Portions are also trade secret. Any use, duplication, derivation, distribution
 * or disclosure of this code, for any reason, not expressly authorized is
 * prohibited. All other rights are expressly reserved by Seagate Technology, LLC.
 *
 * Author: Satendra Singh <satendra.singh@seagate.com>
 * Author: Jatinder Kumar <jatinder.kumar@seagate.com>
 *
 */

#include <errno.h> /* errono */
#include <string.h> /* memcpy */
#include "common.h" /* container_of */
#include "internal/fs.h" /* fs interface */
#include "namespace.h" /* namespace methods */
#include "tenant.h" /* tenant method */
#include "common/helpers.h" /* RC_WRAP_LABEL */
#include "common/log.h" /* logging */
#include "kvtree.h"
#include "kvnode.h"

/* data types */

/* fs node : in memory data structure. */
struct efs_fs_node {
	struct efs_fs efs_fs; /* fs object */
	LIST_ENTRY(efs_fs_node) link;
};

LIST_HEAD(list, efs_fs_node) fs_list = LIST_HEAD_INITIALIZER();

static int efs_fs_is_empty(const struct efs_fs *fs)
{
	//@todo
	//return -ENOTEMPTY;
	return 0;
}

void efs_get_ns_id(struct efs_fs *fs, uint16_t *ns_id)
{
	dassert(fs);
	ns_get_id(fs->ns, ns_id);
}

int efs_fs_lookup(const str256_t *name, struct efs_fs **fs)
{
	int rc = -ENOENT;
	struct efs_fs_node *fs_node;
	str256_t *fs_name = NULL;

	if (fs != NULL) {
		*fs = NULL;
	}

	LIST_FOREACH(fs_node, &fs_list, link) {
		ns_get_name(fs_node->efs_fs.ns, &fs_name);
		if (str256_cmp(name, fs_name) == 0) {
			rc = 0;
			if (fs != NULL) {
				*fs = &fs_node->efs_fs;
			}
			goto out;
		}
	}

out:
	log_debug(STR256_F " rc=%d", STR256_P(name), rc);
	return rc;
}

void fs_ns_scan_cb(struct namespace *ns, size_t ns_size)
{
	struct efs_fs_node *fs_node = NULL;

	fs_node = malloc(sizeof(struct efs_fs_node));
	if (fs_node == NULL) {
                log_err("Could not allocate memory for efs_fs_node");
		return;
	}

	fs_node->efs_fs.ns = malloc(ns_size);
	if (fs_node->efs_fs.ns == NULL) {
                log_err("Could not allocate memory for ns object");
		free(fs_node);
		return;
	}

	memcpy(fs_node->efs_fs.ns, ns, ns_size);

	fs_node->efs_fs.kvtree = NULL;

	LIST_INSERT_HEAD(&fs_list, fs_node, link);
}

static int endpoint_tenant_scan_cb(void *cb_ctx, struct tenant *tenant)
{
	int rc = 0;
	str256_t *endpoint_name = NULL;
	struct efs_fs *fs = NULL;
	struct efs_fs_node *fs_node = NULL;

	if (tenant == NULL) {
		rc = -ENOENT;
		goto out;
	}

	tenant_get_name(tenant, &endpoint_name);

	rc = efs_fs_lookup(endpoint_name, &fs);
	log_debug("FS for tenant " STR256_F " is %p, rc = %d",
		  STR256_P(endpoint_name), fs, rc);

	/* TODO: We don't have auto-recovery for such cases,
	* so that we just halt execution if we cannot find
	* the corresponding filesystem. Later on,
	* EFS should make an attempt to recover the filesystem,
	* and report to the user (using some alert mechanism
	* that can be implemented in CSM) in case if recovery
	* is not possible.
	*/
	if (rc != 0) {
		log_err("Tenant list and FS list is not consistent %d.", rc);
	}
	dassert(rc == 0);

	/* update fs_list */
	fs_node = container_of(fs, struct efs_fs_node, efs_fs);
	fs_node->efs_fs.tenant = tenant;
out:
	return rc;
}

int efs_fs_init(struct collection_item *cfg)
{
	int rc = 0;
	rc = ns_scan(fs_ns_scan_cb);
	log_debug("filesystem initialization, rc=%d", rc);
	return rc;
}

int efs_endpoint_init(struct collection_item *cfg_items)
{
	int rc = 0;

	RC_WRAP_LABEL(rc, out, tenant_scan, endpoint_tenant_scan_cb, NULL);
out:
	log_debug("endpoint initialization, rc=%d", rc);
	return rc;
}

int efs_endpoint_fini()
{
	int rc = 0;
	struct efs_fs_node *fs_node = NULL, *fs_node_ptr = NULL;

	LIST_FOREACH_SAFE(fs_node, &fs_list, link, fs_node_ptr) {
		fs_node->efs_fs.tenant = NULL;
	}

	log_debug("endpoint finalize, rc=%d", rc);
	return rc;
}

int efs_fs_fini()
{
	int rc = 0;
	struct efs_fs_node *fs_node = NULL, *fs_node_ptr = NULL;

	RC_WRAP_LABEL(rc, out, efs_endpoint_fini);
	LIST_FOREACH_SAFE(fs_node, &fs_list, link, fs_node_ptr) {
		LIST_REMOVE(fs_node, link);
		free(fs_node->efs_fs.ns);
		free(fs_node);
	}

out:
	log_debug("filesystem finalize, rc=%d", rc);
	return rc;
}

int efs_fs_scan_list(int (*fs_scan_cb)(const struct efs_fs_list_entry *list,
		     void *args), void *args)
{
	int rc = 0;
	struct efs_fs_node *fs_node = NULL;
	struct efs_fs_list_entry fs_entry;

	LIST_FOREACH(fs_node, &fs_list, link) {
		dassert(fs_node != NULL);
		efs_fs_get_name(&fs_node->efs_fs, &fs_entry.fs_name);
		efs_fs_endpoint_info(&fs_node->efs_fs,
				     (void **)&fs_entry.endpoint_info);
		dassert(fs_node->efs_fs.ns != NULL);
		RC_WRAP_LABEL(rc, out, fs_scan_cb, &fs_entry, args);
	}
out:
	return rc;
}

int efs_fs_create(const str256_t *fs_name)
{
        int rc = 0;
	struct namespace *ns;
	struct efs_fs_node *fs_node;
	size_t ns_size = 0;

	rc = efs_fs_lookup(fs_name, NULL);
        if (rc == 0) {
		log_err(STR256_F " already exist rc=%d\n",
			STR256_P(fs_name), rc);
		rc = -EEXIST;
		goto out;
        }

	/* create new node in fs_list */
	fs_node = malloc(sizeof(struct efs_fs_node));
	if (!fs_node) {
		rc = -ENOMEM;
		log_err("Could not allocate memory for efs_fs_node");
		goto out;
	}
	RC_WRAP_LABEL(rc, free_fs_node, ns_create, fs_name, &ns, &ns_size);

	// Attach ns to efs
	fs_node->efs_fs.ns = malloc(ns_size);
	memcpy(fs_node->efs_fs.ns, ns, ns_size);
	fs_node->efs_fs.tenant = NULL;

	/* @TODO This whole code of index open-close, efs_tree_create_root is
	 * repetitive and will be removed in second phase of kvtree */

	struct kvtree *kvtree = NULL;
	struct stat bufstat;

	/* Set stat */
	memset(&bufstat, 0, sizeof(struct stat));
	bufstat.st_mode = S_IFDIR|0777;
	bufstat.st_ino = EFS_ROOT_INODE;
	bufstat.st_nlink = 2;
	bufstat.st_uid = 0;
	bufstat.st_gid = 0;
	bufstat.st_atim.tv_sec = 0;
	bufstat.st_mtim.tv_sec = 0;
	bufstat.st_ctim.tv_sec = 0;

	RC_WRAP_LABEL(rc, free_info, kvtree_create, ns, (void *)&bufstat,
	              sizeof(struct stat), &kvtree);

	/* @TODO set inode num generator this FS after deletion of
	 * efs_tree_create_root */

	/* Attach kvtree pointer to efs struct */
	fs_node->efs_fs.kvtree = kvtree;

	rc = efs_tree_create_root(&fs_node->efs_fs);
free_info:

	if (rc == 0) {
		LIST_INSERT_HEAD(&fs_list, fs_node, link);
		goto out;
	}

free_fs_node:
	if (fs_node) {
		free(fs_node);
	}

out:
	log_info("fs_name=" STR256_F " rc=%d", STR256_P(fs_name), rc);
        return rc;
}

int efs_endpoint_create(const str256_t *endpoint_name, const char *endpoint_options)
{
	int rc = 0;
	uint16_t ns_id = 0;
	struct efs_fs_node *fs_node = NULL;
	struct efs_fs *fs = NULL;
	struct tenant *tenant;

	/* check file system exist */
	rc = efs_fs_lookup(endpoint_name, &fs);
	if (rc != 0) {
		log_err("Can't create endpoint for non existent fs");
		rc = -ENOENT;
		goto out;
	}

	if (fs->tenant != NULL ) {
		log_err("fs=" STR256_F " already exported",
			 STR256_P(endpoint_name));
		rc = -EEXIST;
		goto out;
	}

	/* get namespace ID */
	efs_get_ns_id(fs, &ns_id);

	/* TODO: At this position we should call the corresponding function
	* provided by some endpoint_ops virtual table to create export
	* at the protocol specific layer, for example to modify
	* the config file of NFS Ganesha service:
	* @code
	* struct endpoint_ops *e_ops = efs_get_registered_endpoint_operations(endpoint_options);
	* RC_WRAP_LABEL(rc, out, e_ops->on_create_export, endpoint_name, ns_id, endpoint_options);
	* @endcode
	* Right now, we don't have this code, so we are just dropping a warning.
	*/
	log_warn("Protocol-specific operation for creating export is not executed.");

	/* create tenant object */
	RC_WRAP_LABEL(rc, out, tenant_create, endpoint_name, &tenant,
		      ns_id, endpoint_options);

	/* update fs_list */
	fs_node = container_of(fs, struct efs_fs_node, efs_fs);
	fs_node->efs_fs.tenant = tenant;
	tenant = NULL;
	/* @todo call back to ganesha for dynamic export */

out:
	log_info("endpoint_name=" STR256_F " rc=%d", STR256_P(endpoint_name), rc);
	return rc;
}

int efs_endpoint_delete(const str256_t *endpoint_name)
{
	int rc = 0;
	uint16_t ns_id = 0;
	struct efs_fs *fs = NULL;
	struct efs_fs_node *fs_node = NULL;

	/* check endpoint exist */
	rc = efs_fs_lookup(endpoint_name, &fs);
	if (rc != 0) {
		log_err("Can not delete " STR256_F ". endpoint for non existent. fs",
			 STR256_P(endpoint_name));
		rc = -ENOENT;
		goto out;
	}

	if (fs->tenant == NULL) {
		log_err("Can not delete " STR256_F ". endpoint. Doesn't existent.",
			 STR256_P(endpoint_name));
		rc = -ENOENT;
		goto out;
	}

	/** TODO: check if FS is mounted.
	 * We should not remove the endpoint it is still
	 * "mounted" on one of the clients. Right now,
	 * we don't have a mechanism to check that,
	 * so that ignore this requirement.
	 */

	/* get namespace ID */
	efs_get_ns_id(fs, &ns_id);

	/* TODO: At this position we should call the corresponding function
	* provided by some endpoint_ops virtual table to create export
	* at the protocol specific layer, for example to modify
	* the config file of NFS Ganesha service:
	* @code
	* struct endpoint_ops *e_ops = efs_get_registered_endpoint_operations(endpoint_options);
	* RC_WRAP_LABEL(rc, out, e_ops->on_delete_export, endpoint_name, ns_id, endpoint_options);
	* @endcode
	* Right now, we don't have this code, so we are just dropping a warning.
	*/
	log_warn("Protocol-specific operation for deleting export is not executed.");

	/* delete tenant from nsal */
	fs_node = container_of(fs, struct efs_fs_node, efs_fs);
	RC_WRAP_LABEL(rc, out, tenant_delete, fs_node->efs_fs.tenant);

	/* Remove endpoint from the fs list */
	free(fs_node->efs_fs.tenant);
	fs_node->efs_fs.tenant= NULL;

out:
	log_info("endpoint_name=" STR256_F " rc=%d", STR256_P(endpoint_name), rc);
	return rc;
}

int efs_fs_delete(const str256_t *fs_name)
{
	int rc = 0;
	struct efs_fs *fs;
	struct efs_fs_node *fs_node = NULL;

	rc = efs_fs_lookup(fs_name, &fs);
	if (rc != 0) {
		log_err("Can not delete " STR256_F ". FS doesn't exists.",
			STR256_P(fs_name));
		goto out;
	}

	if (fs->tenant != NULL) {
		log_err("Can not delete exported " STR256_F ". Fs",
			STR256_P(fs_name));
		rc = -EINVAL;
		goto out;
	}

	rc = efs_fs_is_empty(fs);
	if (rc != 0) {
		log_err("Can not delete FS " STR256_F ". It is not empty",
			STR256_P(fs_name));
		goto out;
	}

	RC_WRAP_LABEL(rc, out, efs_tree_delete_root, fs);

	/* delete kvtree */
	RC_WRAP_LABEL(rc, out, kvtree_delete, fs->kvtree);
	fs->kvtree = NULL;

	/* Remove fs from the efs list */
	fs_node = container_of(fs, struct efs_fs_node, efs_fs);
	LIST_REMOVE(fs_node, link);

	RC_WRAP_LABEL(rc, out, ns_delete, fs->ns);
	fs->ns = NULL;

out:
	log_info("fs_name=" STR256_F " rc=%d", STR256_P(fs_name), rc);
	return rc;
}

void efs_fs_get_name(const struct efs_fs *fs, str256_t **name)
{
	dassert(fs);
	ns_get_name(fs->ns, name);
}

void efs_fs_endpoint_info(const struct efs_fs *fs, void **info)
{
	dassert(fs);
	if (fs->tenant == NULL) {
		*info = NULL;
	} else {
		tenant_get_info(fs->tenant, info);
	}
}

int efs_fs_open(const char *fs_name, struct efs_fs **ret_fs)
{
	int rc;
	struct efs_fs *fs = NULL;
	kvs_idx_fid_t ns_fid;
	str256_t name;

	str256_from_cstr(name, fs_name, strlen(fs_name));
	rc = efs_fs_lookup(&name, &fs);
	if (rc != 0) {
		log_err(STR256_F " FS not found rc=%d",
				STR256_P(&name), rc);
		rc = -ENOENT;
		goto error;
	}

	ns_get_fid(fs->ns, &ns_fid);
	//RC_WRAP_LABEL(rc, error, kvs_index_open, kvstor, &ns_fid, index);

	if (fs->kvtree == NULL) {
		fs->kvtree = malloc(sizeof(struct kvtree));
		if (!fs->kvtree) {
			rc = -ENOMEM;
			goto out;
		}
	}
	rc = kvtree_init(fs->ns, fs->kvtree);
	*ret_fs = fs;

error:
	if (rc != 0) {
		log_err("Cannot open fid for fs_name=%s, rc:%d", fs_name, rc);
	}

out:
	return rc;
}

void efs_fs_close(struct efs_fs *efs_fs)
{
	kvtree_fini(efs_fs->kvtree);

}
