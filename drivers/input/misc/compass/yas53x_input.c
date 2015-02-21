/*
* Copyright (C) 2012 Invensense, Inc.
*
* This software is licensed under the terms of the GNU General Public
* License version 2, as published by the Free Software Foundation, and
* may be copied, distributed, and modified under those terms.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
*/

/**
 *  @addtogroup DRIVERS
 *  @brief	Hardware drivers.
 *
 *  @{
 *	  @file	   yas53x_input.c
 *	  @brief   A sysfs device driver for Invensense devices
 *	  @details This driver currently works for the YAS53X
 */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/hrtimer.h>
#include <linux/ktime.h>
     
#include <linux/mpu.h>


/**
 *  struct inv_compass_state - Driver state variables.
 *  @dev:		Represents read-only node for accessing buffered data.
 *  @idev:		Handle to input device.
 *  @sl_handle:		Handle to I2C port.
 */
struct inv_compass_state {
    struct i2c_client *i2c;
    atomic_t enable;
	atomic_t delay; /* msec */
	unsigned char compass_id;
    struct mpu_platform_data plat_data;
    short i2c_addr;
    void *sl_handle;
    struct device   *inv_dev;
    struct class	*compass;
    struct input_dev *idev;
    struct work_struct work;
    struct hrtimer timer;

    struct mutex value_mutex;
    struct mutex enable_mutex;
    short value[3];
};

enum yas53x_id {
    ID_YAS530 = 0,
    ID_YAS532,
};

#define YAS53X_REGADDR_DEVICE_ID          (0x80)
#define YAS53X_REGADDR_ACTUATE_INIT_COIL  (0x81)
#define YAS53X_REGADDR_MEASURE_COMMAND    (0x82)
#define YAS53X_REGADDR_CONFIG             (0x83)
#define YAS53X_REGADDR_MEASURE_INTERVAL   (0x84)
#define YAS53X_REGADDR_OFFSET_X           (0x85)
#define YAS53X_REGADDR_OFFSET_Y1          (0x86)
#define YAS53X_REGADDR_OFFSET_Y2          (0x87)
#define YAS53X_REGADDR_TEST1              (0x88)
#define YAS53X_REGADDR_TEST2              (0x89)
#define YAS53X_REGADDR_CAL                (0x90)
#define YAS53X_REGADDR_MEASURE_DATA       (0xb0)

#define YAS53X_MAX_DELAY                  (200)//(100)
#define YAS53X_MIN_DELAY                  (10)
#define YAS53X_DEFAULT_DELAY              (100)
/* -------------------------------------------------------------------------- */
static int Cx, Cy1, Cy2;
static int /*a1, */ a2, a3, a4, a5, a6, a7, a8, a9;
static int k;

static unsigned char dx, dy1, dy2;
static unsigned char d2, d3, d4, d5, d6, d7, d8, d9, d0;
static unsigned char dck;
static unsigned char ver;

/* -------------------------------------------------------------------------- */
static inline s64 get_time_ns(void)
{
	struct timespec ts;
	ktime_get_ts(&ts);
	return timespec_to_ns(&ts);
}

/**
 *  inv_serial_read() - Read one or more bytes from the device registers.
 *  @st:	Device driver instance.
 *  @reg:	First device register to be read from.
 *  @length:	Number of bytes to read.
 *  @data:	Data read from device.
 *  NOTE: The slave register will not increment when reading from the FIFO.
 */
static int inv_serial_read(struct inv_compass_state *st,
	unsigned char reg, unsigned short length, unsigned char *data)
{
	struct i2c_msg msgs[2];
	int res;

	if (!data || !st->sl_handle)
		return -EINVAL;

	msgs[0].addr = st->i2c_addr;
	msgs[0].flags = 0;	/* write */
	msgs[0].buf = &reg;
	msgs[0].len = 1;

	msgs[1].addr = st->i2c_addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].buf = data;
	msgs[1].len = length;

	res = i2c_transfer(st->sl_handle, msgs, 2);
	if (res < 2) {
		if (res >= 0)
			res = -EIO;
		return res;
	} else
		return 0;
}

/**
 *  inv_serial_single_write() - Write a byte to a device register.
 *  @st:	Device driver instance.
 *  @reg:	Device register to be written to.
 *  @data:	Byte to write to device.
 */
