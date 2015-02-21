/* include/linux/cm3232.h
 *
 * Copyright (C) 2011 Capella Microsystems Inc.
 * Author: Frank Hsieh <pengyueh@gmail.com>  
 *
 * Copyright (C) 2013, Wistron Corporation.
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

#ifndef __LINUX_CM3232_H
#define __LINUX_CM3232_H

#define CM3232_I2C_NAME "cm3232"

#define	CM3232_SLAVE_addr	0x10

#define ALS_CALIBRATED		0x6EE3

/*cm3232*/
#define CM3232_ALS_COMMAND_CODE 	       0
#define CM3232_ALS_READ_COMMAND_CODE 	0x50

/*for ALS command*/
#define CM3232_ALS_RESET 	    (1 << 6)
#define CM3232_ALS_IT_100ms 	(0 << 2)
#define CM3232_ALS_IT_200ms 	(1 << 2)
#define CM3232_ALS_IT_400ms 	(2 << 2)
#define CM3232_ALS_IT_800ms 	(3 << 2)
#define CM3232_ALS_IT_1600ms 	(4 << 2)
#define CM3232_ALS_IT_3200ms 	(5 << 2)

#define CM3232_ALS_HS_HIGH 	  (1 << 1)
#define CM3232_ALS_SD		      (1 << 0)

#define LS_PWR_ON					(1 << 0)
#define PS_PWR_ON					(1 << 1)

#endif
