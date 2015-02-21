/* drivers/input/touchscreen/fts.c
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 */

#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/gpio.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <asm/uaccess.h>
#include <linux/cdev.h>
#include <linux/version.h>
#include <linux/input/fts_ts.h>
#ifdef WISTRON_EXTENSION
#include <linux/proc_fs.h>
#endif

#ifdef FTS_EXTENSION
struct firmware_upgrade_info {
	u8 id1;				/*upgrade id 1 */
	u8 id2;				/*upgrade id 2 */
	u16 delay_aa;			/*delay of write FT_UPGRADE_AA */
	u16 delay_55;			/*delay of write FT_UPGRADE_55 */
	u16 delay_readid;		/*delay of read id */
	u16 delay_earse; 		/*delay of earse flash*/
};
#endif

struct fts_info{
	struct i2c_client *client;
	struct input_dev *input_dev;
	struct mutex lock;
	struct fts_platform_data *pdata;
#ifdef FTS_EXTENSION
	struct firmware_upgrade_info fw_upgrade_info;
#endif
	u8 chip_id;
#ifdef WISTRON_EXTENSION
	u8 ctpm_id;
#endif
};

#ifdef WISTRON_EXTENSION
static DEFINE_MUTEX(fts_power_lock);
static bool isPowered = false;
static int probe_state = 0;
static bool isOpen = false;
#endif

struct fts_touch_info{
	u8 x_msb:4, rev:2, event:2;
	u8 x_lsb;
	u8 y_msb:4, id:4;
	u8 y_lsb;
	u8 weight;
	u8 speed:2, direction:2, area:4;
};

struct fts_packet_info{
	u8 gesture;
	u8 fingers:4, frame:4;
	struct fts_touch_info touch[FTS_MAX_TOUCH];
};

static int fts_readsb(struct fts_info *ts, u8 *addr, u8 *buf, u16 len)
{

	int ret;
	struct i2c_msg msg[2];

	if(addr){
		FILL_I2C_MSG(msg[0], ts->client->addr, 0, 1, addr);
		FILL_I2C_MSG(msg[1], ts->client->addr, I2C_M_RD, len, buf);
		ret = i2c_transfer(ts->client->adapter, msg, 2);
	}
	else{
		FILL_I2C_MSG(msg[0], ts->client->addr, I2C_M_RD, len, buf);
		ret = i2c_transfer(ts->client->adapter, &msg[0], 1);
	}

	if (ret < 0){
		fts_msg("i2c_transfer failed !\n");
		return ret;
	}

	return 0;
}

static int fts_writesb(struct fts_info *ts, u8 *buf, u16 len)
{

	int ret;
	struct i2c_msg msg;

	FILL_I2C_MSG(msg, ts->client->addr, 0, len, buf);

	ret = i2c_transfer(ts->client->adapter, &msg, 1);
	if (ret < 0){
		fts_msg("i2c_transfer failed !\n");
		return ret;
	}

	return 0;
}

static int fts_readb(struct fts_info *ts, u8 addr, u8 *value)
{

	u8 reg = addr;
	int ret;
	struct i2c_msg msg[2];

	FILL_I2C_MSG(msg[0], ts->client->addr, 0, 1, &reg);
	FILL_I2C_MSG(msg[1], ts->client->addr, I2C_M_RD, 1, value);

	ret = i2c_transfer(ts->client->adapter, msg, 2);
	if (ret < 0){
		fts_msg("i2c_transfer failed !\n");
		return ret;
	}

	fts_dbg("Read %02X = 0x%02X\n", addr, *value);

	return 0;
}

static int fts_writeb(struct fts_info *ts, u8 addr, u8 value)
{

	struct i2c_msg msg;
	int ret;
	u8 buf[2];

	buf[0] = addr;
	buf[1] = value;

	FILL_I2C_MSG(msg, ts->client->addr, 0, 2, buf);

	ret = i2c_transfer(ts->client->adapter, &msg, 1);
	if (ret < 0){
		fts_msg("i2c_transfer failed !\n");
		return ret;
	}

	return 0;
}

#ifdef HW_PWR_ONOFF
static int fts_hw_power_on(struct fts_info *ts, int onoff)
{
	if(ts->pdata->hw_power)
		ts->pdata->hw_power(&ts->client->dev,onoff);

	return 0;
}
#endif

static int fts_gpio_reset(struct fts_info *ts, int val)
{
	if(ts->pdata->hw_reset)
		ts->pdata->hw_reset(val);
/*

	setting reset pin to high

	mdelay(1);

	setting reset pin to low
	mdelay(2);

	setting reset pin to high

	mdelay(150);

*/

	return 0;
}