static int inv_serial_single_write(struct inv_compass_state *st,
	unsigned char reg, unsigned char data)
{
	unsigned char tmp[2];
	struct i2c_msg msg;
	int res;

	if (!st->sl_handle)
		return -EINVAL;

	tmp[0] = reg;
	tmp[1] = data;

	msg.addr = st->i2c_addr;
	msg.flags = 0;	/* write */
	msg.buf = tmp;
	msg.len = 2;

	/* printk(KERN_ERR "WS%02X%02X%02X\n", st->i2c_addr, reg, data); */
	res = i2c_transfer(st->sl_handle, &msg, 1);
	if (res < 1) {
		if (res == 0)
			res = -EIO;
		return res;
	} else
		return 0;
}

static int set_hardware_offset(struct inv_compass_state *st,
			       char offset_x, char offset_y1, char offset_y2)
{
	char data;
	int result = 0;

	data = offset_x & 0x3f;
	result = inv_serial_single_write(st, YAS53X_REGADDR_OFFSET_X, data);
	if (result)
		return result;

	data = offset_y1 & 0x3f;
	result = inv_serial_single_write(st, YAS53X_REGADDR_OFFSET_Y1, data);
	if (result)
		return result;

	data = offset_y2 & 0x3f;
	result = inv_serial_single_write(st, YAS53X_REGADDR_OFFSET_Y2, data);
	return result;
}

static int set_measure_command(struct inv_compass_state *st)
{
	int result = 0;
	result = inv_serial_single_write(st,
					 YAS53X_REGADDR_MEASURE_COMMAND, 0x01);
	return result;
}

static int measure_normal(struct inv_compass_state *st,
			  int *busy, unsigned short *t,
			  unsigned short *x, unsigned short *y1,
			  unsigned short *y2)
{
	unsigned char data[8];
	unsigned short b, to, xo, y1o, y2o;
	int result;
	ktime_t sleeptime;
	result = set_measure_command(st);
	sleeptime = ktime_set(0, 2 * NSEC_PER_MSEC);
	set_current_state(TASK_UNINTERRUPTIBLE);
	schedule_hrtimeout(&sleeptime, HRTIMER_MODE_REL);

	result = inv_serial_read(st,
				 YAS53X_REGADDR_MEASURE_DATA, 8, data);
	if (result)
		return result;

    if (st->compass_id == ID_YAS530) {
    	b = (data[0] >> 7) & 0x01;
    	to = ((data[0] << 2) & 0x1fc) | ((data[1] >> 6) & 0x03);
    	xo = ((data[2] << 5) & 0xfe0) | ((data[3] >> 3) & 0x1f);
    	y1o = ((data[4] << 5) & 0xfe0) | ((data[5] >> 3) & 0x1f);
    	y2o = ((data[6] << 5) & 0xfe0) | ((data[7] >> 3) & 0x1f);
    }
    else if (st->compass_id == ID_YAS532) {
        b = (data[0] >> 7) & 0x01;
        to = ((data[0] << 3) & 0x3f8) | ((data[1] >> 5) & 0x07);
        xo = ((data[2] << 6) & 0x1fc0) | ((data[3] >> 2) & 0x3f);
        y1o = ((data[4] << 6) & 0x1fc0) | ((data[5] >> 2) & 0x3f);
        y2o = ((data[6] << 6) & 0x1fc0) | ((data[7] >> 2) & 0x3f);        
    }
    
	*busy = b;
	*t = to;
	*x = xo;
	*y1 = y1o;
	*y2 = y2o;

	return result;
}

static int measure_int(struct inv_compass_state *st,
			  int *busy, unsigned short *t,
			  unsigned short *x, unsigned short *y1,
			  unsigned short *y2)
{
	unsigned char data[8];
	unsigned short b, to, xo, y1o, y2o;
	int result;
	result = inv_serial_read(st,
				 YAS53X_REGADDR_MEASURE_DATA, 8, data);
	if (result)
		return result;

    if (st->compass_id == ID_YAS530) {
    	b = (data[0] >> 7) & 0x01;
    	to = ((data[0] << 2) & 0x1fc) | ((data[1] >> 6) & 0x03);
    	xo = ((data[2] << 5) & 0xfe0) | ((data[3] >> 3) & 0x1f);
    	y1o = ((data[4] << 5) & 0xfe0) | ((data[5] >> 3) & 0x1f);
    	y2o = ((data[6] << 5) & 0xfe0) | ((data[7] >> 3) & 0x1f);
    }
    else if (st->compass_id == ID_YAS532) {
        b = (data[0] >> 7) & 0x01;
        to = ((data[0] << 3) & 0x3f8) | ((data[1] >> 5) & 0x07);
        xo = ((data[2] << 6) & 0x1fc0) | ((data[3] >> 2) & 0x3f);
        y1o = ((data[4] << 6) & 0x1fc0) | ((data[5] >> 2) & 0x3f);
        y2o = ((data[6] << 6) & 0x1fc0) | ((data[7] >> 2) & 0x3f);        
    }

	*busy = b;
	*t = to;
	*x = xo;
	*y1 = y1o;
	*y2 = y2o;

	result = set_measure_command(st);
	return result;
}

