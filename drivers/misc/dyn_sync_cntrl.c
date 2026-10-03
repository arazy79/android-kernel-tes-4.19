/*
 * Dynamic sync control driver V2 (modif - no state notifier)
 * by andip71, ported by user
 */

#include <linux/module.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/writeback.h>
#include <linux/dyn_sync_cntrl.h>

#define DYN_FSYNC_ACTIVE_DEFAULT true
#define DYN_FSYNC_VERSION_MAJOR 2
#define DYN_FSYNC_VERSION_MINOR 1

static DEFINE_MUTEX(fsync_mutex);

bool suspend_active = false;
bool dyn_fsync_active = DYN_FSYNC_ACTIVE_DEFAULT;

extern void sync_filesystems(int wait);

static void dyn_fsync_force_flush(void)
{
	sync_filesystems(0);
	sync_filesystems(1);
}

/* Export buat dipanggil dari suspend.c */
void dyn_fsync_suspend(void)
{
	mutex_lock(&fsync_mutex);
	suspend_active = true;
	mutex_unlock(&fsync_mutex);
}
EXPORT_SYMBOL(dyn_fsync_suspend);

void dyn_fsync_resume(void)
{
	mutex_lock(&fsync_mutex);
	suspend_active = false;
	if (dyn_fsync_active)
		dyn_fsync_force_flush();
	mutex_unlock(&fsync_mutex);
}
EXPORT_SYMBOL(dyn_fsync_resume);

static ssize_t dyn_fsync_active_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", (dyn_fsync_active ? 1 : 0));
}

static ssize_t dyn_fsync_active_store(struct kobject *kobj,
		struct kobj_attribute *attr, const char *buf, size_t count)
{
	unsigned int data;

	if (sscanf(buf, "%u\n", &data) == 1) {
		if (data == 1) {
			pr_info("%s: dynamic fsync enabled\n", __func__);
			dyn_fsync_active = true;
		} else if (data == 0) {
			pr_info("%s: dynamic fsync disabled\n", __func__);
			dyn_fsync_active = false;
			dyn_fsync_force_flush();
		} else {
			pr_info("%s: bad value: %u\n", __func__, data);
		}
	} else {
		pr_info("%s: unknown input!\n", __func__);
	}
	return count;
}

static ssize_t dyn_fsync_version_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "version: %u.%u\n",
		DYN_FSYNC_VERSION_MAJOR,
		DYN_FSYNC_VERSION_MINOR);
}

static ssize_t dyn_fsync_suspend_show(struct kobject *kobj,
		struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "suspend active: %u\n", suspend_active);
}

static int dyn_fsync_panic_event(struct notifier_block *this,
		unsigned long event, void *ptr)
{
	suspend_active = false;
	dyn_fsync_force_flush();
	pr_warn("dynamic fsync: panic - force flush!\n");
	return NOTIFY_DONE;
}

static int dyn_fsync_notify_sys(struct notifier_block *this, unsigned long code,
				void *unused)
{
	if (code == SYS_DOWN || code == SYS_HALT || code == SYS_POWER_OFF) {
		suspend_active = false;
		dyn_fsync_active = false;
		dyn_fsync_force_flush();
		pr_warn("dynamic fsync: reboot - force flush!\n");
	}
	return NOTIFY_DONE;
}

static struct notifier_block dyn_fsync_notifier = {
	.notifier_call = dyn_fsync_notify_sys,
};

static struct notifier_block dyn_fsync_panic_block = {
	.notifier_call = dyn_fsync_panic_event,
	.priority = INT_MAX,
};

static struct kobj_attribute dyn_fsync_active_attribute =
	__ATTR(Dyn_fsync_active, 0664,
		dyn_fsync_active_show,
		dyn_fsync_active_store);

static struct kobj_attribute dyn_fsync_version_attribute =
	__ATTR(Dyn_fsync_version, 0444, dyn_fsync_version_show, NULL);

static struct kobj_attribute dyn_fsync_suspend_attribute =
	__ATTR(Dyn_fsync_suspend, 0444, dyn_fsync_suspend_show, NULL);

static struct attribute *dyn_fsync_active_attrs[] = {
	&dyn_fsync_active_attribute.attr,
	&dyn_fsync_version_attribute.attr,
	&dyn_fsync_suspend_attribute.attr,
	NULL,
};

static struct attribute_group dyn_fsync_active_attr_group = {
	.attrs = dyn_fsync_active_attrs,
};

static struct kobject *dyn_fsync_kobj;

static int __init dyn_fsync_init(void)
{
	int sysfs_result;

	register_reboot_notifier(&dyn_fsync_notifier);
	atomic_notifier_chain_register(&panic_notifier_list,
		&dyn_fsync_panic_block);

	dyn_fsync_kobj = kobject_create_and_add("dyn_fsync", kernel_kobj);
	if (!dyn_fsync_kobj) {
		pr_err("%s dyn_fsync_kobj create failed!\n", __func__);
		return -ENOMEM;
	}

	sysfs_result = sysfs_create_group(dyn_fsync_kobj,
			&dyn_fsync_active_attr_group);
	if (sysfs_result) {
		pr_err("%s dyn_fsync sysfs create failed!\n", __func__);
		kobject_put(dyn_fsync_kobj);
	}

	pr_info("%s dynamic fsync initialisation complete\n", __func__);
	return sysfs_result;
}

static void __exit dyn_fsync_exit(void)
{
	unregister_reboot_notifier(&dyn_fsync_notifier);
	atomic_notifier_chain_unregister(&panic_notifier_list,
		&dyn_fsync_panic_block);
	if (dyn_fsync_kobj != NULL)
		kobject_put(dyn_fsync_kobj);
	pr_info("%s dynamic fsync unregistration complete\n", __func__);
}

module_init(dyn_fsync_init);
module_exit(dyn_fsync_exit);

MODULE_AUTHOR("andip71");
MODULE_DESCRIPTION("dynamic fsync - automatic fs sync optimization");
MODULE_LICENSE("GPL v2");
