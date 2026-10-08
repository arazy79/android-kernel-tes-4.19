/*
 * Auto-cut charging module for X00TD
 * Sysfs: /sys/module/autocut/parameters/max_soc
 *        /sys/module/autocut/parameters/min_soc
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/workqueue.h>
#include <linux/power_supply.h>

static int max_soc = 100;
static int min_soc = 70;
module_param(max_soc, int, 0644);
MODULE_PARM_DESC(max_soc, "Max SOC to stop charging (default 100)");
module_param(min_soc, int, 0644);
MODULE_PARM_DESC(min_soc, "Min SOC to resume charging (default 90)");

static struct delayed_work autocut_work;
static bool last_usb_present = false;

static void autocut_work_fn(struct work_struct *work)
{
	struct power_supply *psy_batt, *psy_usb;
	union power_supply_propval val;
	int ret, capacity, charging;
	bool usb_present = false;

	/* Cek apakah charger/USB nyolok */
	psy_usb = power_supply_get_by_name("usb");
	if (psy_usb) {
		ret = power_supply_get_property(psy_usb, POWER_SUPPLY_PROP_PRESENT, &val);
		if (!ret)
			usb_present = val.intval;
		power_supply_put(psy_usb);
	}

	/* Ambil battery power supply */
	psy_batt = power_supply_get_by_name("battery");
	if (!psy_batt)
		goto reschedule;

	/* Baca capacity */
	ret = power_supply_get_property(psy_batt, POWER_SUPPLY_PROP_CAPACITY, &val);
	if (ret)
		goto put_batt;
	capacity = val.intval;

	/* Baca status charging_enabled */
	ret = power_supply_get_property(psy_batt, POWER_SUPPLY_PROP_INPUT_SUSPEND, &val);
	if (ret)
		goto put_batt;
	charging = val.intval;

	if (usb_present) {
		if (capacity >= max_soc && charging) {
			/* Nyampe max → STOP */
			val.intval = 0;
			power_supply_set_property(psy_batt, POWER_SUPPLY_PROP_INPUT_SUSPEND, &val);
			pr_info("autocut: charging STOPPED at %d%% (max=%d)\n", capacity, max_soc);

		} else if (!charging && capacity < max_soc) {
			/* Charger nyolok tapi charging mati.
			 * Resume kalau:
			 * 1. Baterai turun ke min_soc (charger nyolok terus), ATAU
			 * 2. Charger BARU dicolok (bypass min_soc)
			 */
			if (capacity <= min_soc || !last_usb_present) {
				val.intval = 1;
				power_supply_set_property(psy_batt, POWER_SUPPLY_PROP_INPUT_SUSPEND, &val);
				pr_info("autocut: charging RESUMED at %d%% (reason=%s)\n",
					capacity,
					!last_usb_present ? "reconnect" : "min_soc");
			}
		}
	}

	last_usb_present = usb_present;

put_batt:
	power_supply_put(psy_batt);
reschedule:
	schedule_delayed_work(&autocut_work, msecs_to_jiffies(1000));
}

static int __init autocut_init(void)
{
	INIT_DELAYED_WORK(&autocut_work, autocut_work_fn);
	schedule_delayed_work(&autocut_work, msecs_to_jiffies(1000));
	pr_info("autocut: loaded (max_soc=%d, min_soc=%d)\n", max_soc, min_soc);
	return 0;
}

static void __exit autocut_exit(void)
{
	cancel_delayed_work_sync(&autocut_work);
	pr_info("autocut: unloaded\n");
}

module_init(autocut_init);
module_exit(autocut_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Auto-cut charging with configurable SOC thresholds");
MODULE_AUTHOR("X00TD Porter");
