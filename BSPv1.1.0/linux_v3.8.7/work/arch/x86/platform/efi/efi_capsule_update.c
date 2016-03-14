/*
 * Copyright(c) 2013-2015 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */
#define DEBUG
#include <asm/qrk.h>
#include <linux/errno.h>
#include <linux/firmware.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/efi.h>

#define DRIVER_NAME	"efi_capsule_update"
#define PFX		"efi-capsupdate: "
#define MAX_PATH	256
#define MAX_CHUNK	PAGE_SIZE
#define CSH_HDR_SIZE	0x400

typedef struct {
	u64 length;
	union {
		u64 data_block;
		u64 continuation_pointer;
	};
} efi_blk_desc_t;

static struct kobject * efi_capsule_kobj;
static struct device *dev;
static struct list_head sg_list;
static char fpath[MAX_PATH];
static bool path_set = false;
static int csh_jump = CSH_HDR_SIZE;		/* Quark EDK wants CSH jump */

/**
 * efi_capsule_trigger_update
 *
 * Trigger the EFI capsule update
 */
static int efi_capsule_trigger_update(void)
{
	const struct firmware *fw_entry;
	int ret = 0;
	u32 nblocks = 0, i = 0, total_size = 0, data_len = 0, offset = 0;
	efi_capsule_header_t *chdr = NULL;
	efi_blk_desc_t * desc_block = NULL;
	u8 * data = NULL;

	if (path_set == false)
		return -ENODEV;

	ret = request_firmware(&fw_entry, fpath, dev);
	if (ret || fw_entry == NULL){
		pr_err(PFX"unable to load firmware %s\n", fpath);
		return ret;
	}

	/* Determine necessary sizes */
	nblocks = (fw_entry->size/MAX_CHUNK) + 2;
	total_size = fw_entry->size;

	/* Allocate array of descriptor blocks + 1 for terminator */
	desc_block = (efi_blk_desc_t*)kzalloc(nblocks * sizeof(efi_blk_desc_t), GFP_KERNEL);
	if (desc_block == NULL){
		pr_info(PFX"%s failed to allocate %d blocks\n", __func__, nblocks);
		ret = -ENOMEM;
		goto done_close;
	}

	pr_info(PFX"File %s size %u descriptor blocks %u\n",
		fpath, total_size, nblocks);

	/* Read in data */
	for (i = 0; i < nblocks && offset < total_size; i++){
		/* Determine read len */
		data_len = offset < total_size - MAX_CHUNK ?
				MAX_CHUNK : total_size - offset;
		data = kmalloc(MAX_CHUNK, GFP_KERNEL);
		if (data == NULL){
			ret = -ENOMEM;
			pr_info("Alloc fail %d bytes entry %d\n",
				nblocks, i);
			goto done;
		}
		memcpy(data, fw_entry->data + offset, data_len);
		offset += data_len;

		/* Sanity check */
		if (i >= nblocks){
			pr_err(PFX"%s Driver bug line %d\n", __func__, __LINE__);
			ret = -EINVAL;
			goto done;
		}

		/* Validate header as appropriate */
		if (chdr == NULL){
			chdr = (efi_capsule_header_t*)&data[csh_jump];
			desc_block[i].data_block = __pa(&data[csh_jump]);
			desc_block[i].length = data_len - csh_jump;
			pr_debug(PFX"hdr offset in file %d bytes\n", csh_jump);
			pr_debug(PFX"hdr size %u flags 0x%08x imagesize 0x%08x\n",
				chdr->headersize, chdr->flags, chdr->imagesize);

		}else{
			desc_block[i].data_block = __pa(data);
			desc_block[i].length = data_len;
		}
		pr_debug(PFX "block %d length %u data @ phys 0x%08x virt %x\n",
			i, (int)desc_block[i].length,
			(unsigned int)desc_block[i].data_block, (unsigned int)data);
	}

	if (i > nblocks-1){
		pr_err(PFX"%s Used block %d expected %d !\n", __func__, i, nblocks-1);
		ret = -EINVAL;
		goto done;
	}

	pr_debug(PFX"submitting capsule to EDKII firmware\n");
 
	ret = efi.update_capsule(&chdr, 1, __pa(desc_block));
	if(ret != EFI_SUCCESS) {
		pr_err(PFX"submission fail err=0x%08x\n", ret);
	}else{
		pr_debug(PFX"submission success\n");
		ret = 0;
	}

	if (chdr != NULL && chdr->flags & 0x10000){
		pr_debug(PFX"capsule persist across S3 skipping capsule free\n");
		goto done_close;
	}
done:

	for (i = 0; i < nblocks; i++){
		if (desc_block[i].data_block != 0)
			kfree(phys_to_virt((u32)desc_block[i].data_block));
	}

	if (desc_block != NULL)
		kfree(desc_block);
done_close:
	release_firmware(fw_entry);
	return ret;
}