static int check_offset(struct inv_compass_state *st,
			char offset_x, char offset_y1, char offset_y2,
			int *flag_x, int *flag_y1, int *flag_y2)
{
	int result;
	int busy;
	short t, x, y1, y2;
    short offset_chk_limit=2048;

	result = set_hardware_offset(st, offset_x, offset_y1, offset_y2);
	if (result)
		return result;
	result = measure_normal(st, &busy, &t, &x, &y1, &y2);
	if (result)
		return result;
	*flag_x = 0;
	*flag_y1 = 0;
	*flag_y2 = 0;

    if (st->compass_id == ID_YAS530)
        offset_chk_limit = 2048;
    else if (st->compass_id == ID_YAS532)
        offset_chk_limit = 4096;

	if (x > offset_chk_limit)
		*flag_x = 1;
	if (y1 > offset_chk_limit)
		*flag_y1 = 1;
	if (y2 > offset_chk_limit)
		*flag_y2 = 1;
	if (x < offset_chk_limit)
		*flag_x = -1;
	if (y1 < offset_chk_limit)
		*flag_y1 = -1;
	if (y2 < offset_chk_limit)
		*flag_y2 = -1;

	return result;
}

static int measure_and_set_offset(struct inv_compass_state *st,
				  char *offset)
{
	int i;
	int result = 0;
	char offset_x = 0, offset_y1 = 0, offset_y2 = 0;
	int flag_x = 0, flag_y1 = 0, flag_y2 = 0;
	static const int correct[5] = { 16, 8, 4, 2, 1 };

	for (i = 0; i < 5; i++) {
		result = check_offset(st,
				      offset_x, offset_y1, offset_y2,
				      &flag_x, &flag_y1, &flag_y2);
		if (result)
			return result;
		if (flag_x)
			offset_x += flag_x * correct[i];
		if (flag_y1)
			offset_y1 += flag_y1 * correct[i];
		if (flag_y2)
			offset_y2 += flag_y2 * correct[i];
	}

	result = set_hardware_offset(st, offset_x, offset_y1, offset_y2);
	if (result)
		return result;
	offset[0] = offset_x;
	offset[1] = offset_y1;
	offset[2] = offset_y2;

	return result;
}

static void coordinate_conversion(short x, short y1, short y2, short t,
				  int32_t *xo, int32_t *yo, int32_t *zo)
{
	int32_t sx, sy1, sy2, sy, sz;
	int32_t hx, hy, hz;

	sx = x - (Cx * t) / 100;
	sy1 = y1 - (Cy1 * t) / 100;
	sy2 = y2 - (Cy2 * t) / 100;

	sy = sy1 - sy2;
	sz = -sy1 - sy2;

	hx = k * ((100 * sx + a2 * sy + a3 * sz) / 10);
	hy = k * ((a4 * sx + a5 * sy + a6 * sz) / 10);
	hz = k * ((a7 * sx + a8 * sy + a9 * sz) / 10);

	*xo = hx;
	*yo = hy;
	*zo = hz;
}

static int yas53x_init(struct inv_compass_state *st)
{
	int result = 0;
	unsigned char dummyData = 0x00;
	unsigned char read_reg[1];

	/* =============================================== */

	/* Step 1 - Test register initialization */
	dummyData = 0x00;
	result = inv_serial_single_write(st,
					 YAS53X_REGADDR_TEST1, dummyData);
	if (result)
		return result;
	result =
	    inv_serial_single_write(st,
				    YAS53X_REGADDR_TEST2, dummyData);
	if (result)
		return result;
	/* Device ID read  */
	result = inv_serial_read(st,
				 YAS53X_REGADDR_DEVICE_ID, 1, read_reg);
    printk("%s, DEVICE_ID[reg0x%x]=0x%x\n", __func__, YAS53X_REGADDR_DEVICE_ID, read_reg[0]);

    return result;
}

