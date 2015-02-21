/*
 * max77665-charger.c - Battery charger driver
 *
 *  Copyright (c) 2012-2013, NVIDIA CORPORATION.  All rights reserved.
 *  Syed Rafiuddin <srafiuddin@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <linux/err.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/alarmtimer.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/wakelock.h>
#include <linux/mfd/max77665.h>
#include <linux/max77665-charger.h>
#include <linux/interrupt.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include <linux/delay.h>

#define CHARGER_TYPE_DETECTION_DEBOUNCE_TIME_MS 500

/* fast charge current in mA */
static const uint32_t chg_cc[]  = {
	0, 33, 66, 99, 133, 166, 199, 233, 266, 299,
	333, 366, 399, 432, 466, 499, 532, 566, 599, 632,
	666, 699, 732, 765, 799, 832, 865, 899, 932, 965,
	999, 1032, 1065, 1098, 1132, 1165, 1198, 1232, 1265,
	1298, 1332, 1365, 1398, 1421, 1465, 1498, 1531, 1565,
	1598, 1631, 1665, 1698, 1731, 1764, 1798, 1831, 1864,
	1898, 1931, 1964, 1998, 2031, 2064, 2097
};

/* primary charge termination voltage in mV */
static const uint32_t chg_cv_prm[] = {
	3650, 3675, 3700, 3725, 3750,
	3775, 3800, 3825, 3850, 3875,
	3900, 3925, 3950, 3975, 4000,
	4025, 4050, 4075, 4100, 4125,
	4150, 4175, 4200, 4225, 4250,
	4275, 4300, 4325, 4340, 4350,
	4375, 4400
};

static int max77665_bat_to_sys_oc_thres[] = {
	0, 3000, 3250, 3500, 3750, 4000, 4250, 4500
};


struct max77665_charger {
	enum max77665_mode mode;
	struct device		*dev;
	int			irq;
	struct power_supply	ac;
	struct power_supply	usb;
	struct max77665_charger_plat_data *plat_data;
	struct mutex current_limit_mutex;
	int max_current_mA;
	uint8_t ac_online;
	uint8_t usb_online;
	uint8_t num_cables;
	struct extcon_dev *edev;
	struct alarm wdt_alarm;
	struct delayed_work wdt_ack_work;
	struct delayed_work set_max_current_work;
	struct wake_lock wdt_wake_lock;
	unsigned int oc_count;
};

static enum power_supply_property max77665_charger_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
};

static int max77665_write_reg(struct max77665_charger *charger,
	uint8_t reg, int value)
{
	int ret = 0;
	struct device *dev = charger->dev;

	if ((value < 0) || (value > 0xFF))
		return -EINVAL;

	ret = max77665_write(dev->parent, MAX77665_I2C_SLAVE_PMIC, reg, value);
	if (ret < 0)
		dev_err(dev, "Failed to write to reg 0x%x\n", reg);
	return ret;
}

static int max77665_read_reg(struct max77665_charger *charger,
	uint8_t reg, uint32_t *value)
{
	int ret = 0;
	uint8_t read = 0;
	struct device *dev = charger->dev;

	ret = max77665_read(dev->parent, MAX77665_I2C_SLAVE_PMIC, reg, &read);
	if (0 > ret)
		dev_err(dev, "Failed to read register 0x%x\n", reg);
	else
		*value = read;

	return ret;
}

static int max77665_update_reg(struct max77665_charger *charger,
	uint8_t reg, int value)
{
	int ret = 0;
	int read_val;

	ret = max77665_read_reg(charger, reg, &read_val);
	if (ret)
		return ret;

	ret = max77665_write_reg(charger, reg, read_val | value);
	return ret;
}