static int fts_config_gpio(void)
{



	return 0;
}

static int fts_fw_version(struct fts_info *ts, u8 *version)
{

	int ret;

	ret = fts_readb(ts, REG_FW_VER, version);
	
	if(ret){
		fts_msg("Can't read firmware version !\n");
		return -ENODEV;
	}

	return 0;
}

#ifdef WISTRON_EXTENSION
static int fts_probe_power_on(struct fts_info *ts)
{
       mutex_lock(&fts_power_lock);

       if(!isPowered) {
               fts_hw_power_on(ts,PWR_ON);
               isPowered = true;
       }

       mutex_unlock(&fts_power_lock);
       return 0;
}

static char fw_ver_buf[10];
static char* fts_ctpm_fw_version(struct fts_info *ts, u8 *version)
{
	u8 id,tmp;

	memset(fw_ver_buf,0,sizeof(fw_ver_buf));
	if(fts_fw_version(ts, version)) {
		fts_msg("FTS read version is fail !\n");
		strcpy(fw_ver_buf,"fail");
	}
	else {
		id = *version & CTPM_ID_MASK;
		tmp = *version & ~(CTPM_ID_MASK);
		if (id == CTPM_OGS_ID) {
			sprintf(fw_ver_buf, "1\.%02x", tmp);
		}
		else if(id == CTPM_GG_ID) {
			sprintf(fw_ver_buf, "2\.%02x", tmp);
		}
		else {
			strcpy(fw_ver_buf, "unknown");
		}
	}

	return fw_ver_buf;
}
#endif

#ifdef FTS_EXTENSION
#include "fts_extension.c"

static void fts_get_upgrade_info(struct fts_info *ts)
{
	switch (ts->chip_id) {
	case FT5x06_ID:
		ts->fw_upgrade_info.id1 = FT5x06_UPGRADE_ID1;
		ts->fw_upgrade_info.id2 = FT5x06_UPGRADE_ID2;
		ts->fw_upgrade_info.delay_aa = FT5x06_UPGRADE_AA_DELAY;
		ts->fw_upgrade_info.delay_55 = FT5x06_UPGRADE_55_DELAY;
		ts->fw_upgrade_info.delay_readid = FT5x06_UPGRADE_READID_DELAY;
		ts->fw_upgrade_info.delay_earse = FT5x06_UPGRADE_EARSE_DELAY;
		break;
	case FT5x16_ID:
		ts->fw_upgrade_info.id1 = FT5x16_UPGRADE_ID1;
		ts->fw_upgrade_info.id2 = FT5x16_UPGRADE_ID2;
		ts->fw_upgrade_info.delay_aa = FT5x16_UPGRADE_AA_DELAY;
		ts->fw_upgrade_info.delay_55 = FT5x16_UPGRADE_55_DELAY;
		ts->fw_upgrade_info.delay_readid = FT5x16_UPGRADE_READID_DELAY;
		ts->fw_upgrade_info.delay_earse = FT5x16_UPGRADE_EARSE_DELAY;
		break;
	case FT5606_ID:
		ts->fw_upgrade_info.id1 = FT5606_UPGRADE_ID1;
		ts->fw_upgrade_info.id2 = FT5606_UPGRADE_ID2;
		ts->fw_upgrade_info.delay_aa = FT5606_UPGRADE_AA_DELAY;
		ts->fw_upgrade_info.delay_55 = FT5606_UPGRADE_55_DELAY;
		ts->fw_upgrade_info.delay_readid = FT5606_UPGRADE_READID_DELAY;
		ts->fw_upgrade_info.delay_earse = FT5606_UPGRADE_EARSE_DELAY;
		break;
	case FT6x06_ID:
		ts->fw_upgrade_info.id1 = FT6x06_UPGRADE_ID1;
		ts->fw_upgrade_info.id2 = FT6x06_UPGRADE_ID2;
		ts->fw_upgrade_info.delay_aa = FT6x06_UPGRADE_AA_DELAY;
		ts->fw_upgrade_info.delay_55 = FT6x06_UPGRADE_55_DELAY;
		ts->fw_upgrade_info.delay_readid = FT6x06_UPGRADE_READID_DELAY;
		ts->fw_upgrade_info.delay_earse = FT6x06_UPGRADE_EARSE_DELAY;
		break;
	default:
		break;
	}
}
#endif

