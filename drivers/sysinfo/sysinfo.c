/*
 * Sample kobject implementation
 *
 * Copyright (C) 2004-2007 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (C) 2013 Wistron Inc.
 *
 * Released under the GPL version 2 only.
 *
 */
#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <../gpio-names.h>	
#include "../../arch/arm/mach-tegra/board.h"

#define CMD_LINE_LENGTH 1024

static struct proc_dir_entry *proc_sysinfo_dir;
static struct odm_info oi;

static int proc_bootloader_ver_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n" , oi.bootloader_ver);
	return 0;
}

static int proc_kernel_ver_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%s\n" , oi.kernel_ver);	
	return 0;
}

static ssize_t proc_sn_show(struct seq_file *m, void *v)
{
	mm_segment_t fs;
	struct file *fp; 
	char boot_buf[1024];
	char sn[1024];
	
	fp = filp_open("/sys/bus/i2c/devices/0-0056/sn" ,O_RDONLY ,0 );
	fs = get_fs();
	set_fs(get_ds());
	if(fp != NULL)
	{
		fp->f_op->read(fp, boot_buf, CMD_LINE_LENGTH, &fp->f_pos);	
	}
	else  
	{
		printk("open file fail!\n");
		
	}
	
	set_fs(fs);
	strcpy(sn,boot_buf);
	seq_printf(m, "%s\n" , sn);
	filp_close(fp, NULL);
	return 0;
}

static ssize_t proc_pcb_ver_show(struct seq_file *m, void *v)
{
	const char *pcb_ver_name[10] = {"EVT", "DVT1", "DVT2", "PVT", "MP", "TBD", "TBD"};
	seq_printf(m, "%s\n" , pcb_ver_name[oi.pcb_ver]);
	return 0;
}

static ssize_t proc_ddr_sku_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", oi.ddr_sku);
	return 0;
}

static ssize_t proc_os_ver_show(struct seq_file *m, void *v)
{
    seq_printf(m, "%d\n" ,DORA_OS_VERSION);

	return 0;
}

static ssize_t proc_wcis_show(struct seq_file *m, void *v)
{
	mm_segment_t fs;
	struct file *fp; 
	char boot_buf[1024]={'\0'};
	char wcis[1024]={'\0'};
	
	fp = filp_open("/sys/bus/i2c/devices/0-0056/wcis" ,O_RDONLY ,0 );
	fs = get_fs();
	set_fs(get_ds());
	if(fp != NULL)
	{
		fp->f_op->read(fp, boot_buf, CMD_LINE_LENGTH, &fp->f_pos);	
	}
	else  
	{
		printk("open file fail!\n");
		
	}
	
	set_fs(fs);
	strcpy(wcis,boot_buf);
    seq_printf(m, "%s\n" , wcis);
	filp_close(fp, NULL);
    return 0;
}

static ssize_t proc_manufacturer_show(struct seq_file *m, void *v)
{
    seq_printf(m, "%s\n" ,oi.manufacturer);

	return 0;
}

static int open_kernel_ver_show(struct inode *inode, struct file *file)	
{	
	return single_open(file, proc_kernel_ver_show, NULL);	
}

static int open_sn_show(struct inode *inode, struct file *file)	
{	
	return single_open(file, proc_sn_show, NULL);	
}	
			
static int open_bootloader_ver_show(struct inode *inode, struct file *file)	
{	
	return single_open(file, proc_bootloader_ver_show, NULL);
}

static int open_pcb_ver_show(struct inode *inode, struct file *file)	
{	
	return single_open(file, proc_pcb_ver_show, NULL);	
}

static int open_ddr_sku_show(struct inode *inode, struct file *file)	
{	
	return single_open(file, proc_ddr_sku_show, NULL);	
}

static int open_os_ver_show(struct inode *inode, struct file *file)	
{	
	return single_open(file, proc_os_ver_show, NULL);	
}

static int open_wcis_show(struct inode *inode, struct file *file)	
{	
	return single_open(file, proc_wcis_show, NULL);	
}

static int open_manufacturer_show(struct inode *inode, struct file *file)	
{	
	return single_open(file, proc_manufacturer_show, NULL);	
}

static struct kobj_attribute bootloader_ver_attribute =
	__ATTR(bootloader_ver, 0664, open_bootloader_ver_show, NULL);
	
static struct kobj_attribute kernel_ver_attribute =
	__ATTR(kernel_ver, 0664, open_kernel_ver_show, NULL);
	
static struct kobj_attribute sn_attribute =
	__ATTR(sn, 0664, open_sn_show, NULL);

