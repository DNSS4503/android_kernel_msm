/*
	$License:
	Copyright (C) 2011 InvenSense Corporation, All Rights Reserved.

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License version 2 as
	published by the Free Software Foundation.

	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with this program.  If not, see <http://www.gnu.org/licenses/>.
	$
 */

/**
 *  @addtogroup ACCELDL
 *  @brief      Provides the interface to setup and handle an accelerometer.
 *
 *  @{
 *      @file   lsm303dlx_a.c
 *      @brief  Accelerometer setup and handling methods for ST LSM303DLH
 *              or LSM303DLM accel.
 */

/* -------------------------------------------------------------------------- */

#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include "mpu-dev.h"

#include <log.h>
#include <linux/mpu.h>
#include "mlsl.h"
#include "mldl_cfg.h"
#undef MPL_LOG_TAG
#define MPL_LOG_TAG "MPL-acc"

/* -------------------------------------------------------------------------- */

/* full scale setting - register & mask */
#define LSM303DLx_CTRL_REG1         (0x20)
#define LSM303DLx_CTRL_REG2         (0x21)
#define LSM303DLx_CTRL_REG3         (0x22)
#define LSM303DLx_CTRL_REG4         (0x23)
#define LSM303DLx_CTRL_REG5         (0x24)
#define LSM303DLx_HP_FILTER_RESET   (0x25)
#define LSM303DLx_REFERENCE         (0x26)
#define LSM303DLx_STATUS_REG        (0x27)
#define LSM303DLx_OUT_X_L           (0x28)
#define LSM303DLx_OUT_X_H           (0x29)
#define LSM303DLx_OUT_Y_L           (0x2a)
#define LSM303DLx_OUT_Y_H           (0x2b)
#define LSM303DLx_OUT_Z_L           (0x2b)
#define LSM303DLx_OUT_Z_H           (0x2d)

#define LSM303DLx_INT1_CFG          (0x30)
#define LSM303DLx_INT1_SRC          (0x31)
#define LSM303DLx_INT1_THS          (0x32)
#define LSM303DLx_INT1_DURATION     (0x33)

#define LSM303DLx_INT2_CFG          (0x34)
#define LSM303DLx_INT2_SRC          (0x35)
#define LSM303DLx_INT2_THS          (0x36)
#define LSM303DLx_INT2_DURATION     (0x37)

#define LSM303DLx_CTRL_MASK         (0x30)
#define LSM303DLx_SLEEP_MASK        (0x20)
#define LSM303DLx_PWR_MODE_NORMAL   (0x20)

#define LSM303DLx_MAX_DUR           (0x7F)

/* -------------------------------------------------------------------------- */

struct lsm303dlx_a_config {
	unsigned int odr;
	unsigned int fsr; /** < full scale range mg */
	unsigned int ths; /** < Motion no-motion thseshold mg */
	unsigned int dur; /** < Motion no-motion duration ms */
	unsigned char reg_ths;
	unsigned char reg_dur;
	unsigned char ctrl_reg1;
	unsigned char irq_type;
	unsigned char mot_int1_cfg;
};

struct lsm303dlx_a_private_data {
	struct lsm303dlx_a_config suspend;
	struct lsm303dlx_a_config resume;
};

/* -------------------------------------------------------------------------- */

static int lsm303dlx_a_set_ths(void *mlsl_handle,
			       struct ext_slave_platform_data *pdata,
			       struct lsm303dlx_a_config *config,
			       int apply,
			       long ths)
{
	int result = INV_SUCCESS;
	if ((unsigned int) ths >= config->fsr)
		ths = (long) config->fsr - 1;

	if (ths < 0)
		ths = 0;

	config->ths = ths;
	config->reg_ths = (unsigned char)(long)((ths * 128L) / (config->fsr));
	MPL_LOGV("THS: %d, 0x%02x\n", config->ths, (int)config->reg_ths);
	if (apply)
		result = inv_serial_single_write(mlsl_handle, pdata->address,
					LSM303DLx_INT1_THS,
					config->reg_ths);
	return result;
}