/* Convert current to register value using lookup table */
static int convert_to_reg(struct device *dev, char *tbl_name,
		const unsigned int *tbl, size_t size, unsigned int val)
{
	size_t i;

	if ((val < tbl[0]) || (val > tbl[size - 1])) {
		dev_err(dev, "%d is not in %s table\n", val,  tbl_name);
		return -EINVAL;
	}

	for (i = 0; i < size - 1; i++)
		if ((tbl[i] <= val) && (val < tbl[i + 1]))
			break;
	return i;
}
#define CONVERT_TO_REG(table, val)	\
	convert_to_reg(charger->dev, #table, table, ARRAY_SIZE(table), val)

int max77665_set_max_input_current(struct max77665_charger *charger, int mA)
{
	int ret;

	ret = max77665_write_reg(charger, MAX77665_CHG_CNFG_09,
			mA / CURRENT_STEP_mA);
	if (ret < 0)
		dev_err(charger->dev, "failed to set %dmA charging\n", mA);
	return 0;
}

int max77665_get_max_input_current(struct max77665_charger *charger, int *mA)
{
	int ret;
	uint32_t val;

	ret = max77665_read_reg(charger, MAX77665_CHG_CNFG_09, &val);
	if (0 > ret)
		dev_err(charger->dev, "failed to get charging current\n");
	val &= 0x7F;
	*mA = max_t(int, MIN_CURRENT_LIMIT_mA, val * CURRENT_STEP_mA);
	return ret;
}

static int max77665_charger_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct max77665_charger *chip = container_of(psy,
				struct max77665_charger, ac);

	if (psp == POWER_SUPPLY_PROP_CURRENT_MAX)
		/* passed value is uA */
		return max77665_set_max_input_current(chip, val->intval / 1000);

	return -EINVAL;
}

static int max77665_charger_get_property(struct power_supply *psy,
		enum power_supply_property psp, union power_supply_propval *val)
{
	int online;
	int ret;
	struct max77665_charger *charger;

	if (psy->type == POWER_SUPPLY_TYPE_MAINS) {
		charger = container_of(psy, struct max77665_charger, ac);
		online = charger->ac_online;
	} else if (psy->type == POWER_SUPPLY_TYPE_USB) {
		charger = container_of(psy, struct max77665_charger, usb);
		online = charger->usb_online;
	} else {
		return -EINVAL;
	}