static int yas53x_resume(struct inv_compass_state *st)
{
	int result = 0;

	unsigned char dummyData = 0x00;
	char offset[3] = { 0, 0, 0 };
	unsigned char data[16], cal_read_len=16;
	unsigned char read_reg[1];

	/* =============================================== */

	/* Step 1 - Test register initialization */
	dummyData = 0x00;
	result = inv_serial_single_write(st,
					 YAS53X_REGADDR_TEST1, dummyData);
	if (result)
		return result;
	result =
	    inv_serial_single_write(st,
				    YAS53X_REGADDR_TEST2, dummyData);
	if (result)
		return result;
	/* Device ID read  */
	result = inv_serial_read(st,
				 YAS53X_REGADDR_DEVICE_ID, 1, read_reg);
    printk("%s, DEVICE_ID[reg0x%x]=0x%x\n", __func__, YAS53X_REGADDR_DEVICE_ID, read_reg[0]);

	/*Step 2 Read the CAL register */    
    if (st->compass_id == ID_YAS530)
        cal_read_len =16;
    else if (st->compass_id == ID_YAS532)
        cal_read_len =14;
        
	/* CAL data read */
	result = inv_serial_read(st,
				 YAS53X_REGADDR_CAL, cal_read_len, data);
	if (result)
		return result;
	/* CAL data Second Read */
	result = inv_serial_read(st,
				 YAS53X_REGADDR_CAL, cal_read_len, data);
	if (result)
		return result;
	/*Cal data */
	dx = data[0];
	dy1 = data[1];
	dy2 = data[2];
	d2 = (data[3] >> 2) & 0x03f;
	d3 = ((data[3] << 2) & 0x0c) | ((data[4] >> 6) & 0x03);
	d4 = data[4] & 0x3f;
	d5 = (data[5] >> 2) & 0x3f;
	d6 = ((data[5] << 4) & 0x30) | ((data[6] >> 4) & 0x0f);
	d7 = ((data[6] << 3) & 0x78) | ((data[7] >> 5) & 0x07);
	d8 = ((data[7] << 1) & 0x3e) | ((data[8] >> 7) & 0x01);
	d9 = ((data[8] << 1) & 0xfe) | ((data[9] >> 7) & 0x01);
	d0 = (data[9] >> 2) & 0x1f;
	dck = ((data[9] << 1) & 0x06) | ((data[10] >> 7) & 0x01);
    if (st->compass_id == ID_YAS532) {
        ver = (data[13]) & 0x01;
        printk("%s, yas532, ver=0x%x\n", __func__, ver);
    }
    
	/*Correction Data */
    if (st->compass_id == ID_YAS530) {    
    	Cx = (int)dx * 6 - 768;
    	Cy1 = (int)dy1 * 6 - 768;
    	Cy2 = (int)dy2 * 6 - 768;
    	a2 = (int)d2 - 32;
    	a3 = (int)d3 - 8;
    	a4 = (int)d4 - 32;
    	a5 = (int)d5 + 38;
    	a6 = (int)d6 - 32;
    	a7 = (int)d7 - 64;
    	a8 = (int)d8 - 32;
    	a9 = (int)d9;
    	k = (int)d0 + 10;
    }
    else if (st->compass_id == ID_YAS532) {
        Cx = (int)dx * 10 - 1280;
        Cy1 = (int)dy1 * 10 - 1280;
        Cy2 = (int)dy2 * 10 - 1280;
        a2 = (int)d2 - 32;
        a3 = (int)d3 - 8;
        a4 = (int)d4 - 32;
        a5 = (int)d5 + 38;
        a6 = (int)d6 - 32;
        a7 = (int)d7 - 64;
        a8 = (int)d8 - 32;
        a9 = (int)d9;
        k = (int)d0;        
    }
    
	/*Obtain the [49:47] bits */
	dck &= 0x07;

	/*Step 3 : Storing the CONFIG with the CLK value */
	dummyData = 0x00 | (dck << 2);
	result = inv_serial_single_write(st,
					 YAS53X_REGADDR_CONFIG, dummyData);
	if (result)
		return result;
	/*Step 4 : Set Acquisition Interval Register */
	dummyData = 0x00;
	result = inv_serial_single_write(st,
					 YAS53X_REGADDR_MEASURE_INTERVAL,
					 dummyData);
	if (result)
		return result;

	/*Step 5 : Reset Coil */
	dummyData = 0x00;
	result = inv_serial_single_write(st,
					 YAS53X_REGADDR_ACTUATE_INIT_COIL,
					 dummyData);
	if (result)
		return result;
    
	/* Offset Measurement and Set */
	result = measure_and_set_offset(st, offset);

	return result;
}