static int lsm303dlx_a_set_dur(void *mlsl_handle,
			       struct ext_slave_platform_data *pdata,
			       struct lsm303dlx_a_config *config,
			       int apply,
			       long dur)
{
	int result = INV_SUCCESS;
	long reg_dur = (dur * config->odr) / 1000000L;
	config->dur = dur;

	if (reg_dur > LSM303DLx_MAX_DUR)
		reg_dur = LSM303DLx_MAX_DUR;

	config->reg_dur = (unsigned char) reg_dur;
	MPL_LOGV("DUR: %d, 0x%02x\n", config->dur, (int)config->reg_dur);
	if (apply)
		result = inv_serial_single_write(mlsl_handle, pdata->address,
					LSM303DLx_INT1_DURATION,
					(unsigned char)reg_dur);
	return result;
}

/**
 * Sets the IRQ to fire when one of the IRQ events occur.  Threshold and
 * duration will not be used uless the type is MOT or NMOT.
 *
 * @param config configuration to apply to, suspend or resume
 * @param irq_type The type of IRQ.  Valid values are
 * - MPU_SLAVE_IRQ_TYPE_NONE
 * - MPU_SLAVE_IRQ_TYPE_MOTION
 * - MPU_SLAVE_IRQ_TYPE_DATA_READY
 */
static int lsm303dlx_a_set_irq(void *mlsl_handle,
			       struct ext_slave_platform_data *pdata,
			       struct lsm303dlx_a_config *config,
			       int apply,
			       long irq_type)
{
	int result = INV_SUCCESS;
	unsigned char reg1;
	unsigned char reg2;

	config->irq_type = (unsigned char)irq_type;
	if (irq_type == MPU_SLAVE_IRQ_TYPE_DATA_READY) {
		reg1 = 0x02;
		reg2 = 0x00;
	} else if (irq_type == MPU_SLAVE_IRQ_TYPE_MOTION) {
		reg1 = 0x00;
		reg2 = config->mot_int1_cfg;
	} else {
		reg1 = 0x00;
		reg2 = 0x00;
	}

	if (apply) {
		result = inv_serial_single_write(mlsl_handle, pdata->address,
					LSM303DLx_CTRL_REG3, reg1);
		result = inv_serial_single_write(mlsl_handle, pdata->address,
					LSM303DLx_INT1_CFG, reg2);
	}

	return result;
}

/**
 *  @brief Set the output data rate for the particular configuration.
 *
 *  @param mlsl_handle
 *             the handle to the serial channel the device is connected to.
 *  @param pdata
 *             a pointer to the slave platform data.
 *  @param config
 *             Config to modify with new ODR.
 *  @param apply
 *             whether to apply immediately or save the settings to be applied
 *             at the next resume.
 *  @param odr
 *             Output data rate in units of 1/1000Hz (mHz).
 *
 *  @return INV_SUCCESS if successful or a non-zero error code.
 */
static int lsm303dlx_a_set_odr(void *mlsl_handle,
			       struct ext_slave_platform_data *pdata,
			       struct lsm303dlx_a_config *config,
			       int apply,
			       long odr)
{
	unsigned char bits;
	int result = INV_SUCCESS;

	/* normal power modes */
	if (odr > 400000) {
		config->odr = 1000000;
		bits = LSM303DLx_PWR_MODE_NORMAL | 0x18;
	} else if (odr > 100000) {
		config->odr = 400000;
		bits = LSM303DLx_PWR_MODE_NORMAL | 0x10;
	} else if (odr > 50000) {
		config->odr = 100000;
		bits = LSM303DLx_PWR_MODE_NORMAL | 0x08;
	} else if (odr > 10000) {
		config->odr = 50000;
		bits = LSM303DLx_PWR_MODE_NORMAL | 0x00;
	/* low power modes */
	} else if (odr > 5000) {
		config->odr = 10000;
		bits = 0xC0;
	} else if (odr > 2000) {
		config->odr = 5000;
		bits = 0xA0;
	} else if (odr > 1000) {
		config->odr = 2000;
		bits = 0x80;
	} else if (odr > 500) {
		config->odr = 1000;
		bits = 0x60;
	} else if (odr > 0) {
		config->odr = 500;
		bits = 0x40;
	} else {
		config->odr = 0;
		bits = 0;
	}

	config->ctrl_reg1 = bits | (config->ctrl_reg1 & 0x7);
	lsm303dlx_a_set_dur(mlsl_handle, pdata, config, apply, config->dur);
	MPL_LOGV("ODR: %d, 0x%02x\n", config->odr, (int)config->ctrl_reg1);
	if (apply)
		result = inv_serial_single_write(mlsl_handle, pdata->address,
					LSM303DLx_CTRL_REG1,
					config->ctrl_reg1);
	return result;
}