/**
 * efi_capsule_csh_jump
 *
 * sysfs callback used to show current path
 */
static ssize_t efi_capsule_csh_jump_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
        return snprintf(buf, sizeof(fpath), "%d\n", csh_jump > 0);
}

/**
 * efi_capsule_path_store
 *
 * sysfs callback used to set a new capsule path
 */
static ssize_t efi_capsule_csh_jump_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	if (buf != NULL && buf[0] == '0')
		csh_jump = 0;
	else
		csh_jump = CSH_HDR_SIZE;
	return count;
}

static struct kobj_attribute efi_capsule_csh_jump_attr =
        __ATTR(csh_jump, 0644, efi_capsule_csh_jump_show, efi_capsule_csh_jump_store);

/**
 * efi_capsule_path_show
 *
 * sysfs callback used to show current path
 */
static ssize_t efi_capsule_path_show(struct kobject *kobj,
                                struct kobj_attribute *attr, char *buf)
{
        return snprintf(buf, sizeof(fpath), fpath);
}

/**
 * efi_capsule_path_store
 *
 * sysfs callback used to set a new capsule path
 */
static ssize_t efi_capsule_path_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	if (count > MAX_PATH-1)
		return -EINVAL;

	memset(fpath, 0x00, sizeof(fpath));
	memcpy(fpath, buf, count);
	path_set = true;

	return count;
}

static struct kobj_attribute efi_capsule_path_attr =
        __ATTR(capsule_path, 0644, efi_capsule_path_show, efi_capsule_path_store);

/**
 * efi_capsule_update_store
 *
 * sysfs callback used to initiate update
 */
static ssize_t efi_capsule_update_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{	int ret = 0;

	ret = efi_capsule_trigger_update();
	return ret == 0 ? count : ret;
}

static struct kobj_attribute efi_capsule_update_attr =
        __ATTR(capsule_update, 0644, NULL, efi_capsule_update_store);

static void efi_capsule_device_release(struct device *dev)
{
	kfree(dev);
}

#define SYSFS_ERRTXT "Error adding sysfs entry!\n"
/**
 * intel_qrk_capsule_update_init
 *
 * @return 0 success < 0 failure
 *
 * Module entry point
 */
static int __init efi_capsule_update_init(void)
{
	int retval = 0;
	extern struct kobject * firmware_kobj;

	INIT_LIST_HEAD(&sg_list);

	/* efi_capsule_kobj subordinate of firmware @ /sys/firmware/efi */
	efi_capsule_kobj = kobject_create_and_add("efi_capsule", firmware_kobj);
	if (!efi_capsule_kobj) {
		pr_err(PFX"kset create error\n");
		retval = -ENODEV;
		goto err;
	}

	dev = kzalloc(sizeof(struct device), GFP_KERNEL);
	if (!dev) {
		retval = -ENOMEM;
		goto err_name;
	}

	retval = dev_set_name(dev, "%s", DRIVER_NAME);
	if (retval < 0){
		pr_err(PFX"dev_set_name err\n");
		goto err_dev_reg;
	}

	dev->kobj.parent = efi_capsule_kobj;
	dev->groups = NULL;
	dev->release = efi_capsule_device_release;

	retval = device_register(dev);
	if (retval < 0){
		pr_err(PFX"device_register error\n");
		goto err_dev_reg;
	}

	if(sysfs_create_file(efi_capsule_kobj, &efi_capsule_path_attr.attr)) {
		pr_err(PFX SYSFS_ERRTXT);
		retval = -ENODEV;
		goto err_dev_reg;
	}
	if(sysfs_create_file(efi_capsule_kobj, &efi_capsule_update_attr.attr)) {
		pr_err(PFX SYSFS_ERRTXT);
		retval = -ENODEV;
		goto err_dev_reg;

	}
	if(sysfs_create_file(efi_capsule_kobj, &efi_capsule_csh_jump_attr.attr)) {
		pr_err(PFX SYSFS_ERRTXT);
		retval = -ENODEV;
		goto err_dev_reg;

	}
	return 0;

err_dev_reg:
	put_device(dev);
err_name:
	kfree(dev);
err:
	return retval;
}

/**
 * intel_qrk_esram_exit
 *
 * Module exit
 */
static void __exit efi_capsule_update_exit(void)
{
}

MODULE_AUTHOR("Bryan O'Donoghue <bryan.odonoghue@intel.com>");
MODULE_DESCRIPTION("EFI Capsule Update driver");
MODULE_LICENSE("Dual BSD/GPL");

module_init(efi_capsule_update_init);
module_exit(efi_capsule_update_exit);