static int yas53x_read(struct inv_compass_state *st, short rawfixed[3])
{
	int result = 0;

	int busy;
	short t, x, y1, y2;
	int32_t xyz[3];

	result = measure_int(st, &busy, &t, &x, &y1, &y2);
	if (result)
		return result;
	coordinate_conversion(x, y1, y2, t, &xyz[0], &xyz[1], &xyz[2]);

	rawfixed[0] = (short)(xyz[0] / 100);
	rawfixed[1] = (short)(xyz[1] / 100);
	rawfixed[2] = (short)(xyz[2] / 100);
	if (busy)
		return -1;
	return result;
}

#define HRTIMER_RESTART_INSIDE_TIMERISR 0
static enum hrtimer_restart yas53x_timer_func(struct hrtimer *timer)   
{
	struct inv_compass_state *st =
		container_of(timer, struct inv_compass_state, timer);

    if (atomic_read(&st->enable)) {
        schedule_work(&st->work);
#if HRTIMER_RESTART_INSIDE_TIMERISR==1
        hrtimer_forward_now(timer, ns_to_ktime((atomic_read(&st->delay))*1000000));
        return HRTIMER_RESTART;
#else        
        return HRTIMER_NORESTART;  
#endif        
    } else {
        return HRTIMER_NORESTART;  
    }
}  

static void yas53x_work_func(struct work_struct *work)
{
	struct inv_compass_state *st =
		container_of(work, struct inv_compass_state, work);
	long long timestamp;    
	short c[3];
    
#if HRTIMER_RESTART_INSIDE_TIMERISR==1
#else
        ktime_t ktime;
        if (atomic_read(&st->enable) == 0)
            return;        
        ktime = ktime_set(0, (atomic_read(&st->delay))*1000000);
        hrtimer_start(&st->timer, ktime ,HRTIMER_MODE_REL);  
#endif

	timestamp = get_time_ns();
	c[0] = c[1] = c[2] = 0;
	if (0 == yas53x_read(st, c)) {        
	} else {
		c[0] = st->value[0];
		c[1] = st->value[1];
		c[2] = st->value[2];
	}

    input_report_rel(st->idev, REL_X, c[0]);
    input_report_rel(st->idev, REL_Y, c[1]);
    input_report_rel(st->idev, REL_Z, c[2]);
    input_report_rel(st->idev, REL_MISC, (unsigned int)(timestamp >> 32));
    input_report_rel(st->idev, REL_WHEEL, (unsigned int)(timestamp & 0xffffffff));
    input_sync(st->idev);
    //printk("compass %d, %d, %d, ns=%lld\n", c[0], c[1], c[2], timestamp);        

	mutex_lock(&st->value_mutex);
	st->value[0] = c[0];
	st->value[1] = c[1];
	st->value[2] = c[2];
	mutex_unlock(&st->value_mutex);
    
}

static ssize_t yas53x_value_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_compass_state *st = dev_get_drvdata(dev);
	short c[3];

	mutex_lock(&st->value_mutex);
	c[0] = st->value[0];
	c[1] = st->value[1];
	c[2] = st->value[2];
	mutex_unlock(&st->value_mutex);
	return sprintf(buf, "%d, %d, %d\n", c[0], c[1], c[2]);
}

static ssize_t yas53x_scale_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	/* The sensitivity is 0.1 uT/LSB for all axes based on the conversion
	   performed from X, Y1, Y1 to X, Y, Z coordinates and adjustments.
	   The scale is the 0.1 * 2^15 = 3276.8 uT
	   The scale value is represented in q15 format, therefore multipled
	   by 2^15 */
	return sprintf(buf, "%ld\n", 107374182L);
}