/**
 *  @brief Set the full scale range of the accels
 *
 *  @param mlsl_handle
 *             the handle to the serial channel the device is connected to.
 *  @param pdata
 *             a pointer to the slave platform data.
 *  @param config
 *             pointer to configuration.
 *  @param apply
 *             whether to apply immediately or save the settings to be applied
 *             at the next resume.
 *  @param fsr
 *             requested full scale range.
 *
 *  @return INV_SUCCESS if successful or a non-zero error code.
 */
static int lsm303dlx_a_set_fsr(void *mlsl_handle,
			       struct ext_slave_platform_data *pdata,
			       struct lsm303dlx_a_config *config,
			       int apply,
			       long fsr)
{
	unsigned char reg1 = 0x40;
	int result = INV_SUCCESS;

	if (fsr <= 2048) {
		config->fsr = 2048;
	} else if (fsr <= 4096) {
		reg1 |= 0x30;
		config->fsr = 4096;
	} else {
		reg1 |= 0x10;
		config->fsr = 8192;
	}

	lsm303dlx_a_set_ths(mlsl_handle, pdata,
			config, apply, config->ths);
	MPL_LOGV("FSR: %d\n", config->fsr);
	if (apply)
		result = inv_serial_single_write(mlsl_handle, pdata->address,
					LSM303DLx_CTRL_REG4, reg1);

	return result;
}

/**
 *  @brief suspends the device to put it in its lowest power mode.
 *
 *  @param mlsl_handle
 *             the handle to the serial channel the device is connected to.
 *  @param slave
 *             a pointer to the slave descriptor data structure.
 *  @param pdata
 *             a pointer to the slave platform data.
 *
 *  @return INV_SUCCESS if successful or a non-zero error code.
 */
static int lsm303dlx_a_suspend(void *mlsl_handle,
			       struct ext_slave_descr *slave,
			       struct ext_slave_platform_data *pdata)
{
	int result = INV_SUCCESS;
	unsigned char reg1;
	unsigned char reg2;
	struct lsm303dlx_a_private_data *private_data =
		(struct lsm303dlx_a_private_data *)(pdata->private_data);

	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_CTRL_REG1,
				       private_data->suspend.ctrl_reg1);

	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_CTRL_REG2, 0x0f);
	reg1 = 0x40;
	if (private_data->suspend.fsr == 8192)
		reg1 |= 0x30;
	else if (private_data->suspend.fsr == 4096)
		reg1 |= 0x10;
	/* else bits [4..5] are already zero */

	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_CTRL_REG4, reg1);
	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_INT1_THS,
				       private_data->suspend.reg_ths);
	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_INT1_DURATION,
				       private_data->suspend.reg_dur);

	if (private_data->suspend.irq_type == MPU_SLAVE_IRQ_TYPE_DATA_READY) {
		reg1 = 0x02;
		reg2 = 0x00;
	} else if (private_data->suspend.irq_type ==
		   MPU_SLAVE_IRQ_TYPE_MOTION) {
		reg1 = 0x00;
		reg2 = private_data->suspend.mot_int1_cfg;
	} else {
		reg1 = 0x00;
		reg2 = 0x00;
	}
	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_CTRL_REG3, reg1);
	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_INT1_CFG, reg2);
	result = inv_serial_read(mlsl_handle, pdata->address,
				LSM303DLx_HP_FILTER_RESET, 1, &reg1);
	return result;
}

/**
 *  @brief resume the device in the proper power state given the configuration
 *         chosen.
 *
 *  @param mlsl_handle
 *             the handle to the serial channel the device is connected to.
 *  @param slave
 *             a pointer to the slave descriptor data structure.
 *  @param pdata
 *             a pointer to the slave platform data.
 *
 *  @return INV_SUCCESS if successful or a non-zero error code.
 */