static int fts_verify_dev(struct fts_info *ts)
{

	u8 id;

	if(fts_readb(ts, 0xA3, &id))
	{
		fts_msg("Can't get chip id\n");
		return -ENODEV;
	}

#ifdef WISTRON_EXTENSION
	if(ts->client->addr == CTPM_EDT_I2C_ADDR) {
		ts->ctpm_id = CTPM_OGS_ID;
	}
	else {
		ts->ctpm_id = CTPM_GG_ID;
	}
	fts_msg("FTS CTPM ID = 0x%02X !\n", ts->ctpm_id);
#endif

	switch (id){
		case FT5x06_ID:
			fts_msg("FTS FT5x06 Chip\n");
			break;		
		case FT5x16_ID:
			fts_msg("FTS FT5x16 Chip\n");
			break;		
		case FT5606_ID:
			fts_msg("FTS FT5606 Chip\n");
			break;
		case FT6x06_ID:
			fts_msg("FTS FT6X06 Chip\n");
			break;
		default:
			fts_msg("Can't support unknown chip\n");
			return -ENODEV;
			break;
	}

	ts->chip_id = id;
	fts_msg("FTS Chip ID = 0x%02X !\n", ts->chip_id);

#ifdef FTS_EXTENSION
	fts_get_upgrade_info(ts);
#endif
	return 0;
}

static void fts_input_report(struct fts_info *ts, struct fts_packet_info *buf)
{

	int i, j, x, y;
	int touch_type;
	struct fts_touch_info *touch;

	touch = &buf->touch[0];
	touch_type = 0;

	DECLARE_BITMAP(used, FTS_MAX_TOUCH);

	bitmap_zero(used, FTS_MAX_TOUCH);

	for(i = 0; (touch->id < 0x0F) && (i < FTS_MAX_TOUCH); i++, touch++){

		x = (u16)(touch->x_msb << 8) | (u16)touch->x_lsb;
		y = (u16)(touch->y_msb << 8) | (u16)touch->y_lsb;

		fts_dbg("ID = %d, Event = %d, X = %d, Y = %d\n", touch->id, touch->event, x, y);

#ifdef TOUCH_REPORT_TYPE_A
		if ((touch->event == F_DOWN) || (touch->event == F_CONTACT)){
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, FTS_MAX_TOUCH_PRESS);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MINOR, FTS_MAX_TOUCH_PRESS);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
			input_mt_sync(ts->input_dev);
			touch_type |= POINTER_TOUCH;
		}
#else
		input_mt_slot(ts->input_dev, touch->id);


		if ((touch->event == F_DOWN) || (touch->event == F_CONTACT)){
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, true);
			input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, FTS_MAX_TOUCH_PRESS);
			input_report_abs(ts->input_dev, ABS_MT_PRESSURE, FTS_MAX_TOUCH_PRESS);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_X, x);
			input_report_abs(ts->input_dev, ABS_MT_POSITION_Y, y);
			__set_bit(touch->id, used);  
		}
		else{
			input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);
		}

		fts_dbg("ID %d Event %d, X = %d, Y = %d\n", touch->id, touch->event, x, y);

		touch_type |= POINTER_TOUCH;
#endif
	}

#ifdef TOUCH_REPORT_TYPE_B
	for (i = 0; i < FTS_MAX_TOUCH; i++) {
		if (test_bit(i, used))
			continue;

		input_mt_slot(ts->input_dev, i);
		input_mt_report_slot_state(ts->input_dev, MT_TOOL_FINGER, false);
	}
#endif

	if(touch_type){
#ifdef TOUCH_REPORT_TYPE_A
		if(touch_type & POINTER_TOUCH){
			input_report_abs(ts->input_dev, ABS_X, x);
			input_report_abs(ts->input_dev, ABS_Y, y);
			input_report_key(ts->input_dev, BTN_TOUCH, touch_type);
			input_report_key(ts->input_dev, BTN_TOOL_FINGER, touch_type);
		}
#else
		if(touch_type & POINTER_TOUCH)
			input_mt_report_pointer_emulation(ts->input_dev, false);
#endif

		input_sync(ts->input_dev);
	}
#ifdef TOUCH_REPORT_TYPE_A
	else{
		input_report_abs(ts->input_dev, ABS_MT_TOUCH_MAJOR, 0);
		input_report_abs(ts->input_dev, ABS_MT_TOUCH_MINOR, 0);
		input_mt_sync(ts->input_dev);
		input_report_key(ts->input_dev, BTN_TOUCH, 0);
		input_report_key(ts->input_dev, BTN_TOOL_FINGER, 0);
		input_sync(ts->input_dev);
	}