	ret = 0;
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = online;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		ret = max77665_get_max_input_current(charger, &val->intval);
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static int max77665_charger_property_is_writeable(struct power_supply *psy,
						enum power_supply_property psp)
{
	if (psp == POWER_SUPPLY_PROP_CURRENT_MAX)
		return 1;
	return 0;
}

static int max77665_enable_write(struct max77665_charger *charger, bool access)
{
	int ret = 0;

	if (access)
		/* enable write acces to registers */
		ret = max77665_write_reg(charger, MAX77665_CHG_CNFG_06, 0x0c);
	else
		/* Disable write acces to registers */
		ret = max77665_write_reg(charger, MAX77665_CHG_CNFG_06, 0x00);

	if (ret < 0)
		dev_err(charger->dev, "failed to %s write acess\n",
				access ? "eanble" : "disable");
	return ret;
}

static bool max77665_check_charging_ok(struct max77665_charger *charger)
{
	uint32_t chgin_dtls;
	uint32_t byp_dtls;
	int ret;

	/* check charging input is OK */
	ret = max77665_read_reg(charger, MAX77665_CHG_DTLS_00, &chgin_dtls);
	if ((ret < 0) || (CHGIN_DTLS_MASK(chgin_dtls) != CHGIN_DTLS_VALID))
		return false;

	/* check voltage regulation loop */
	ret = max77665_read_reg(charger, MAX77665_CHG_DTLS_02, &byp_dtls);
	if ((ret < 0) || (BYP_DTLS_MASK(byp_dtls) != BYP_DTLS_VALID))
		return false;

	return true;
}

static int max77665_set_ideal_input_current(struct max77665_charger *charger)
{
	int ret;
	int min;
	int max;
	int mid;

	min = 100;
	max = charger->max_current_mA;
	/* binary search the ideal input charger current limit */
	do {
		mid = (min + max) / 2;

		ret = max77665_set_max_input_current(charger, mid);
		if (ret < 0)
			return ret;

		/* let new charging current settle for 50mS */
		msleep(50);
		if (max77665_check_charging_ok(charger))
			min = mid;
		else
			max = mid;
	} while (CURRENT_STEP_mA <= (max - min));

	charger->max_current_mA = mid;
	dev_info(charger->dev, "max current after calibration is %dmA\n", mid);
	return 0;
}

static void max77665_set_ideal_input_current_work(struct work_struct *w)
{
	struct max77665_charger *charger = container_of(to_delayed_work(w),
			struct max77665_charger, set_max_current_work);
	int irq_mask;
	int safeout_ctrl;

	mutex_lock(&charger->current_limit_mutex);
	if (!max77665_check_charging_ok(charger)) {
		/*
		 * during the max current searching, we need mask charger
		 * input current related IRQs
		 */
		max77665_read_reg(charger, MAX77665_CHG_INT_MASK, &irq_mask);
		max77665_write_reg(charger, MAX77665_CHG_INT_MASK,
					irq_mask | BYP_BIT | CHGIN_BIT);
		/*
		 * also we turn off the SAFEOUT1/2 output, so we don't need
		 * generate extra IRQ for the OTG input.
		 */
		max77665_read_reg(charger, MAX77665_SAFEOUTCTRL, &safeout_ctrl);
		max77665_write_reg(charger, MAX77665_SAFEOUTCTRL,
				safeout_ctrl & ~(ENSAFEOUT1 | ENSAFEOUT2));

		max77665_set_ideal_input_current(charger);

		/* restore IRQs and safeout */
		max77665_write_reg(charger, MAX77665_SAFEOUTCTRL, safeout_ctrl);
		max77665_write_reg(charger, MAX77665_CHG_INT_MASK, irq_mask);
	}
	mutex_unlock(&charger->current_limit_mutex);
}

static void max77665_display_charger_status(struct max77665_charger *charger,
		uint32_t status)
{
	int i;
	uint32_t val;
	bool ok;
	int bits[] = { BYP_BIT, DETBAT_BIT, BAT_BIT, CHG_BIT, CHGIN_BIT };
	char *info[] = {
		"bypass", "main battery presence", "battery",
		"charger", "charging input"
	};

	ok = true;
	for (i = 0; i < ARRAY_SIZE(bits); i++) {
		if (0 == (status & bits[i])) {
			ok = false;
			dev_dbg(charger->dev, "%s is not OK\n", info[i]);
		}
	}

	if (ok == false) {
		max77665_read_reg(charger, MAX77665_CHG_DTLS_00, &val);
		dev_dbg(charger->dev, "chg_details_00 is %x\n", val);

		max77665_read_reg(charger, MAX77665_CHG_DTLS_01, &val);
		dev_dbg(charger->dev, "chg_details_01 is %x\n", val);

		max77665_read_reg(charger, MAX77665_CHG_DTLS_02, &val);
		dev_dbg(charger->dev, "chg_details_02 is %x\n", val);
	}
}

static int max77665_handle_charger_status(struct max77665_charger *charger,
		uint32_t status)
{
	uint32_t val;

	max77665_display_charger_status(charger, status);

	/*
	 * if it is charging input or charing error after charging
	 * started, we will find the ideal charging current again
	 */
	if (!(status & CHG_BIT) || !(status & CHGIN_BIT))
		schedule_delayed_work(&charger->set_max_current_work,
				msecs_to_jiffies(100));

	if (!(status & BAT_BIT)) {
		max77665_read_reg(charger, MAX77665_CHG_DTLS_01, &val);
		if (BAT_DTLS_MASK(val) == BAT_DTLS_OVERCURRENT)
			charger->oc_count++;
	}

	return 0;
}

static int max77665_set_charger_mode(struct max77665_charger *charger,
		enum max77665_mode mode)
{
	int ret;
	int flags;

	charger->mode = mode;
	ret = max77665_enable_write(charger, true);
	if (ret < 0)
		return ret;

	if (mode == OFF)
		flags = CHARGER_OFF_OTG_OFF_BUCK_ON_BOOST_OFF;
	if (mode == CHARGER)
		/* enable charging and charging watchdog */
		flags = CHARGER_ON_OTG_OFF_BUCK_ON_BOOST_OFF | WDTEN;
	else if (mode == OTG)
		flags = CHARGER_OFF_OTG_ON_BUCK_OFF_BOOST_ON;

	ret = max77665_write_reg(charger, MAX77665_CHG_CNFG_00, flags);
	if (ret < 0)
		goto error;