static int lsm303dlx_a_resume(void *mlsl_handle,
			      struct ext_slave_descr *slave,
			      struct ext_slave_platform_data *pdata)
{
	int result = INV_SUCCESS;
	unsigned char reg1;
	unsigned char reg2;
	struct lsm303dlx_a_private_data *private_data =
		(struct lsm303dlx_a_private_data *)(pdata->private_data);


	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_CTRL_REG1,
				       private_data->resume.ctrl_reg1);
	if (result) {
		LOG_RESULT_LOCATION(result);
		return result;
	}
	msleep(6);

	/* Full Scale */
	reg1 = 0x40;
	if (private_data->resume.fsr == 8192)
		reg1 |= 0x30;
	else if (private_data->resume.fsr == 4096)
		reg1 |= 0x10;

	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_CTRL_REG4, reg1);
	if (result) {
		LOG_RESULT_LOCATION(result);
		return result;
	}

	/* Configure high pass filter */
	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_CTRL_REG2, 0x0F);
	if (result) {
		LOG_RESULT_LOCATION(result);
		return result;
	}

	if (private_data->resume.irq_type == MPU_SLAVE_IRQ_TYPE_DATA_READY) {
		reg1 = 0x02;
		reg2 = 0x00;
	} else if (private_data->resume.irq_type ==
		   MPU_SLAVE_IRQ_TYPE_MOTION) {
		reg1 = 0x00;
		reg2 = private_data->resume.mot_int1_cfg;
	} else {
		reg1 = 0x00;
		reg2 = 0x00;
	}
	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_CTRL_REG3, reg1);
	if (result) {
		LOG_RESULT_LOCATION(result);
		return result;
	}
	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_INT1_THS,
				       private_data->resume.reg_ths);
	if (result) {
		LOG_RESULT_LOCATION(result);
		return result;
	}
	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_INT1_DURATION,
				       private_data->resume.reg_dur);
	if (result) {
		LOG_RESULT_LOCATION(result);
		return result;
	}
	result = inv_serial_single_write(mlsl_handle, pdata->address,
				       LSM303DLx_INT1_CFG, reg2);
	if (result) {
		LOG_RESULT_LOCATION(result);
		return result;
	}
	result = inv_serial_read(mlsl_handle, pdata->address,
				LSM303DLx_HP_FILTER_RESET, 1, &reg1);
	if (result) {
		LOG_RESULT_LOCATION(result);
		return result;
	}
	return result;
}

/**
 *  @brief read the sensor data from the device.
 *
 *  @param mlsl_handle
 *             the handle to the serial channel the device is connected to.
 *  @param slave
 *             a pointer to the slave descriptor data structure.
 *  @param pdata
 *             a pointer to the slave platform data.
 *  @param data
 *             a buffer to store the data read.
 *
 *  @return INV_SUCCESS if successful or a non-zero error code.
 */
static int lsm303dlx_a_read(void *mlsl_handle,
			    struct ext_slave_descr *slave,
			    struct ext_slave_platform_data *pdata,
			    unsigned char *data)
{
	int result = INV_SUCCESS;
	result = inv_serial_read(mlsl_handle, pdata->address,
				LSM303DLx_STATUS_REG, 1, data);
	if (data[0] & 0x0F) {
		result = inv_serial_read(mlsl_handle, pdata->address,
					slave->read_reg, slave->read_len, data);
	return result;
	} else
		return INV_ERROR_ACCEL_DATA_NOT_READY;
}

/**
 *  @brief one-time device driver initialization function.
 *         If the driver is built as a kernel module, this function will be
 *         called when the module is loaded in the kernel.
 *         If the driver is built-in in the kernel, this function will be
 *         called at boot time.
 *
 *  @param mlsl_handle
 *             the handle to the serial channel the device is connected to.
 *  @param slave
 *             a pointer to the slave descriptor data structure.
 *  @param pdata
 *             a pointer to the slave platform data.
 *
 *  @return INV_SUCCESS if successful or a non-zero error code.
 */
static int lsm303dlx_a_init(void *mlsl_handle,
			    struct ext_slave_descr *slave,
			    struct ext_slave_platform_data *pdata)
{
	long range;
	struct lsm303dlx_a_private_data *private_data;
	private_data = (struct lsm303dlx_a_private_data *)
	    kzalloc(sizeof(struct lsm303dlx_a_private_data), GFP_KERNEL);

	if (!private_data)
		return INV_ERROR_MEMORY_EXAUSTED;

	pdata->private_data = private_data;