static ssize_t yas53x_reset_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_compass_state *st = dev_get_drvdata(dev);
	int result;
	int enable = atomic_read(&st->enable);

    if (enable)
        return -EPERM;
    
	result = yas53x_resume(st);
	return sprintf(buf, "%d\n", result);
}

static ssize_t yas53x_enable_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_compass_state *st = dev_get_drvdata(dev);
    int enable;    
	mutex_lock(&st->enable_mutex);
    enable = atomic_read(&st->enable);    
	mutex_unlock(&st->enable_mutex);    
	return sprintf(buf, "%d\n", enable);
}

//"hz" to user space
static ssize_t yas53x_rate_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct inv_compass_state *st = dev_get_drvdata(dev);
	/* transform delay in ms to hz */
    unsigned int delay = atomic_read(&st->delay);
	return sprintf(buf, "%d\n", 1000/delay);
}

/**
 * yas53x_matrix_show() - show orientation matrix
 */
static ssize_t yas53x_matrix_show(struct device *dev,
	    struct device_attribute *attr, char *buf)
{
	struct inv_compass_state *st = dev_get_drvdata(dev);
	signed char *m;
	m = st->plat_data.orientation;
	return sprintf(buf,
	"%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
		m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
}

//"hz" from user space
static ssize_t yas53x_rate_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long data;
	int error;
	struct inv_compass_state *st = dev_get_drvdata(dev);

	error = strict_strtoul(buf, 10, &data);
	if (error)
		return error;
	/* transform hz to delay in ms */
	data = 1000 / data;
	if (data > YAS53X_MAX_DELAY)
		data = YAS53X_MAX_DELAY;
	if (data < YAS53X_MIN_DELAY)
		data = YAS53X_MIN_DELAY;
	atomic_set(&st->delay, (unsigned int) data);
	return count;
}

static void yas53x_set_enable(struct device *dev, int enable)
{
	struct inv_compass_state *st = dev_get_drvdata(dev);
	int result = 0;    
	int pre_enable = atomic_read(&st->enable);
    ktime_t ktime;

	mutex_lock(&st->enable_mutex);
	if (enable) {
		if (pre_enable == 0) {
            result = yas53x_resume(st);            
        	if (result) {
        		pr_err("%s, line=%d\n", __func__, __LINE__);
        	}
			atomic_set(&st->enable, 1);
            ktime = ktime_set(0, (atomic_read(&st->delay))*1000000);
            hrtimer_start(&st->timer, ktime ,HRTIMER_MODE_REL);  
		}

	} else {
		if (pre_enable == 1) {
			atomic_set(&st->enable, 0);            
            cancel_work_sync(&st->work);
            atomic_set(&st->delay, YAS53X_DEFAULT_DELAY);
		}
	}
	mutex_unlock(&st->enable_mutex);
}

static ssize_t yas53x_enable_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t count)
{
	unsigned long data;
	int error;

	error = strict_strtoul(buf, 10, &data);
	if (error)
		return error;
	if ((data == 0) || (data == 1))
		yas53x_set_enable(dev, data);

	return count;
}

static DEVICE_ATTR(enable, S_IRUGO | S_IWUSR, yas53x_enable_show, yas53x_enable_store);
static DEVICE_ATTR(value, S_IRUGO, yas53x_value_show, NULL);
static DEVICE_ATTR(scale, S_IRUGO, yas53x_scale_show, NULL);
static DEVICE_ATTR(reset, S_IRUGO, yas53x_reset_show, NULL);
static DEVICE_ATTR(rate, S_IRUGO | S_IWUSR, yas53x_rate_show, yas53x_rate_store);
static DEVICE_ATTR(matrix, S_IRUGO, yas53x_matrix_show, NULL);

#define CLASS_SYSFS 1
#define INPUT_SYSFS 0

#if CLASS_SYSFS
static struct device_attribute *yas53x_attributes[] = {
	&dev_attr_enable,
	&dev_attr_value,
	&dev_attr_scale,
	&dev_attr_reset,
	&dev_attr_rate,
    &dev_attr_matrix,
    NULL
};
#endif

#if INPUT_SYSFS
static struct attribute *yas53x_attributes[] = {
	&dev_attr_enable.attr,
	&dev_attr_value.attr,
	&dev_attr_scale.attr,
	&dev_attr_reset.attr,
	&dev_attr_rate.attr,
    &dev_attr_matrix.attr,
	NULL
};