#endif

}

static irqreturn_t fts_irq(int irq, void *dev_id)
{
	struct fts_info *ts = dev_id;
	struct fts_packet_info buf;
	u8 addr = 0x01;

	if(fts_readsb(ts, &addr, (u8 *)&buf, sizeof(struct fts_packet_info)))
		goto exit;

  if(isOpen)
 	  fts_input_report(ts, &buf);

exit:
	return IRQ_HANDLED;
}

static void fts_suspend(struct fts_info *ts)
{

	int err;

	fts_dbg("called\n");

	disable_irq(ts->client->irq);

#ifdef HW_PWR_ONOFF
	/* Put reset pin in low and delay 2ms */
	fts_gpio_reset(ts,PULL_RESET_LOW);
	/* hardware power off */
	fts_hw_power_on(ts,PWR_OFF);
#else
	if((err = fts_writeb(ts, REG_PWR_MODE, P_HIBERNATE)))
		fts_msg("touchpanel early suspend fail %d", err);

#endif

}

static void fts_resume(struct fts_info *ts)
{

	fts_dbg("called\n");

#ifdef HW_PWR_ONOFF
	/* hardware power on */
	fts_hw_power_on(ts,PWR_ON);
#endif

	fts_gpio_reset(ts,PULL_RESET);

	enable_irq(ts->client->irq);
}

static int fts_init_dev(struct fts_info *ts)
{

	if(fts_config_gpio())
		return -ENODEV;

#ifdef HW_PWR_ONOFF
#ifdef WISTRON_EXTENSION
	fts_probe_power_on(ts);
#else
	/* hardware power on */
	fts_hw_power_on(ts,PWR_ON);
#endif
#endif
	fts_gpio_reset(ts,PULL_RESET);

	return fts_verify_dev(ts);
}

static int fts_open(struct input_dev *dev)
{
	struct fts_info *ts = input_get_drvdata(dev);

	enable_irq(ts->client->irq);
  isOpen = true;

	return 0;
}

static void fts_close(struct input_dev *dev)
{
	struct fts_info *ts = input_get_drvdata(dev);

	disable_irq(ts->client->irq);

  isOpen = false;

}

static int fts_probe(struct i2c_client *client, const struct i2c_device_id *id)
{

	struct input_dev *input_dev;
	struct fts_info *ts;
	int i, err = -ENOMEM;
       int fw_update=0;
	u8 ver = 0;
       char *str;
	struct fts_platform_data *pdata =
	    (struct fts_platform_data *)client->dev.platform_data;

#ifdef WISTRON_EXTENSION
	probe_state ++;
#endif
	if(!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)){
		fts_msg("FTS: need I2C_FUNC_I2C\n");
		return -ENODEV;
	}

	ts = kzalloc (sizeof(struct fts_info), GFP_KERNEL);
	if (!ts)
		return err;

	/* register driver_data */
	ts->client = client;
	i2c_set_clientdata(client, ts);
	ts->pdata = pdata;

	fts_dbg("%s install %02X\n", DRIVER_DESC, client->addr);

	if((err = fts_init_dev(ts)) < 0)
		goto out_free;

	input_dev = input_allocate_device();
	if (!input_dev)
		goto out_free;

	input_dev->name = DRIVER_NAME;
	input_dev->id.bustype = BUS_I2C;
	input_dev->dev.parent = &client->dev;

	input_dev->open = fts_open;
	input_dev->close = fts_close;

	ts->input_dev = input_dev;

	set_bit(INPUT_PROP_DIRECT, input_dev->propbit);
#ifdef TOUCH_REPORT_TYPE_B
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(EV_ABS, input_dev->evbit);
	set_bit(BTN_TOUCH, input_dev->keybit);

	//input_set_abs_params(input_dev, ABS_X, 0, MAX_X, 0, 0);
	//input_set_abs_params(input_dev, ABS_Y, 0, MAX_Y, 0, 0);
	//input_set_abs_params(input_dev, ABS_PRESSURE, 0, FTS_MAX_TOUCH_PRESS, 0, 0);

	input_mt_init_slots(input_dev, FTS_MAX_TOUCH);
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, MAX_X, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, MAX_Y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, FTS_MAX_TOUCH_PRESS, 0, 0);
	//input_set_abs_params(input_dev, ABS_MT_PRESSURE, 0, FTS_MAX_TOUCH_PRESS, 0, 0);