	private_data->resume.ctrl_reg1 = 0x37;
	private_data->suspend.ctrl_reg1 = 0x47;
	private_data->resume.mot_int1_cfg = 0x95;
	private_data->suspend.mot_int1_cfg = 0x2a;

	lsm303dlx_a_set_odr(mlsl_handle, pdata, &private_data->suspend,
			false, 0);
	lsm303dlx_a_set_odr(mlsl_handle, pdata, &private_data->resume,
			false, 200000);

	range = range_fixedpoint_to_long_mg(slave->range);
	lsm303dlx_a_set_fsr(mlsl_handle, pdata, &private_data->suspend,
			false, range);
	lsm303dlx_a_set_fsr(mlsl_handle, pdata, &private_data->resume,
			false, range);

	lsm303dlx_a_set_ths(mlsl_handle, pdata, &private_data->suspend,
			false, 80);
	lsm303dlx_a_set_ths(mlsl_handle, pdata, &private_data->resume,
			false, 40);

	lsm303dlx_a_set_dur(mlsl_handle, pdata, &private_data->suspend,
			false, 1000);
	lsm303dlx_a_set_dur(mlsl_handle, pdata, &private_data->resume,
			false, 2540);

	lsm303dlx_a_set_irq(mlsl_handle, pdata, &private_data->suspend,
			false, MPU_SLAVE_IRQ_TYPE_NONE);
	lsm303dlx_a_set_irq(mlsl_handle, pdata, &private_data->resume,
			false, MPU_SLAVE_IRQ_TYPE_NONE);
	return INV_SUCCESS;
}

/**
 *  @brief one-time device driver exit function.
 *         If the driver is built as a kernel module, this function will be
 *         called when the module is removed from the kernel.
 *
 *  @param mlsl_handle
 *             the handle to the serial channel the device is connected to.
 *  @param slave
 *             a pointer to the slave descriptor data structure.
 *  @param pdata
 *             a pointer to the slave platform data.
 *
 *  @return INV_SUCCESS if successful or a non-zero error code.
 */
static int lsm303dlx_a_exit(void *mlsl_handle,
			    struct ext_slave_descr *slave,
			    struct ext_slave_platform_data *pdata)
{
	kfree(pdata->private_data);
	return INV_SUCCESS;
}

/**
 *  @brief device configuration facility.
 *
 *  @param mlsl_handle
 *             the handle to the serial channel the device is connected to.
 *  @param slave
 *             a pointer to the slave descriptor data structure.
 *  @param pdata
 *             a pointer to the slave platform data.
 *  @param data
 *             a pointer to the configuration data structure.
 *
 *  @return INV_SUCCESS if successful or a non-zero error code.
 */
static int lsm303dlx_a_config(void *mlsl_handle,
			      struct ext_slave_descr *slave,
			      struct ext_slave_platform_data *pdata,
			      struct ext_slave_config *data)
{
	struct lsm303dlx_a_private_data *private_data = pdata->private_data;
	if (!data->data)
		return INV_ERROR_INVALID_PARAMETER;

	switch (data->key) {
	case MPU_SLAVE_CONFIG_ODR_SUSPEND:
		return lsm303dlx_a_set_odr(mlsl_handle, pdata,
					&private_data->suspend,
					data->apply,
					*((long *)data->data));
	case MPU_SLAVE_CONFIG_ODR_RESUME:
		return lsm303dlx_a_set_odr(mlsl_handle, pdata,
					&private_data->resume,
					data->apply,
					*((long *)data->data));
	case MPU_SLAVE_CONFIG_FSR_SUSPEND:
		return lsm303dlx_a_set_fsr(mlsl_handle, pdata,
					&private_data->suspend,
					data->apply,
					*((long *)data->data));
	case MPU_SLAVE_CONFIG_FSR_RESUME:
		return lsm303dlx_a_set_fsr(mlsl_handle, pdata,
					&private_data->resume,
					data->apply,
					*((long *)data->data));
	case MPU_SLAVE_CONFIG_MOT_THS:
		return lsm303dlx_a_set_ths(mlsl_handle, pdata,
					&private_data->suspend,
					data->apply,
					*((long *)data->data));
	case MPU_SLAVE_CONFIG_NMOT_THS:
		return lsm303dlx_a_set_ths(mlsl_handle, pdata,
					&private_data->resume,
					data->apply,
					*((long *)data->data));
	case MPU_SLAVE_CONFIG_MOT_DUR:
		return lsm303dlx_a_set_dur(mlsl_handle, pdata,
					&private_data->suspend,
					data->apply,
					*((long *)data->data));
	case MPU_SLAVE_CONFIG_NMOT_DUR:
		return lsm303dlx_a_set_dur(mlsl_handle, pdata,
					&private_data->resume,
					data->apply,
					*((long *)data->data));
	case MPU_SLAVE_CONFIG_IRQ_SUSPEND:
		return lsm303dlx_a_set_irq(mlsl_handle, pdata,
					&private_data->suspend,
					data->apply,
					*((long *)data->data));
	case MPU_SLAVE_CONFIG_IRQ_RESUME:
		return lsm303dlx_a_set_irq(mlsl_handle, pdata,
					&private_data->resume,
					data->apply,
					*((long *)data->data));
	default:
		LOG_RESULT_LOCATION(INV_ERROR_FEATURE_NOT_IMPLEMENTED);
		return INV_ERROR_FEATURE_NOT_IMPLEMENTED;
	};