static struct attribute_group yas53x_attribute_group = {
	.name = "yas53x",
	.attrs = yas53x_attributes
};
#endif

#if CLASS_SYSFS
/***** sysfs functions ******************************************/
static int create_device_attributes(
	struct device *dev,
	struct device_attribute *attrs[])
{
	int i;
	int err = 0;

	for (i = 0 ; NULL != attrs[i] ; ++i) {
		err = device_create_file(dev, attrs[i]);
		if (0 != err)
			break;
	}

	if (0 != err) {
		for (; i >= 0 ; --i)
			device_remove_file(dev, attrs[i]);
	}

	return err;
}

static void remove_device_attributes(
	struct device *dev,
	struct device_attribute *attrs[])
{
	int i;

	for (i = 0 ; NULL != attrs[i] ; ++i)
		device_remove_file(dev, attrs[i]);
}

#define YAS_MINOR_NUMBER    254
static char const *const compass_class_name = "invensense_compass";
static char const *const yas_device_name = "yas53x";
static char const *const device_link_name = "i2c";
static dev_t const yas_device_dev_t = MKDEV(MISC_MAJOR, YAS_MINOR_NUMBER);
static int create_sysfs_interfaces(struct inv_compass_state *st)
{
	int err;

	if (NULL == st)
		return -EINVAL;

	err = 0;

	st->compass = class_create(THIS_MODULE, compass_class_name);
	if (IS_ERR(st->compass)) {
		err = PTR_ERR(st->compass);
		goto exit_class_create_failed;
	}

	st->inv_dev = device_create(
						st->compass,
						NULL,
						yas_device_dev_t,
						st,
						yas_device_name);
	if (IS_ERR(st->inv_dev)) {
		err = PTR_ERR(st->inv_dev);
		goto exit_class_device_create_failed;
	}

	err = sysfs_create_link(
			&st->inv_dev->kobj,
			&st->i2c->dev.kobj,
			device_link_name);
	if (0 > err)
		goto exit_sysfs_create_link_failed;

	err = create_device_attributes(
			st->inv_dev,
			yas53x_attributes);
	if (0 > err)
		goto exit_device_attributes_create_failed;

	return err;

exit_device_attributes_create_failed:
	sysfs_remove_link(&st->inv_dev->kobj, device_link_name);
exit_sysfs_create_link_failed:
	device_destroy(st->compass, yas_device_dev_t);
exit_class_device_create_failed:
	st->inv_dev = NULL;
	class_destroy(st->compass);
exit_class_create_failed:
	st->compass = NULL;
	return err;
}

static void remove_sysfs_interfaces(struct inv_compass_state *st)
{
	if (NULL == st)
		return;

	if (NULL != st->inv_dev) {
		remove_device_attributes(
			st->inv_dev,
			yas53x_attributes);
		sysfs_remove_link(
			&st->inv_dev->kobj,
			device_link_name);
		st->inv_dev = NULL;
	}
	if (NULL != st->compass) {
		device_destroy(
			st->compass,
			yas_device_dev_t);
		class_destroy(st->compass);
		st->compass = NULL;
	}
}
#endif

/**
 *  inv_setup_input() - internal setup input device.
 *  @st:	Device driver instance.
 *  @**idev_in  pointer to input device
 *  @*client    i2c client
 *  @*name      name of the input device.
 */
static int inv_setup_input(struct inv_compass_state *st,
	struct input_dev **idev_in, struct i2c_client *client,
	unsigned char *name) {
	int result;
	struct input_dev *idev;
	idev = input_allocate_device();
	if (!idev) {
		result = -ENOMEM;
		return result;
	}
	/* Setup input device. */
	idev->name = name;

	idev->id.bustype = BUS_I2C;
	idev->id.product = 'S';
	idev->id.vendor     = ('I'<<8) | 'S';
	idev->id.version    = 1;
	idev->dev.parent = &client->dev;
	/* Open and close method. */
	idev->open = NULL;
	idev->close = NULL;

	__set_bit(EV_REL, idev->evbit);
	input_set_capability(idev, EV_REL, REL_X);
	input_set_capability(idev, EV_REL, REL_Y);
	input_set_capability(idev, EV_REL, REL_Z);

	input_set_capability(idev, EV_REL, REL_MISC);
	input_set_capability(idev, EV_REL, REL_WHEEL);

	input_set_drvdata(idev, st);
	result = input_register_device(idev);
	if (result)
		input_free_device(idev);

	*idev_in = idev;
	return result;
}
static unsigned short normal_i2c[] = { I2C_CLIENT_END };