	/*
	 * Under regulation loop voltage, the VBUS should be higher then the
	 * Charging Port Undershoot Voltage(4.2v) according the USB charging
	 * spec 1.2
	 */
	max77665_read_reg(charger, MAX77665_CHG_CNFG_12, &flags);
	flags |= VCHGIN_REGULATION_4V3;
	ret = max77665_write_reg(charger, MAX77665_CHG_CNFG_12, flags);
	if (ret < 0)
		goto error;

	/*
	 * Set to max current in theory. If the the charger has less current
	 * capability, we will calibrated the current inside the charging error
	 * IRQs handler.
	 */
	ret = max77665_set_max_input_current(charger, charger->max_current_mA);
	dev_info(charger->dev, "max input current %sset to %dmA\n",
			(ret == 0) ? "" : "failed ", charger->max_current_mA);
error:
	return max77665_enable_write(charger, false);
}

static int max77665_charger_init(struct max77665_charger *charger)
{
	int ret = 0;
	int val;

	ret = max77665_enable_write(charger, true);
	if (ret < 0)
		goto error;

	val = FAST_CHARGE_DURATION_4HR | CHARGER_RESTART_THRESHOLD_150mV |
						LOW_BATTERY_PREQ_ENABLE;
	ret = max77665_update_reg(charger, MAX77665_CHG_CNFG_01, val);
	if (ret < 0) {
		dev_err(charger->dev, "Failed in writing register 0x%x\n",
			MAX77665_CHG_CNFG_01);
		goto error;
	}

	if (charger->plat_data->fast_chg_cc) {
		val = CONVERT_TO_REG(chg_cc, charger->plat_data->fast_chg_cc);
		ret = max77665_update_reg(charger, MAX77665_CHG_CNFG_02, val);
		if (ret < 0) {
			dev_err(charger->dev, "Failed writing register 0x%x\n",
				MAX77665_CHG_CNFG_02);
			goto error;
		}
	}

	if (charger->plat_data->term_volt) {
		val = CONVERT_TO_REG(chg_cv_prm, charger->plat_data->term_volt);
		ret = max77665_update_reg(charger, MAX77665_CHG_CNFG_04, val);
		if (ret < 0) {
			dev_err(charger->dev, "Failed writing to reg:0x%x\n",
				MAX77665_CHG_CNFG_04);
			goto error;
		}
	}

error:
	ret = max77665_enable_write(charger, false);
	return ret;
}

static void max77665_charger_disable_wdt(struct max77665_charger *charger)
{
	cancel_delayed_work_sync(&charger->wdt_ack_work);
	alarm_cancel(&charger->wdt_alarm);
}

static int max77665_disable_charger(struct max77665_charger *charger,
		struct extcon_dev *edev)
{
	int ret;

	charger->max_current_mA = 0;
	ret = max77665_set_charger_mode(charger, OFF);
	if (0 > ret)
		dev_err(charger->dev, "failed to disable charging");
	max77665_charger_disable_wdt(charger);

	if (charger->plat_data->update_status)
		charger->plat_data->update_status(0);

	charger->ac_online = 0;
	charger->usb_online = 0;
	power_supply_changed(&charger->usb);
	power_supply_changed(&charger->ac);

	return ret;
}

static int max77665_enable_charger(struct max77665_charger *charger,
		struct extcon_dev *edev)
{
	int ret = 0;
	int ilim;
	enum max77665_mode mode;

	charger->usb_online = 0;
	charger->ac_online = 0;

	if (charger->plat_data->update_status)
		charger->plat_data->update_status(0);

	mode = CHARGER;
	if (true == extcon_get_cable_state(edev, "USB-Host")) {
		mode = OTG;
		charger->max_current_mA = 0;
	} else if (true == extcon_get_cable_state(edev, "USB")) {
		charger->usb_online = 1;
		charger->max_current_mA = 500;
	} else if (true == extcon_get_cable_state(edev, "Charge-downstream")) {
		charger->usb_online = 1;
		charger->max_current_mA = 1500;
	} else if (true == extcon_get_cable_state(edev, "TA")) {
		charger->ac_online = 1;
		charger->max_current_mA = 2000;
	} else if (true == extcon_get_cable_state(edev, "Fast-charger")) {
		charger->ac_online = 1;
		charger->max_current_mA = 2200;
	} else if (true == extcon_get_cable_state(edev, "Slow-charger")) {
		charger->ac_online = 1;
		charger->max_current_mA = 500;
	} else {
		/* no cable connected */
		goto done;
	}