	return INV_SUCCESS;
}

/**
 *  @brief facility to retrieve the device configuration.
 *
 *  @param mlsl_handle
 *             the handle to the serial channel the device is connected to.
 *  @param slave
 *             a pointer to the slave descriptor data structure.
 *  @param pdata
 *             a pointer to the slave platform data.
 *  @param data
 *             a pointer to store the returned configuration data structure.
 *
 *  @return INV_SUCCESS if successful or a non-zero error code.
 */
static int lsm303dlx_a_get_config(void *mlsl_handle,
				  struct ext_slave_descr *slave,
				  struct ext_slave_platform_data *pdata,
				  struct ext_slave_config *data)
{
	struct lsm303dlx_a_private_data *private_data = pdata->private_data;
	if (!data->data)
		return INV_ERROR_INVALID_PARAMETER;

	switch (data->key) {
	case MPU_SLAVE_CONFIG_ODR_SUSPEND:
		(*(unsigned long *)data->data) =
			(unsigned long) private_data->suspend.odr;
		break;
	case MPU_SLAVE_CONFIG_ODR_RESUME:
		(*(unsigned long *)data->data) =
			(unsigned long) private_data->resume.odr;
		break;
	case MPU_SLAVE_CONFIG_FSR_SUSPEND:
		(*(unsigned long *)data->data) =
			(unsigned long) private_data->suspend.fsr;
		break;
	case MPU_SLAVE_CONFIG_FSR_RESUME:
		(*(unsigned long *)data->data) =
			(unsigned long) private_data->resume.fsr;
		break;
	case MPU_SLAVE_CONFIG_MOT_THS:
		(*(unsigned long *)data->data) =
			(unsigned long) private_data->suspend.ths;
		break;
	case MPU_SLAVE_CONFIG_NMOT_THS:
		(*(unsigned long *)data->data) =
			(unsigned long) private_data->resume.ths;
		break;
	case MPU_SLAVE_CONFIG_MOT_DUR:
		(*(unsigned long *)data->data) =
			(unsigned long) private_data->suspend.dur;
		break;
	case MPU_SLAVE_CONFIG_NMOT_DUR:
		(*(unsigned long *)data->data) =
			(unsigned long) private_data->resume.dur;
		break;
	case MPU_SLAVE_CONFIG_IRQ_SUSPEND:
		(*(unsigned long *)data->data) =
			(unsigned long) private_data->suspend.irq_type;
		break;
	case MPU_SLAVE_CONFIG_IRQ_RESUME:
		(*(unsigned long *)data->data) =
			(unsigned long) private_data->resume.irq_type;
		break;
	default:
		LOG_RESULT_LOCATION(INV_ERROR_FEATURE_NOT_IMPLEMENTED);
		return INV_ERROR_FEATURE_NOT_IMPLEMENTED;
	};

	return INV_SUCCESS;
}

