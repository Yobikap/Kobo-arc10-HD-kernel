#ifndef __LINUX_FTS_TS_H__
#define __LINUX_FTS_TS_H__

/* FTS chip declare */
//#define FT5x06		/* FT5206, FT5306, FT5406 */
//#define FT5x16		/* FT5216, FT5216 */
//#define FT5606		/* FT5506, FT5606, FT5816 */
//#define FT6x06		/* FT6206, FT6306 */

#define WISTRON_EXTENSION
#ifdef WISTRON_EXTENSION
#define CTPM_ID_BIT             6 // bit 6~7 , 01 for OGS, 10 for GG
#define CTPM_ID_MASK			      (0x03 << CTPM_ID_BIT)
#define CTPM_OGS_ID			      (0x01<< CTPM_ID_BIT)
#define CTPM_GG_ID			             (0x02<< CTPM_ID_BIT)
#define CTPM_EDT_ID			        (0x00 << CTPM_ID_BIT)
#define CTPM_JTOUCH_ID			    (0x01 << CTPM_ID_BIT)
#define CTPM_EDT_I2C_ADDR		    (0x70 >> 1)
#define CTPM_JTOUCH_I2C_ADDR		(0x72 >> 1)

//provided by vendor
#define PROC_MAX_TX     38
#define PROC_MAX_RX     24

#define PROBE_1CTPM     0x01
#define PROBE_2CTPM     0x02
#define PROBE_DONE      0x04
#endif

#define FTS_I2C_ADDR            (0x70 >> 1)

#define HW_PWR_ONOFF
#define PULL_RESET 1
#define PULL_RESET_LOW 0
#define PWR_ON 1
#define PWR_OFF 0

/* kernel multi-touch report protocol define */
//#define TOUCH_REPORT_TYPE_A
#define TOUCH_REPORT_TYPE_B

//#define DEBUG_MSG
//#define VIRTUAL_KEY
#define FTS_EXTENSION
//#define FTS_RAWDATA_DEBUGVIEW

#ifdef TOUCH_REPORT_TYPE_B
#include <linux/input/mt.h>
#endif

#ifdef DEBUG_MSG
#define fts_dbg(format, arg...) dev_err(&ts->client->dev, "%s: " format , __func__ , ## arg)
#else
#define fts_dbg(format, arg...) do {} while (0)
#endif

#define fts_msg(format, arg...) dev_err(&ts->client->dev, "%s: " format "\n" , \
	__func__ , ## arg)

#define FILL_I2C_MSG(msg, address, flag, length, buffer)	{\
	msg.addr = address;	\
	msg.flags = flag;	\
	msg.len = length;	\
	msg.buf = (u8 *)buffer;	\
}

/* Panel parameter define */
#define MAX_X	2559
#define MAX_Y	1599

#define FTS_MAX_TOUCH		10
#define FTS_MAX_TOUCH_PRESS	255

#define F_DOWN		0x00
#define F_UP			0x01
#define F_CONTACT	0x02

#define FT5x06_ID		0x55
#define FT5x16_ID		0x0A
#define FT5606_ID		0x08
#define FT6x06_ID		0x06

/*
#ifdef FT5x06
#define FTS_DEV_ID		FT5x06_ID
#elif defined(FT5x16)
#define FTS_DEV_ID		FT5x16_ID
#elif defined(FT5606)
#define FTS_DEV_ID		FT5606_ID
#elif defined(FT6x06)
#define FTS_DEV_ID		FT6X06_ID
#else
#error Sorry, no define FTS chip
#endif
*/

#ifdef FTS_EXTENSION
#define FT5x06_UPGRADE_ID1			      0x79
#define FT5x06_UPGRADE_ID2			      0x03
#define FT5x06_UPGRADE_AA_DELAY		      50
#define FT5x06_UPGRADE_55_DELAY		      30
#define FT5x06_UPGRADE_READID_DELAY      1
#define FT5x06_UPGRADE_EARSE_DELAY	  2000

#define FT5x16_UPGRADE_ID1	      		0x79
#define FT5x16_UPGRADE_ID2			      0x07
#define FT5x16_UPGRADE_AA_DELAY		      50
#define FT5x16_UPGRADE_55_DELAY		      10
#define FT5x16_UPGRADE_READID_DELAY	     1
#define FT5x16_UPGRADE_EARSE_DELAY	  1500

#define FT5606_UPGRADE_ID1			      0x79
#define FT5606_UPGRADE_ID2			      0x06
#define FT5606_UPGRADE_AA_DELAY		      50
#define FT5606_UPGRADE_55_DELAY		      10
#define FT5606_UPGRADE_READID_DELAY	   100
#define FT5606_UPGRADE_EARSE_DELAY	  2000

#define FT6x06_UPGRADE_ID1			      0x79
#define FT6x06_UPGRADE_ID2			      0x05
#define FT6x06_UPGRADE_AA_DELAY		      60
#define FT6x06_UPGRADE_55_DELAY		      10
#define FT6x06_UPGRADE_READID_DELAY	    10
#define FT6x06_UPGRADE_EARSE_DELAY	  2000
#endif

#define REG_DEV_MODE		0x00
#define REG_RAWDATA_ROW	0x01
#define REG_RAWDATA		  0x10
#define REG_PWR_MODE		0xA5
#define REG_FW_VER			0xA6

typedef	enum{
	P_ACTIVE,
	P_MONITOR,
	P_STANDBY,
	P_HIBERNATE,
}PWR_MODE;

#define FTS_MODE_NORMAL	0x00
#define FTS_MODE_SYS		0x10
#define FTS_MODE_TEST		0x40

#define DRIVER_VERSION_OGS		"1.03"
#define DRIVER_VERSION_GG		"2.02"
#define DRIVER_DESC			  "FTS TouchScreen driver"
#define DRIVER_NAME			  "fts_ts"//"fts"
//#define DRIVER_DEV_NAME	"fts_ts"
#define FTS_NAME          DRIVER_NAME

#define POINTER_TOUCH	BIT(0)
#define KEY_TOUCH		BIT(1)


/* The platform data for the Focaltech touchscreen driver */
struct fts_platform_data {
	unsigned long irqflags;
	int irq;
	//unsigned int reset;
  int	(*hw_power)(struct device *, int);
  int (*hw_reset)(int);
};

#endif