#else
	set_bit(BTN_TOUCH, input_dev->keybit);
	set_bit(BTN_TOOL_FINGER, input_dev->keybit);
	set_bit(EV_KEY, input_dev->evbit);
	set_bit(EV_ABS, input_dev->evbit);

	input_set_abs_params(input_dev, ABS_X, 0, MAX_X, 0, 0);
	input_set_abs_params(input_dev, ABS_Y, 0, MAX_Y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_X, 0, MAX_X, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_POSITION_Y, 0, MAX_Y, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MAJOR, 0, FTS_MAX_TOUCH_PRESS, 0, 0);
	input_set_abs_params(input_dev, ABS_MT_TOUCH_MINOR, 0, FTS_MAX_TOUCH_PRESS, 0, 0);
#endif

	input_set_drvdata(input_dev, ts);

	err = input_register_device(ts->input_dev);
	if (err) {
		fts_msg("input_register_device failed, err: %d", err);
		goto out_free_mem;
	}

	if(client->irq){
		fts_dbg("Using IRQ %d\n", client->irq);

		err = request_threaded_irq(client->irq, NULL, fts_irq, pdata->irqflags, client->dev.driver->name, ts);

		if(err){
			fts_msg("request_irq failed");
			goto out_unregister_device;
		}
	}
	else{
		fts_msg("No IRQ resource\n");
		goto out_unregister_device;
	}

	if(fts_init_sysfs(ts)) {
		fts_msg("create sysfs failed\n");
		fts_release_sysfs(ts);
	}

#ifdef WISTRON_EXTENSION
	if(fts_create_proc(ts)) {
		fts_msg("create proc failed\n");
	}

	probe_state |= PROBE_DONE;		   
#endif
	disable_irq(ts->client->irq);

	return 0;

out_unregister_device:
	input_unregister_device(input_dev);

out_free_mem:
	input_free_device(input_dev);

out_free:
#ifdef HW_PWR_ONOFF
#ifdef WISTRON_EXTENSION
	if (probe_state & ( PROBE_DONE | PROBE_1CTPM))
	{
		kfree(ts);
		return err;
	}
	//fts_msg("addr=[%02x] out_free probe_state[%x]! and close power\n",ts->client->addr,probe_state);
#endif
	/* Put reset pin in low and delay 2ms */
	fts_gpio_reset(ts,PULL_RESET_LOW);
	/* hardware power off */
	fts_hw_power_on(ts,PWR_OFF);
#endif
	kfree(ts);

	return err;
}

static int fts_remove(struct i2c_client *client)
{
	struct fts_info *ts = i2c_get_clientdata(client);

	fts_dbg("called\n");

	free_irq(client->irq, ts);

	input_unregister_device(ts->input_dev);
	input_free_device(ts->input_dev);

	fts_release_sysfs(ts);

#ifdef WISTRON_EXTENSION
	fts_release_proc(ts);
#endif
#ifdef HW_PWR_ONOFF
	/* Put reset pin in low and delay 2ms */
	fts_gpio_reset(ts,PULL_RESET_LOW);
	/* hardware power off */
	fts_hw_power_on(ts,PWR_OFF);
#endif
	kfree(ts);

	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int fts_i2c_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct fts_info *ts = i2c_get_clientdata(client);
	struct input_dev *input_dev = ts->input_dev;

	mutex_lock(&input_dev->mutex);

	if (input_dev->users)
		fts_suspend(ts);

	mutex_unlock(&input_dev->mutex);

	return 0;
}

static int fts_i2c_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct fts_info *ts = i2c_get_clientdata(client);
	struct input_dev *input_dev = ts->input_dev;

	mutex_lock(&input_dev->mutex);

	if (input_dev->users)
		fts_resume(ts);

	mutex_unlock(&input_dev->mutex);

	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(fts_pm_ops, fts_i2c_suspend, fts_i2c_resume);

static const struct i2c_device_id i2c_id[] = {
	{ DRIVER_NAME, 0 },
	{}
};

MODULE_DEVICE_TABLE(i2c, i2c_id);

static struct i2c_driver fts_i2c_driver = {
	.probe =		fts_probe,
	.remove =	fts_remove,
	.id_table =	i2c_id,
	.driver = {
		.name = DRIVER_NAME,
		.pm	= &fts_pm_ops,
	},
};

static int fts_init(void)
{

	return i2c_add_driver(&fts_i2c_driver);
}

static void fts_exit(void)
{

	i2c_del_driver(&fts_i2c_driver);
}

module_init(fts_init);
module_exit(fts_exit);

MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