static int yas53x_mod_probe(struct i2c_client *client,
			   const struct i2c_device_id *devid)
{
	struct mpu_platform_data *pdata;
	struct inv_compass_state *st;
	struct input_dev *idev;
	int result = 0;

	dev_info(&client->adapter->dev, "%s: %s\n", __func__, devid->name);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		result = -ENODEV;
		goto out_no_free;
	}

	pdata = (struct mpu_platform_data *)dev_get_platdata(&client->dev);
	if (!pdata) {
		dev_err(&client->adapter->dev,
			"Missing platform data for slave %s\n", devid->name);
		result = -EFAULT;
		goto out_no_free;
	}

	st = kzalloc(sizeof(*st), GFP_KERNEL);
	if (!st) {
		result = -ENOMEM;
		goto out_no_free;
	}

	i2c_set_clientdata(client, st);
	st->i2c = client;
	mutex_init(&st->value_mutex);
	mutex_init(&st->enable_mutex);
	atomic_set(&st->delay, YAS53X_DEFAULT_DELAY);
	atomic_set(&st->enable, 0);    
	st->sl_handle = client->adapter;
	st->plat_data = *pdata;
	st->i2c_addr = client->addr;
    st->compass_id = devid->driver_data;
    
	INIT_WORK(&st->work, yas53x_work_func);
    hrtimer_init(&st->timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    st->timer.function = yas53x_timer_func;
    
	result = inv_setup_input(st, &idev, client, "INV_YAS53X");
	if (result)
		goto out_free_memory;
	st->idev = idev;
#if INPUT_SYSFS    
	result = sysfs_create_group(&st->idev->dev.kobj, &yas53x_attribute_group);
#endif
#if CLASS_SYSFS
    result = create_sysfs_interfaces(st);
#endif
	if (result < 0) {
        dev_err(&client->dev,
			"%s: create sysfs failed.", __func__);
		goto error_sysfs;
    }
    
	result = yas53x_init(st);
	if (result < 0)
		goto error_sysfs;

	return result;
error_sysfs:
	input_unregister_device(st->idev);
out_free_memory:
	kfree(st);
out_no_free:
	dev_err(&client->adapter->dev, "%s failed %d\n", __func__, result);
	return result;

}

static int yas53x_mod_remove(struct i2c_client *client)
{
	struct inv_compass_state *st =
		i2c_get_clientdata(client);

	dev_dbg(&client->adapter->dev, "%s\n", __func__);
	yas53x_set_enable(&st->idev->dev, 0);
#if INPUT_SYSFS    
	sysfs_remove_group(&st->idev->dev.kobj, &yas53x_attribute_group);
#endif
#if CLASS_SYSFS
    remove_sysfs_interfaces(st);
#endif
	input_unregister_device(st->idev);
	kfree(st);
	return 0;
}

static const struct i2c_device_id yas53x_mod_id[] = {
	{ "yas530", ID_YAS530 },
    { "yas532", ID_YAS532 },
	{}
};

MODULE_DEVICE_TABLE(i2c, yas53x_mod_id);

static struct i2c_driver yas53x_mod_driver = {
	.class = I2C_CLASS_HWMON,
	.probe = yas53x_mod_probe,
	.remove = yas53x_mod_remove,
	.id_table = yas53x_mod_id,
	.driver = {
		   .owner = THIS_MODULE,
		   .name = "yas53x_mod",
		   },
	.address_list = normal_i2c,
};

static int __init yas53x_mod_init(void)
{
	int res = i2c_add_driver(&yas53x_mod_driver);
	pr_info("%s: Probe name %s\n", __func__, "yas53x");
	if (res)
		pr_err("%s failed\n", __func__);
	return res;
}

static void __exit yas53x_mod_exit(void)
{
	pr_info("%s\n", __func__);
	i2c_del_driver(&yas53x_mod_driver);
}

module_init(yas53x_mod_init);
module_exit(yas53x_mod_exit);

MODULE_AUTHOR("Invensense Corporation");
MODULE_DESCRIPTION("Driver for YAS53X sensors with input subsystem");
MODULE_LICENSE("GPL");
MODULE_ALIAS("yas53x_mod");

/**
 *  @}
 */
