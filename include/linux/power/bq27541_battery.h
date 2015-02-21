#ifndef __LINUX_BQ27541_BATTERY_H__
#define __LINUX_BQ27541_BATTERY_H__

/**
 * struct bq27541_plaform_data - Platform data for bq27541 devices
 * @name: Name of the battery. If NULL the driver will fallback to "bq27541".
 * @read: HDQ read callback.
 *	This function should provide access to the HDQ bus the battery is
 *	connected to.
 *	The first parameter is a pointer to the battery device, the second the
 *	register to be read. The return value should either be the content of
 *	the passed register or an error value.
 */
struct bq27541_platform_data {
	const char *name;
	int	ac_present_gpio;
	int bat_low_wakeup_to_shutdown;
	int (*read)(struct device *dev, unsigned int);
	int (*is_ac_online)(void);
	int (*is_usb_online)(void);
	bool use_otg_notifier;
};

void external_power_status(int status, int chrg_type);
#endif