static struct ext_slave_descr lsm303dlx_a_descr = {
	.init             = lsm303dlx_a_init,
	.exit             = lsm303dlx_a_exit,
	.suspend          = lsm303dlx_a_suspend,
	.resume           = lsm303dlx_a_resume,
	.read             = lsm303dlx_a_read,
	.config           = lsm303dlx_a_config,
	.get_config       = lsm303dlx_a_get_config,
	.name             = "lsm303dlx_a",
	.type             = EXT_SLAVE_TYPE_ACCEL,
	.id               = ACCEL_ID_LSM303DLX,
	.read_reg         = (0x28 | 0x80), /* 0x80 for burst reads */
	.read_len         = 6,
	.endian           = EXT_SLAVE_BIG_ENDIAN,
	.range            = {2, 480},
	.trigger          = NULL,
};

static
struct ext_slave_descr *lsm303dlx_a_get_slave_descr(void)
{
	return &lsm303dlx_a_descr;
}

/* -------------------------------------------------------------------------- */
struct lsm303dlx_a_mod_private_data {
	struct i2c_client *client;
	struct ext_slave_platform_data *pdata;
};

static unsigned short normal_i2c[] = { I2C_CLIENT_END };

static int lsm303dlx_a_mod_probe(struct i2c_client *client,
			   const struct i2c_device_id *devid)
{
	struct ext_slave_platform_data *pdata;
	struct lsm303dlx_a_mod_private_data *private_data;
	int result = 0;

	dev_info(&client->adapter->dev, "%s: %s\n", __func__, devid->name);

	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		result = -ENODEV;
		goto out_no_free;
	}

	pdata = client->dev.platform_data;
	if (!pdata) {
		dev_err(&client->adapter->dev,
			"Missing platform data for slave %s\n", devid->name);
		result = -EFAULT;
		goto out_no_free;
	}

	private_data = kzalloc(sizeof(*private_data), GFP_KERNEL);
	if (!private_data) {
		result = -ENOMEM;
		goto out_no_free;
	}

	i2c_set_clientdata(client, private_data);
	private_data->client = client;
	private_data->pdata = pdata;

	result = inv_mpu_register_slave(THIS_MODULE, client, pdata,
					lsm303dlx_a_get_slave_descr);
	if (result) {
		dev_err(&client->adapter->dev,
			"Slave registration failed: %s, %d\n",
			devid->name, result);
		goto out_free_memory;
	}

	return result;

out_free_memory:
	kfree(private_data);
out_no_free:
	dev_err(&client->adapter->dev, "%s failed %d\n", __func__, result);
	return result;

}

static int lsm303dlx_a_mod_remove(struct i2c_client *client)
{
	struct lsm303dlx_a_mod_private_data *private_data =
		i2c_get_clientdata(client);

	dev_dbg(&client->adapter->dev, "%s\n", __func__);

	inv_mpu_unregister_slave(client, private_data->pdata,
				lsm303dlx_a_get_slave_descr);

	kfree(private_data);
	return 0;
}

static const struct i2c_device_id lsm303dlx_a_mod_id[] = {
	{ "lsm303dlx", ACCEL_ID_LSM303DLX },
	{}
};

MODULE_DEVICE_TABLE(i2c, lsm303dlx_a_mod_id);

static struct i2c_driver lsm303dlx_a_mod_driver = {
	.class = I2C_CLASS_HWMON,
	.probe = lsm303dlx_a_mod_probe,
	.remove = lsm303dlx_a_mod_remove,
	.id_table = lsm303dlx_a_mod_id,
	.driver = {
		   .owner = THIS_MODULE,
		   .name = "lsm303dlx_a_mod",
		   },
	.address_list = normal_i2c,
};

static int __init lsm303dlx_a_mod_init(void)
{
	int res = i2c_add_driver(&lsm303dlx_a_mod_driver);
	pr_info("%s: Probe name %s\n", __func__, "lsm303dlx_a_mod");
	if (res)
		pr_err("%s failed\n", __func__);
	return res;
}

static void __exit lsm303dlx_a_mod_exit(void)
{
	pr_info("%s\n", __func__);
	i2c_del_driver(&lsm303dlx_a_mod_driver);
}

module_init(lsm303dlx_a_mod_init);
module_exit(lsm303dlx_a_mod_exit);

MODULE_AUTHOR("Invensense Corporation");
MODULE_DESCRIPTION("Driver to integrate LSM303DLX_A sensor with the MPU");
MODULE_LICENSE("GPL");
MODULE_ALIAS("lsm303dlx_a_mod");

/**
 *  @}
 */
