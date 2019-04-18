/*
 * Configfs entries for device-tree
 *
 * Copyright (C) 2013 - Pantelis Antoniou <panto@antoniou-consulting.com>
 *
 * Driver extracted from: OF: DT-Overlay configfs interface (v2)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#define DEBUG
#include <linux/ctype.h>
#include <linux/cpu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/spinlock.h>
#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/configfs.h>
#include <linux/types.h>
#include <linux/stat.h>
#include <linux/limits.h>
#include <linux/file.h>
#include <linux/vmalloc.h>
#include <linux/firmware.h>

#include "of_private.h"

struct cfs_overlay_item {
	struct config_item	item;

	char			path[PATH_MAX];

	const struct firmware	*fw;
	struct device_node	*overlay;
	int			ov_id;

	void			*dtbo;
	int			dtbo_size;
};

static int create_overlay(struct cfs_overlay_item *overlay, void *blob)
{
	int ret;

	/* unflatten the tree */
	of_fdt_unflatten_tree((void *)blob, NULL, &overlay->overlay);
	if (overlay->overlay == NULL) {
		pr_err("%s: failed to unflatten tree\n", __func__);
		return -EINVAL;
	}
	pr_debug("%s: unflattened OK\n", __func__);

	/* mark it as detached */
	of_node_set_flag(overlay->overlay, OF_DETACHED);

	/* perform resolution */
	ret = of_resolve_phandles(overlay->overlay);
	if (ret != 0) {
		pr_err("%s: Failed to resolve tree\n", __func__);
		return ret;
	}
	pr_debug("%s: resolved OK\n", __func__);

	ret = of_overlay_fdt_apply(overlay->overlay, overlay->dtbo_size, &overlay->ov_id);
	if (ret < 0) {
		pr_err("%s: Failed to create overlay (err=%d)\n",
				__func__, ret);
		return ret;
	}

	return 0;
}

static inline struct cfs_overlay_item *to_cfs_overlay_item(
		struct config_item *item)
{
	return item ? container_of(item, struct cfs_overlay_item, item) : NULL;
}


static ssize_t cfs_overlay_item_path_show(struct config_item *item,
		char *buf)
{
	struct cfs_overlay_item *overlay = to_cfs_overlay_item(item);
	return sprintf(buf, "%s\n", overlay->path);
}

static ssize_t cfs_overlay_item_path_store(struct config_item *item,
		const char *buf, size_t count)
{
	const char *p = buf;
	char *s;
	int err;
	struct cfs_overlay_item *overlay = to_cfs_overlay_item(item);

	/* if it's set do not allow changes */
	if (overlay->path[0] != '\0' || overlay->dtbo_size > 0)
		return -EPERM;

	/* copy to path buffer (and make sure it's always zero terminated */
	count = snprintf(overlay->path, sizeof(overlay->path) - 1, "%s", p);
	overlay->path[sizeof(overlay->path) - 1] = '\0';


	/* strip trailing newlines */
	s = overlay->path + strlen(overlay->path);
	while (s > overlay->path && *--s == '\n')
		*s = '\0';

	pr_debug("%s: path is '%s'\n", __func__, overlay->path);

	err = request_firmware(&overlay->fw, overlay->path, NULL);
	if (err != 0) {
		pr_err("%s: Can not request firmware\n", __func__);
		goto out_err;
	}

	err = create_overlay(overlay, (void *)overlay->fw->data);
	if (err != 0){
		pr_err("%s: Can not create overlay\n", __func__);
		goto out_err;
	}

	return count;

out_err:

	release_firmware(overlay->fw);
	overlay->fw = NULL;

	overlay->path[0] = '\0';
	return err;
}

CONFIGFS_ATTR(, cfs_overlay_item_path);

static ssize_t cfs_overlay_item_status_show(struct config_item *item,
		char *buf)
{
	struct cfs_overlay_item *overlay = to_cfs_overlay_item(item);

	return sprintf(buf, "%s\n",
			overlay->ov_id >= 0 ? "applied" : "unapplied");
}

CONFIGFS_ATTR_RO(, cfs_overlay_item_status);

static struct configfs_attribute *cfs_overlay_attrs[] = {
	&attr_cfs_overlay_item_path,
	&attr_cfs_overlay_item_status,
	NULL,
};

static void cfs_overlay_release(struct config_item *item)
{
	struct cfs_overlay_item *overlay = to_cfs_overlay_item(item);

	if (overlay->ov_id >= 0)
		of_overlay_remove(&overlay->ov_id);
	if (overlay->fw)
		release_firmware(overlay->fw);
	/* kfree with NULL is safe */
	kfree(overlay->dtbo);
	kfree(overlay);
}

static struct configfs_item_operations cfs_overlay_item_ops = {
	.release		= cfs_overlay_release,
};

static struct config_item_type cfs_overlay_type = {
	.ct_item_ops	= &cfs_overlay_item_ops,
	.ct_attrs	= cfs_overlay_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct config_item *cfs_overlay_group_make_item(
		struct config_group *group, const char *name)
{
	struct cfs_overlay_item *overlay;

	overlay = kzalloc(sizeof(*overlay), GFP_KERNEL);
	if (!overlay)
		return ERR_PTR(-ENOMEM);
	overlay->ov_id = -1;

	config_item_init_type_name(&overlay->item, name, &cfs_overlay_type);
	return &overlay->item;
}

static void cfs_overlay_group_drop_item(struct config_group *group,
		struct config_item *item)
{
	struct cfs_overlay_item *overlay = to_cfs_overlay_item(item);

	config_item_put(&overlay->item);
}

static struct configfs_group_operations overlays_ops = {
	.make_item	= cfs_overlay_group_make_item,
	.drop_item	= cfs_overlay_group_drop_item,
};

static struct config_item_type overlays_type = {
	.ct_group_ops   = &overlays_ops,
	.ct_owner       = THIS_MODULE,
};

static struct configfs_group_operations of_cfs_ops = {
	/* empty - we don't allow anything to be created */
};

static struct config_item_type of_cfs_type = {
	.ct_group_ops   = &of_cfs_ops,
	.ct_owner       = THIS_MODULE,
};

static struct configfs_subsystem of_cfs_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "device-tree",
			.ci_type = &of_cfs_type,
		},
	},
	.su_mutex = __MUTEX_INITIALIZER(of_cfs_subsys.su_mutex),
};

static int __init of_cfs_init(void)
{
	int ret;

	pr_info("%s\n", __func__);

	config_group_init_type_name(&of_cfs_subsys.su_group, "overlays",
			&overlays_type);

	ret = configfs_register_subsystem(&of_cfs_subsys);
	if (ret != 0) {
		pr_err("%s: failed to register subsys\n", __func__);
		goto out;
	}
	pr_info("%s: OK\n", __func__);
out:
	return ret;
}
module_init(of_cfs_init);

static void __exit of_cfs_exit(void)
{
	pr_info("%s\n", __func__);

	configfs_unregister_subsystem(&of_cfs_subsys);
}
module_exit(of_cfs_exit);

MODULE_ALIAS("configfs:overlay");
MODULE_AUTHOR("Stefan Eichenberger");
MODULE_LICENSE("GPL");