static struct kobj_attribute pcb_ver_attribute =
	__ATTR(pcb_ver, 0664, open_pcb_ver_show, NULL);

static struct kobj_attribute ddr_sku_attribute =
	__ATTR(ddr_sku, 0664, open_ddr_sku_show, NULL);

static struct kobj_attribute os_ver_attribute =
	__ATTR(os_version, 0664, open_os_ver_show, NULL);

static struct kobj_attribute wcis_attribute =
	__ATTR(WCIS, 0664, open_wcis_show, NULL);
	
static struct kobj_attribute manufacturer_attribute =
	__ATTR(manufacturer, 0664, open_manufacturer_show, NULL);

/*
 * Create a group of attributes so that we can create and destory them all
 * at once.
 */
static struct attribute *attrs[] = {
	&bootloader_ver_attribute.attr,
	&kernel_ver_attribute.attr,
	&sn_attribute.attr,
	&pcb_ver_attribute.attr,
	&ddr_sku_attribute.attr,
	&os_ver_attribute.attr,
	&wcis_attribute.attr,
	&manufacturer_attribute.attr,
	NULL,   /* need to NULL terminate the list of attributes */
};

/*
 * An unnamed attribute group will put all of the attributes directly in
 * the kobject directory.  If we specify a name, a subdirectory will be
 * created for the attributes with the directory being the name of the
 * attribute group.
 */
static struct attribute_group attr_group = {
    .attrs = attrs,
};

static struct kobject *sysinfo_kobj;

static const struct file_operations proc_bootloader_ver_operations = {	
    .open       = open_bootloader_ver_show,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = seq_release,
};

static const struct file_operations proc_kernel_ver_operations = {	
    .open       = open_kernel_ver_show,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = seq_release,
};

static const struct file_operations proc_sn_operations = {	
    .open       = open_sn_show,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = seq_release,
};

static const struct file_operations proc_pcb_ver_operations = {	
    .open       = open_pcb_ver_show,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = seq_release,
};

static const struct file_operations proc_ddr_sku_operations = {	
    .open       = open_ddr_sku_show,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = seq_release,
};

static const struct file_operations proc_os_ver_operations = {	
    .open       = open_os_ver_show,
    .read       = seq_read,
    .llseek     = seq_lseek,
    .release    = seq_release,
};

static const struct file_operations proc_wcis_operations = {	
    .open       = open_wcis_show,
    .read       = seq_read,	
    .llseek     = seq_lseek,	
    .release    = seq_release,
};

static const struct file_operations proc_manufacturer_operations = {	
    .open       = open_manufacturer_show,
    .read       = seq_read,	
    .llseek     = seq_lseek,	
    .release    = seq_release,
};

static int sysinfo_init(void)
{
	int retval;

	/*
	 * Create a simple kobject with the name of "kobject_example",
	 * located under /sys/kernel/
	 *
	 * As this is a simple directory, no uevent will be sent to
	 * userspace.  That is why this function should not be used for
	 * any type of dynamic kobjects, where the name and number are
	 * not known ahead of time.
	 */
	sysinfo_kobj = kobject_create_and_add("sysinfo", NULL);
	if (!sysinfo_kobj)
        return -ENOMEM;

	/* Create the files associated with this kobject */
	retval = sysfs_create_group(sysinfo_kobj, &attr_group);
	if (retval)
		kobject_put(sysinfo_kobj);
        
	proc_sysinfo_dir = proc_mkdir("sysinfo", NULL);

	//sysinfo members
	dora_get_odm_info(&oi);
	proc_create("sysinfo/kernel_ver", 0, NULL, &proc_kernel_ver_operations);
	proc_create("sysinfo/bootloader_ver", 0, NULL, &proc_bootloader_ver_operations);
	proc_create("sysinfo/serial", 0, NULL, &proc_sn_operations);
	proc_create("sysinfo/pcb_ver", 0, NULL, &proc_pcb_ver_operations);
	proc_create("sysinfo/ddr_sku", 0, NULL, &proc_ddr_sku_operations);
	proc_create("sysinfo/os_version", 0, NULL, &proc_os_ver_operations);
	proc_create("sysinfo/WCIS", 0, NULL, &proc_wcis_operations);
	proc_create("sysinfo/manufacturer", 0, NULL, &proc_manufacturer_operations);

	return retval;
}

static void sysinfo_exit(void)
{
	kobject_put(sysinfo_kobj);
}

module_init(sysinfo_init);
module_exit(sysinfo_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Greg Kroah-Hartman <greg@kroah.com>");