	ret = max77665_set_charger_mode(charger, mode);
	if (ret < 0) {
		dev_err(charger->dev, "failed to set device to charger mode\n");
		goto done;
	}

	/* set the charging watchdog timer */
	alarm_start(&charger->wdt_alarm, ktime_add(ktime_get_boottime(),
			ktime_set(MAX77665_WATCHDOG_TIMER_PERIOD_S / 2, 0)));

	if (charger->plat_data->update_status) {
		ret = max77665_get_max_input_current(charger, &ilim);
		if (0 > ret)
			goto done;
		charger->plat_data->update_status(ilim);
	}

done:
	if (charger->usb_online)
		power_supply_changed(&charger->usb);
	if (charger->ac_online)
		power_supply_changed(&charger->ac);

	return ret;
}

static void charger_extcon_handle_notifier(struct work_struct *w)
{
	struct max77665_charger_cable *cable = container_of(to_delayed_work(w),
			struct max77665_charger_cable, extcon_notifier_work);
	struct max77665_charger *charger = cable->charger;
	int val;

	mutex_lock(&charger->current_limit_mutex);
	if (0 > max77665_read_reg(cable->charger, MAX77665_CHG_DTLS_01, &val))
		goto error;

	dev_dbg(charger->dev, "cable is %s, charging is %s\n",
			cable->event ? "attached" : "disconnected",
			charging_is_on(val) ? "on" : "off");
	/*
	 * For high current charging, max77665 might cut off the VBUS_SAFE_OUT
	 * to AP if input voltage is below VCHIN_UVLO (in voltage regulation
	 * mode). If that happens, the charging might is still on when AP
	 * send cable unplugged event. We need check this conditon by reading
	 * the CHG_DTLS_01 register.
	 */
	if (cable->event == 0 && !charging_is_on(val))
		max77665_disable_charger(charger, cable->extcon_dev->edev);
	else if (cable->event == 1 && !charging_is_on(val))
		max77665_enable_charger(charger, cable->extcon_dev->edev);

error:
	mutex_unlock(&charger->current_limit_mutex);

}

static int max77665_reset_charger(struct max77665_charger *charger,
		struct extcon_dev *edev)
{
	int ret;

	mutex_lock(&charger->current_limit_mutex);

	ret = max77665_disable_charger(charger, charger->edev);
	if (ret < 0)
		goto error;

	ret = max77665_enable_charger(charger, charger->edev);
	if (ret < 0)
		goto error;
error:
	mutex_unlock(&charger->current_limit_mutex);
	return 0;
}

static void max77665_charger_wdt_ack_work_handler(struct work_struct *w)
{
	struct max77665_charger *charger = container_of(to_delayed_work(w),
			struct max77665_charger, wdt_ack_work);

	if (0 > max77665_update_reg(charger, MAX77665_CHG_CNFG_06, WDTCLR))
		dev_err(charger->dev, "fail to ack charging WDT\n");

	alarm_start(&charger->wdt_alarm,
			ktime_add(ktime_get_boottime(), ktime_set(30, 0)));
	wake_unlock(&charger->wdt_wake_lock);
}

static enum alarmtimer_restart max77665_charger_wdt_timer(struct alarm *alarm,
		ktime_t now)
{
	struct max77665_charger *charger =
		container_of(alarm, struct max77665_charger, wdt_alarm);

	wake_lock(&charger->wdt_wake_lock);
	schedule_delayed_work(&charger->wdt_ack_work, 0);
	return ALARMTIMER_NORESTART;
}

static int charger_extcon_notifier(struct notifier_block *self,
		unsigned long event, void *ptr)
{
	struct max77665_charger_cable *cable = container_of(self,
		struct max77665_charger_cable, nb);

	cable->event = event;
	cancel_delayed_work(&cable->extcon_notifier_work);
	schedule_delayed_work(&cable->extcon_notifier_work,
		msecs_to_jiffies(CHARGER_TYPE_DETECTION_DEBOUNCE_TIME_MS));

	return NOTIFY_DONE;
}

static int max77665_update_charger_status(struct max77665_charger *charger)
{
	int ret;
	uint32_t read_val;

	mutex_lock(&charger->current_limit_mutex);

	ret = max77665_read_reg(charger, MAX77665_CHG_INT, &read_val);
	if (ret < 0)
		goto error;
	dev_dbg(charger->dev, "CHG_INT = 0x%02x\n", read_val);

	ret = max77665_read_reg(charger, MAX77665_CHG_INT_OK, &read_val);
	if (ret < 0)
		goto error;

	if (charger->plat_data->is_battery_present)
		max77665_handle_charger_status(charger, read_val);

error:
	mutex_unlock(&charger->current_limit_mutex);
	return ret;
}

static irqreturn_t max77665_charger_irq_handler(int irq, void *data)
{
	struct max77665_charger *charger = data;

	max77665_update_charger_status(charger);
	return IRQ_HANDLED;
}

static ssize_t max77665_set_bat_oc_threshold(struct device *dev,
	struct device_attribute *attr, const char *buf, size_t count)
{
	struct max77665_charger *charger = dev_get_drvdata(dev);
	int i;
	int ret;
	int val = 0;
	int n = ARRAY_SIZE(max77665_bat_to_sys_oc_thres);
	char *p = (char *)buf;
	int oc_curr = memparse(p, &p);

	for (i = 0; i < n; ++i) {
		if (oc_curr <= max77665_bat_to_sys_oc_thres[i])
			break;
	}

	val = (i < n) ? i : n - 1;
	ret = max77665_update_bits(charger->dev->parent,
			MAX77665_I2C_SLAVE_PMIC, MAX77665_CHG_CNFG_12,
			BAT_TO_SYS_OVERCURRENT_MASK, val);
	if (ret < 0) {
		dev_err(charger->dev, "CHG_CNFG_12 update failed: %d\n", ret);
		return ret;
	}
	return count;
}

static ssize_t max77665_show_bat_oc_threshold(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct max77665_charger *charger = dev_get_drvdata(dev);
	uint8_t val = 0;
	int ret;

	ret = max77665_read(charger->dev->parent, MAX77665_I2C_SLAVE_PMIC,
				MAX77665_CHG_CNFG_12, &val);
	if (ret < 0) {
		dev_err(charger->dev, "CHG_CNFG_12 read failed: %d\n", ret);
		return ret;
	}
	val &= BAT_TO_SYS_OVERCURRENT_MASK;
	return sprintf(buf, "%d\n", max77665_bat_to_sys_oc_thres[val]);
}
static DEVICE_ATTR(oc_threshold,  0644,
		max77665_show_bat_oc_threshold, max77665_set_bat_oc_threshold);

static ssize_t max77665_set_battery_oc_state(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct max77665_charger *charger = dev_get_drvdata(dev);
	int ret;
	bool enabled;
	unsigned int val;

	if ((*buf == 'E') || (*buf == 'e')) {
		enabled = true;
	} else if ((*buf == 'D') || (*buf == 'd')) {
		enabled = false;
	} else {
		dev_err(charger->dev, "Illegal option\n");
		return -EINVAL;
	}

	val = (enabled) ? 0x0 : 0x8;
	ret = max77665_update_bits(charger->dev->parent,
			MAX77665_I2C_SLAVE_PMIC,
			MAX77665_CHG_INT_MASK, 0x08, val);
	if (ret < 0) {
		dev_err(charger->dev, "CHG_INT_MASK update failed: %d\n", ret);
		return ret;
	}
	return count;
}

static ssize_t max77665_show_battery_oc_state(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct max77665_charger *charger = dev_get_drvdata(dev);
	uint8_t val = 0;
	int ret;

	ret = max77665_read(charger->dev->parent, MAX77665_I2C_SLAVE_PMIC,
			 MAX77665_CHG_INT_MASK, &val);
	if (ret < 0) {
		dev_err(charger->dev, "CHG_INT_MASK read failed: %d\n", ret);
		return ret;
	}
	if (val & 0x8)
		return sprintf(buf, "disabled\n");
	else
		return sprintf(buf, "enabled\n");
}
static DEVICE_ATTR(oc_state, 0644,
		max77665_show_battery_oc_state, max77665_set_battery_oc_state);

static ssize_t max77665_show_battery_oc_count(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct max77665_charger *charger = dev_get_drvdata(dev);
	return sprintf(buf, "%u\n", charger->oc_count);
}

static DEVICE_ATTR(oc_count, 0444, max77665_show_battery_oc_count, NULL);

static struct attribute *max77665_chg_attributes[] = {
	&dev_attr_oc_threshold.attr,
	&dev_attr_oc_state.attr,
	&dev_attr_oc_count.attr,
	NULL,
};

static const struct attribute_group max77665_chg_attr_group = {
	.attrs = max77665_chg_attributes,
};

static int max77665_add_sysfs_entry(struct device *dev)
{
	return sysfs_create_group(&dev->kobj, &max77665_chg_attr_group);
}
static void max77665_remove_sysfs_entry(struct device *dev)
{
	sysfs_remove_group(&dev->kobj, &max77665_chg_attr_group);
}

static __devinit int max77665_battery_probe(struct platform_device *pdev)
{
	int ret = 0;
	uint8_t j;
	struct max77665_charger *charger;

	charger = devm_kzalloc(&pdev->dev, sizeof(*charger), GFP_KERNEL);
	if (!charger) {
		dev_err(&pdev->dev, "failed to allocate memory status\n");
		return -ENOMEM;
	}

	mutex_init(&charger->current_limit_mutex);

	charger->dev = &pdev->dev;

	charger->plat_data = pdev->dev.platform_data;
	dev_set_drvdata(&pdev->dev, charger);

	if (charger->plat_data->is_battery_present) {

		wake_lock_init(&charger->wdt_wake_lock, WAKE_LOCK_SUSPEND,
				"max77665-charger-wdt");
		alarm_init(&charger->wdt_alarm, ALARM_BOOTTIME,
				max77665_charger_wdt_timer);
		INIT_DELAYED_WORK(&charger->wdt_ack_work,
				max77665_charger_wdt_ack_work_handler);
		INIT_DELAYED_WORK(&charger->set_max_current_work,
				max77665_set_ideal_input_current_work);

		/* modify OTP setting of input current limit to 100ma */
		ret = max77665_set_max_input_current(charger, 100);
		if (ret < 0)
			goto remove_charging;

		dev_info(&pdev->dev, "Initializing battery charger code\n");

		charger->ac.name		= "ac";
		charger->ac.type		= POWER_SUPPLY_TYPE_MAINS;
		charger->ac.get_property	= max77665_charger_get_property;
		charger->ac.set_property	= max77665_charger_set_property;
		charger->ac.properties		= max77665_charger_props;
		charger->ac.num_properties = ARRAY_SIZE(max77665_charger_props);
		charger->ac.property_is_writeable =
			max77665_charger_property_is_writeable;
		ret = power_supply_register(charger->dev, &charger->ac);
		if (ret) {
			dev_err(charger->dev, "failed: power supply register\n");
			return ret;
		}

		charger->usb = charger->ac;
		charger->usb.name		= "usb";
		charger->usb.type		= POWER_SUPPLY_TYPE_USB;
		ret = power_supply_register(charger->dev, &charger->usb);
		if (ret) {
			dev_err(charger->dev, "failed: power supply register\n");
			goto pwr_sply_error;
		}

		for (j = 0 ; j < charger->plat_data->num_cables; j++) {
			struct max77665_charger_cable *cable =
				&charger->plat_data->cables[j];
			cable->extcon_dev =  devm_kzalloc(&pdev->dev,
					sizeof(struct extcon_specific_cable_nb),
								GFP_KERNEL);
			if (!cable->extcon_dev) {
				dev_err(&pdev->dev, "failed to allocate memory for extcon dev\n");
				goto chrg_error;
			}

			INIT_DELAYED_WORK(&cable->extcon_notifier_work,
					charger_extcon_handle_notifier);

			cable->charger = charger;
			cable->nb.notifier_call = charger_extcon_notifier;

			ret = extcon_register_interest(cable->extcon_dev,
					charger->plat_data->extcon_name,
					cable->name, &cable->nb);
			if (ret < 0)
				dev_err(charger->dev, "Cannot register for cable: %s\n",
						cable->name);
		}

		charger->edev = extcon_get_extcon_dev(charger->plat_data->extcon_name);
		if (!charger->edev)
			goto chrg_error;

	}

	charger->irq = platform_get_irq(pdev, 0);
	ret = request_threaded_irq(charger->irq, NULL,
			max77665_charger_irq_handler, 0, "charger_irq",
			charger);
	if (ret) {
		dev_err(&pdev->dev, "failed: irq request error :%d)\n", ret);
		goto chrg_error;
	}
	/* unmask all the interrupt */
	max77665_write_reg(charger, MAX77665_CHG_INT_MASK, 0x0);

	ret = max77665_add_sysfs_entry(&pdev->dev);
	if (ret < 0) {
		dev_err(charger->dev, "sysfs create failed %d\n", ret);
		goto free_irq;
	}

	if (charger->plat_data->is_battery_present) {
		ret = max77665_charger_init(charger);
		if (ret < 0) {
			dev_err(charger->dev, "failed to initialize charger\n");
			goto remove_sysfs;
		}
	}

	/* Set OC threshold to 3250mA */
	ret = max77665_update_bits(charger->dev->parent,
		MAX77665_I2C_SLAVE_PMIC, MAX77665_CHG_CNFG_12,
		BAT_TO_SYS_OVERCURRENT_MASK, BAT_TO_SYS_OVERCURRENT_3A25);
	if (ret < 0) {
		dev_err(charger->dev, "CHG_CNFG_12 update failed: %d\n", ret);
		goto remove_sysfs;
	}

	if (charger->plat_data->is_battery_present) {
		/* reset the charging in case cable is already inserted */
		ret = max77665_reset_charger(charger, charger->edev);
		if (ret < 0)
			goto chrg_error;
	}

	dev_info(&pdev->dev, "%s() get success\n", __func__);

	return 0;

remove_sysfs:
	max77665_remove_sysfs_entry(&pdev->dev);
free_irq:
	free_irq(charger->irq, charger);
chrg_error:
if (charger->plat_data->is_battery_present)
	power_supply_unregister(&charger->usb);
pwr_sply_error:
if (charger->plat_data->is_battery_present)
	power_supply_unregister(&charger->ac);
remove_charging:
	mutex_destroy(&charger->current_limit_mutex);
if (charger->plat_data->is_battery_present)
	wake_lock_destroy(&charger->wdt_wake_lock);
	return ret;
}

static int __devexit max77665_battery_remove(struct platform_device *pdev)
{
	struct max77665_charger *charger = platform_get_drvdata(pdev);

	max77665_remove_sysfs_entry(&pdev->dev);
	free_irq(charger->irq, charger);
if (charger->plat_data->is_battery_present)
	power_supply_unregister(&charger->ac);
if (charger->plat_data->is_battery_present)
	power_supply_unregister(&charger->usb);

	return 0;
}
#ifdef CONFIG_PM_SLEEP
static int max77665_suspend(struct device *dev)
{
	return 0;
}
static int max77665_resume(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct max77665_charger *charger = platform_get_drvdata(pdev);
	int ret;

	ret = max77665_update_charger_status(charger);
	if (ret < 0)
		dev_err(charger->dev, "error occured in resume\n");
	return ret;
}

static const struct dev_pm_ops max77665_pm = {
	.suspend = max77665_suspend,
	.resume = max77665_resume,
};
#define MAX77665_PM	(&max77665_pm)
#else
#define MAX77665_PM	NULL
#endif
static struct platform_driver max77665_battery_driver = {
	.driver = {
		.name = "max77665-charger",
		.owner = THIS_MODULE,
		.pm	= MAX77665_PM,
	},
	.probe = max77665_battery_probe,
	.remove = __devexit_p(max77665_battery_remove),

};

static int __init max77665_battery_init(void)
{
	return platform_driver_register(&max77665_battery_driver);
}

static void __exit max77665_battery_exit(void)
{
	platform_driver_unregister(&max77665_battery_driver);
}

late_initcall(max77665_battery_init);
module_exit(max77665_battery_exit);

MODULE_DESCRIPTION("MAXIM MAX77665 battery charging driver");
MODULE_AUTHOR("Syed Rafiuddin <srafiuddin@nvidia.com>");
MODULE_LICENSE("GPL v2");
