/*
 *
 * TWL4030 MADC module driver-This driver monitors the real time
 * conversion of analog signals like battery temperature,
 * battery type, battery level etc. User can also ask for the conversion on a
 * particular channel using the sysfs nodes.
 *
 * Copyright (C) 2010 Texas Instruments Incorporated - http://www.ti.com/
 * J Keerthy <j-keerthy@xxxxxx>
 *
 * Based on twl4030-madc.c
 * Copyright (C) 2008 Nokia Corporation
 * Mikko Ylinen <mikko.k.ylinen@xxxxxxxxx>
 *
 * Amit Kucheria <amit.kucheria@xxxxxxxxxxxxx>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/i2c/twl.h>
#include <linux/i2c/twl4030-madc.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>

/*
 * struct twl4030_madc_data - a container for madc info
 * @hwmon_dev - pointer to device structure for madc
 * @lock - mutex protecting this data structire
 * @requests - Array of request struct corresponding to SW1, SW2 and RT
 * @imr - Interrupt mask register of MADC
 * @isr - Interrupt status register of MADC
 */
struct twl4030_madc_data {
	struct device *hwmon_dev;
	struct mutex lock;/* mutex protecting this data structire */
	struct twl4030_madc_request requests[TWL4030_MADC_NUM_METHODS];
	int imr;
	int isr;
};

static struct twl4030_madc_data *twl4030_madc;

/*
 * sysfs hook function
 */
static ssize_t madc_read(struct device *dev,
			 struct device_attribute *devattr, char *buf)
{
	struct sensor_device_attribute *attr = to_sensor_dev_attr(devattr);
	struct twl4030_madc_request req;
	long val;

	req.channels = (1 << attr->index);
	req.method = TWL4030_MADC_SW2;
	req.func_cb = NULL;
	val = twl4030_madc_conversion(&req);
	if (val >= 0)
		val = req.rbuf[attr->index];
	else
		return val;
	return sprintf(buf, "%ld\n", val);
}

/*
 * Structure containing the registers
 * of different conversion methods supported by MADC.
 * Hardware or RT real time conversion request intiated by external host
 * processor for RT Signal conversions.
 * External host processors can also request for non RT converions
 * SW1 and SW2 software conversions also called asynchronous or GPC request.
 */
static
const struct twl4030_madc_conversion_method twl4030_conversion_methods[] = {
	[TWL4030_MADC_RT] = {
			     .sel = TWL4030_MADC_RTSELECT_LSB,
			     .avg = TWL4030_MADC_RTAVERAGE_LSB,
			     .rbase = TWL4030_MADC_RTCH0_LSB,
			     },
	[TWL4030_MADC_SW1] = {
			      .sel = TWL4030_MADC_SW1SELECT_LSB,
			      .avg = TWL4030_MADC_SW1AVERAGE_LSB,
			      .rbase = TWL4030_MADC_GPCH0_LSB,
			      .ctrl = TWL4030_MADC_CTRL_SW1,
			      },
	[TWL4030_MADC_SW2] = {
			      .sel = TWL4030_MADC_SW2SELECT_LSB,
			      .avg = TWL4030_MADC_SW2AVERAGE_LSB,
			      .rbase = TWL4030_MADC_GPCH0_LSB,
			      .ctrl = TWL4030_MADC_CTRL_SW2,
			      },
};

/*
 * Function to read a particular channel value.
 * @madc - pointer to struct twl4030_madc_data struct
 * @reg - lsb of ADC Channel
 * If the i2c read fails it returns an error else returns 0.
 */
static int twl4030_madc_channel_raw_read(struct twl4030_madc_data *madc, u8 reg)
{
	u8 msb, lsb;
	int ret;
	/*
	 * For each ADC channel, we have MSB and LSB register pair. MSB address
	 * is always LSB address+1. reg parameter is the addr of LSB register
	 */
	ret = twl_i2c_read_u8(TWL4030_MODULE_MADC, &msb, reg + 1);
	if (ret) {
		dev_dbg(madc->hwmon_dev, "unable to read MSB register 0x%X\n",
			reg + 1);
		return ret;
	}
	ret = twl_i2c_read_u8(TWL4030_MODULE_MADC, &lsb, reg);
	if (ret) {
		dev_dbg(madc->hwmon_dev,
		"unable to read LSB register 0x%X\n", reg);
		return ret;
	}

	return (int)(((msb << 8) | lsb) >> 6);
}

/*
 * Function to read channel values
 * @madc - pointer to twl4030_madc_data struct
 * @reg_base - Base address of the first channel
 * @Channel - 16 bit bitmap. If the bit is set channel value is read
 * @buf - The channel values are stored here. if read fails error
 * value is stored
 * Returns the number of successfully read channels.
 */
static int twl4030_madc_read_channels(struct twl4030_madc_data *madc,
				      u8 reg_base, u16 channels, int *buf)
{
	int count = 0, count_req = 0;
	u8 reg, i;

	for (i = 0; i < TWL4030_MADC_MAX_CHANNELS; i++) {
		if (channels & (1 << i)) {
			reg = reg_base + 2 * i;
			buf[i] = twl4030_madc_channel_raw_read(madc, reg);
			if (buf[i] < 0) {
				dev_dbg(madc->hwmon_dev,
					"Unable to read register 0x%X\n",
					reg);
				count_req++;
			} else
				count++;

		}
	}
	if (count_req) {
		dev_dbg(madc->hwmon_dev,
			"%d channel conversion failed\n", count_req);
	}

	return count;
}

/*
 * Enables irq.
 * @madc - pointer to twl4030_madc_data struct
 * @id - irq number to be enabled
 * can take one of TWL4030_MADC_RT, TWL4030_MADC_SW1, TWL4030_MADC_SW2
 * corresponding to RT, SW1, SW2 conversion requests.
 * If the i2c read fails it returns an error else returns 0.
 */
static int twl4030_madc_enable_irq(struct twl4030_madc_data *madc, int id)
{
	u8 val;
	int ret;
	ret = twl_i2c_read_u8(TWL4030_MODULE_MADC, &val, madc->imr);
	if (ret) {
		dev_dbg(madc->hwmon_dev, "unable to read imr register 0x%X\n",
			madc->imr);
		return ret;
	}
	val &= ~(1 << id);
	ret = twl_i2c_write_u8(TWL4030_MODULE_MADC, val, madc->imr);
	if (ret) {
		dev_err(madc->hwmon_dev,
			"unable to write imr register 0x%X\n", madc->imr);
		return ret;

	}

	return 0;
}

/*
 * Disables irq.
 * @madc - pointer to twl4030_madc_data struct
 * @id - irq number to be disabled
 * can take one of TWL4030_MADC_RT, TWL4030_MADC_SW1, TWL4030_MADC_SW2
 * corresponding to RT, SW1, SW2 conversion requests.
 * Returns error if i2c read/write fails.
 */
static int twl4030_madc_disable_irq(struct twl4030_madc_data *madc, int id)
{
	u8 val;
	int ret;

	ret = twl_i2c_read_u8(TWL4030_MODULE_MADC, &val, madc->imr);
	if (ret) {
		dev_dbg(madc->hwmon_dev, "unable to read imr register 0x%X\n",
			madc->imr);
		return ret;
	}
	val |= (1 << id);
	ret = twl_i2c_write_u8(TWL4030_MODULE_MADC, val, madc->imr);
	if (ret) {
		dev_err(madc->hwmon_dev,
			"unable to write imr register 0x%X\n", madc->imr);
		return ret;
	}

	return 0;
}

static irqreturn_t twl4030_madc_threaded_irq_handler(int irq, void *_madc)
{
	struct twl4030_madc_data *madc = _madc;
	const struct twl4030_madc_conversion_method *method;
	u8 isr_val, imr_val;
	int i, len, ret;
	struct twl4030_madc_request *r;

	ret = twl_i2c_read_u8(TWL4030_MODULE_MADC, &isr_val, madc->isr);
	if (ret) {
		dev_dbg(madc->hwmon_dev, "unable to read isr register 0x%X\n",
			madc->isr);
		goto err_i2c;
	}
	ret = twl_i2c_read_u8(TWL4030_MODULE_MADC, &imr_val, madc->imr);
	if (ret) {
		dev_dbg(madc->hwmon_dev, "unable to read imr register 0x%X\n",
			madc->imr);
		goto err_i2c;
	}

	isr_val &= ~imr_val;

	for (i = 0; i < TWL4030_MADC_NUM_METHODS; i++) {

		if (!(isr_val & (1 << i)))
			continue;

		ret = twl4030_madc_disable_irq(madc, i);
		if (ret < 0) {
			dev_dbg(madc->hwmon_dev,
			"Disable interrupt failed%d\n", i);
		}

		madc->requests[i].result_pending = 1;
	}
	mutex_lock(&madc->lock);
	for (i = 0; i < TWL4030_MADC_NUM_METHODS; i++) {

		r = &madc->requests[i];

		/* No pending results for this method, move to next one */
		if (!r->result_pending)
			continue;

		method = &twl4030_conversion_methods[r->method];
		/* Read results */
		len = twl4030_madc_read_channels(madc, method->rbase,
						 r->channels, r->rbuf);

		/* Return results to caller */
		if (r->func_cb != NULL) {
			r->func_cb(len, r->channels, r->rbuf);
			r->func_cb = NULL;
		}

		/* Free request */
		r->result_pending = 0;
		r->active = 0;
	}
	mutex_unlock(&madc->lock);

	return IRQ_HANDLED;

err_i2c:
	/*
	 *  In case of error check whichever is active
	 *   and service the same.
	 */
	mutex_lock(&madc->lock);
	for (i = 0; i < TWL4030_MADC_NUM_METHODS; i++) {
		r = &madc->requests[i];
		if (r->active == 0)
			continue;
		method = &twl4030_conversion_methods[r->method];
		/* Read results */
		len = twl4030_madc_read_channels(madc, method->rbase,
			r->channels, r->rbuf);

		/* Return results to caller */
		if (r->func_cb != NULL) {
			r->func_cb(len, r->channels, r->rbuf);
			r->func_cb = NULL;
		}

		/* Free request */
		r->result_pending = 0;
		r->active = 0;
	}
	mutex_unlock(&madc->lock);

	return IRQ_HANDLED;
}

static int twl4030_madc_set_irq(struct twl4030_madc_data *madc,
				struct twl4030_madc_request *req)
{
	struct twl4030_madc_request *p;
	int ret;
	p = &madc->requests[req->method];

	memcpy(p, req, sizeof *req);

	ret = twl4030_madc_enable_irq(madc, req->method);
	if (ret < 0) {
		dev_dbg(madc->hwmon_dev, "enable irq failed!!\n");
		return ret;
	}

	return 0;
}

/*
 * Function which enables the madc conversion
 * by writing to the control register.
 * @madc - pointer to twl4030_madc_data struct
 * @conv_method - can be TWL4030_MADC_RT, TWL4030_MADC_SW2, TWL4030_MADC_SW1
 * corresponding to RT SW1 or SW2 conversion methods.
 * Returns 0 if succeeds else a negative error value
 */
static int twl4030_madc_start_conversion(struct twl4030_madc_data *madc,
						 int conv_method)
{
	const struct twl4030_madc_conversion_method *method;
	int ret = 0;

	method = &twl4030_conversion_methods[conv_method];

	switch (conv_method) {
	case TWL4030_MADC_SW1:
	case TWL4030_MADC_SW2:
		ret = twl_i2c_write_u8(TWL4030_MODULE_MADC,
			TWL4030_MADC_SW_START, method->ctrl);
		if (ret) {
			dev_err(madc->hwmon_dev,
			"unable to write ctrl register 0x%X\n", method->ctrl);
			return ret;
		}
		break;
	case TWL4030_MADC_RT:
	default:
		break;
	}

	return 0;
}

/*
 * Function that waits for conversion to be ready
 * @madc - pointer to twl4030_madc_data struct
 * @timeout_ms - timeout value in mili seconds
 * @status_reg - ctrl register
 * returns 0 if succeeds else a negative error value
 */
static int twl4030_madc_wait_conversion_ready(struct twl4030_madc_data *madc,
					      unsigned int timeout_ms,
					      u8 status_reg)
{
	unsigned long timeout;
	int ret;

	timeout = jiffies + msecs_to_jiffies(timeout_ms);
	do {
		u8 reg;

		ret = twl_i2c_read_u8(TWL4030_MODULE_MADC, &reg, status_reg);
		if (ret) {
			dev_dbg(madc->hwmon_dev,
				"unable to read status register 0x%X\n",
				status_reg);
			return ret;
		}
		if (!(reg & TWL4030_MADC_BUSY) && (reg & TWL4030_MADC_EOC_SW))
			return 0;
		usleep_range(500, 2000);
	} while (!time_after(jiffies, timeout));
	dev_dbg(madc->hwmon_dev, "conversion timeout!\n");

	return -EAGAIN;
}

/*
 * An exported function which can be called from other kernel drivers.
 * @req twl4030_madc_request structure
 * req->rbuf will be filled with read values of channels based on the
 * channel index. If a particular channel reading fails there will
 * be a negative error value in the correspoding array element.
 * retuns 0 is succeeds else error value
 */
int twl4030_madc_conversion(struct twl4030_madc_request *req)
{
	const struct twl4030_madc_conversion_method *method;
	u8 ch_msb, ch_lsb;
	int ret;

	if (!req)
		return -EINVAL;

	mutex_lock(&twl4030_madc->lock);

	if (req->method < TWL4030_MADC_RT || req->method > TWL4030_MADC_SW2) {
		ret = -EINVAL;
		goto out;
	}

	/* Do we have a conversion request ongoing */
	if (twl4030_madc->requests[req->method].active) {
		ret = -EBUSY;
		goto out;
	}

	ch_msb = (req->channels >> 8) & 0xff;
	ch_lsb = req->channels & 0xff;

	method = &twl4030_conversion_methods[req->method];

	/* Select channels to be converted */
	ret = twl_i2c_write_u8(TWL4030_MODULE_MADC, ch_msb, method->sel + 1);
	if (ret) {
		dev_dbg(twl4030_madc->hwmon_dev,
		"unable to write sel register 0x%X\n", method->sel + 1);
		return ret;
	}
	ret = twl_i2c_write_u8(TWL4030_MODULE_MADC, ch_lsb, method->sel);
	if (ret) {
		dev_dbg(twl4030_madc->hwmon_dev,
		"unable to write sel register 0x%X\n", method->sel + 1);
		return ret;
	}

	/* Select averaging for all channels if do_avg is set */
	if (req->do_avg) {
		ret = twl_i2c_write_u8(TWL4030_MODULE_MADC,
			ch_msb, method->avg + 1);
		if (ret) {
			dev_dbg(twl4030_madc->hwmon_dev,
			"unable to write avg register 0x%X\n", method->avg + 1);
			return ret;
		}
		ret = twl_i2c_write_u8(TWL4030_MODULE_MADC,
			ch_lsb, method->avg);
		if (ret) {
			dev_dbg(twl4030_madc->hwmon_dev,
			"unable to write sel reg 0x%X\n", method->sel + 1);
			return ret;
		}
	}

	if (req->type == TWL4030_MADC_IRQ_ONESHOT && req->func_cb != NULL) {
		ret = twl4030_madc_set_irq(twl4030_madc, req);
		if (ret < 0)
			goto out;
		ret = twl4030_madc_start_conversion(twl4030_madc, req->method);
		if (ret < 0)
			goto out;
		twl4030_madc->requests[req->method].active = 1;
		ret = 0;
		goto out;
	}

	/* With RT method we should not be here anymore */
	if (req->method == TWL4030_MADC_RT) {
		ret = -EINVAL;
		goto out;
	}

	ret = twl4030_madc_start_conversion(twl4030_madc, req->method);
	if (ret < 0)
		goto out;
	twl4030_madc->requests[req->method].active = 1;

	/* Wait until conversion is ready (ctrl register returns EOC) */
	ret = twl4030_madc_wait_conversion_ready(twl4030_madc, 5, method->ctrl);
	if (ret) {
		twl4030_madc->requests[req->method].active = 0;
		goto out;
	}

	ret = twl4030_madc_read_channels(twl4030_madc, method->rbase,
		req->channels, req->rbuf);
	twl4030_madc->requests[req->method].active = 0;

out:
	mutex_unlock(&twl4030_madc->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(twl4030_madc_conversion);

/*
 * Return channel value
 * Or < 0 on failure.
 */
int twl4030_get_madc_conversion(int channel_no)
{
	struct twl4030_madc_request req;
	int temp = 0;
	int ret;

	req.channels = (1 << channel_no);
	req.method = TWL4030_MADC_SW2;
	req.active = 0;
	req.func_cb = NULL;
	ret = twl4030_madc_conversion(&req);
	if (ret < 0)
		return ret;

	if (req.rbuf[channel_no] > 0)
		temp = req.rbuf[channel_no];

	return temp;
}
EXPORT_SYMBOL_GPL(twl4030_get_madc_conversion);

/*
 * Function to enable or disble bias current for
 * main battery type reading or temperature sensing
 * @madc - pointer to twl4030_madc_data struct
 * @chan - cab be one of the two values
 * TWL4030_BCI_ITHEN - Enables bias current for main battery type reading
 * TWL4030_BCI_TYPEN - Enables bias current for main battery temperature
 * sensing
 * @on - enable or disable chan.
 */
static int twl4030_madc_set_current_generator(struct twl4030_madc_data *madc,
					      int chan, int on)
{
	int ret;
	u8 regval;

	ret = twl_i2c_read_u8(TWL4030_MODULE_MAIN_CHARGE,
			      &regval, TWL4030_BCI_BCICTL1);
	if (ret) {
		dev_dbg(madc->hwmon_dev,
		"unable to read BCICTL1 reg 0x%X", TWL4030_BCI_BCICTL1);
		return ret;
	}
	if (on)
		regval |= chan ? TWL4030_BCI_ITHEN : TWL4030_BCI_TYPEN;
	else
		regval &= chan ? ~TWL4030_BCI_ITHEN : ~TWL4030_BCI_TYPEN;
	ret = twl_i2c_write_u8(TWL4030_MODULE_MAIN_CHARGE,
			       regval, TWL4030_BCI_BCICTL1);
	if (ret) {
		dev_err(madc->hwmon_dev,
		"unable to write BCICTL1 reg 0x%X\n", TWL4030_BCI_BCICTL1);
		return ret;
	}

	return 0;
}

/*
 * Function that sets MADC software power on bit to enable MADC
 * @madc - pointer to twl4030_madc_data struct
 * @on - Enable or diable MADC software powen on bit.
 * returns error if i2c read/write fails else 0
 */
static int twl4030_madc_set_power(struct twl4030_madc_data *madc, int on)
{
	u8 regval;
	int ret;
	ret = twl_i2c_read_u8(TWL4030_MODULE_MAIN_CHARGE,
			      &regval, TWL4030_MADC_CTRL1);
	if (ret) {
		dev_dbg(madc->hwmon_dev,
		"unable to read madc ctrl1 reg 0x%X\n", TWL4030_MADC_CTRL1);
		return ret;
	}

	if (on)
		regval |= TWL4030_MADC_MADCON;
	else
		regval &= ~TWL4030_MADC_MADCON;
	ret = twl_i2c_write_u8(TWL4030_MODULE_MADC, regval, TWL4030_MADC_CTRL1);
	if (ret) {
		dev_dbg(madc->hwmon_dev,
		"unable to write madc ctrl1 reg 0x%X\n", TWL4030_MADC_CTRL1);
		return ret;
	}

	return 0;
}

/* sysfs nodes to read individual channels from user side */
static SENSOR_DEVICE_ATTR(in0_input, S_IRUGO, madc_read, NULL, 0);
static SENSOR_DEVICE_ATTR(in1_input, S_IRUGO, madc_read, NULL, 1);
static SENSOR_DEVICE_ATTR(in2_input, S_IRUGO, madc_read, NULL, 2);
static SENSOR_DEVICE_ATTR(in3_input, S_IRUGO, madc_read, NULL, 3);
static SENSOR_DEVICE_ATTR(in4_input, S_IRUGO, madc_read, NULL, 4);
static SENSOR_DEVICE_ATTR(in5_input, S_IRUGO, madc_read, NULL, 5);
static SENSOR_DEVICE_ATTR(in6_input, S_IRUGO, madc_read, NULL, 6);
static SENSOR_DEVICE_ATTR(in7_input, S_IRUGO, madc_read, NULL, 7);
static SENSOR_DEVICE_ATTR(in8_input, S_IRUGO, madc_read, NULL, 8);
static SENSOR_DEVICE_ATTR(in9_input, S_IRUGO, madc_read, NULL, 9);
static SENSOR_DEVICE_ATTR(in10_input, S_IRUGO, madc_read, NULL, 10);
static SENSOR_DEVICE_ATTR(in11_input, S_IRUGO, madc_read, NULL, 11);
static SENSOR_DEVICE_ATTR(in12_input, S_IRUGO, madc_read, NULL, 12);
static SENSOR_DEVICE_ATTR(in13_input, S_IRUGO, madc_read, NULL, 13);
static SENSOR_DEVICE_ATTR(in14_input, S_IRUGO, madc_read, NULL, 14);
static SENSOR_DEVICE_ATTR(in15_input, S_IRUGO, madc_read, NULL, 15);

static struct attribute *twl4030_madc_attributes[] = {
	&sensor_dev_attr_in0_input.dev_attr.attr,
	&sensor_dev_attr_in1_input.dev_attr.attr,
	&sensor_dev_attr_in2_input.dev_attr.attr,
	&sensor_dev_attr_in3_input.dev_attr.attr,
	&sensor_dev_attr_in4_input.dev_attr.attr,
	&sensor_dev_attr_in5_input.dev_attr.attr,
	&sensor_dev_attr_in6_input.dev_attr.attr,
	&sensor_dev_attr_in7_input.dev_attr.attr,
	&sensor_dev_attr_in8_input.dev_attr.attr,
	&sensor_dev_attr_in9_input.dev_attr.attr,
	&sensor_dev_attr_in10_input.dev_attr.attr,
	&sensor_dev_attr_in11_input.dev_attr.attr,
	&sensor_dev_attr_in12_input.dev_attr.attr,
	&sensor_dev_attr_in13_input.dev_attr.attr,
	&sensor_dev_attr_in14_input.dev_attr.attr,
	&sensor_dev_attr_in15_input.dev_attr.attr,
	NULL
};

static const struct attribute_group twl4030_madc_group = {
	.attrs = twl4030_madc_attributes,
};

/*
 * Initialize MADC and request for threaded irq
 * and register as a hwmon device
 */
static int __devinit twl4030_madc_probe(struct platform_device *pdev)
{
	struct twl4030_madc_data *madc;
	struct twl4030_madc_platform_data *pdata = pdev->dev.platform_data;
	int ret;
	int status;
	u8 regval;

	if (!pdata) {
		dev_dbg(&pdev->dev, "platform_data not available\n");
		return -EINVAL;
	}

	madc = kzalloc(sizeof *madc, GFP_KERNEL);
	if (!madc)
		return -ENOMEM;
	madc->imr = (pdata->irq_line == 1) ?
	    TWL4030_MADC_IMR1 : TWL4030_MADC_IMR2;
	madc->isr = (pdata->irq_line == 1) ?
	    TWL4030_MADC_ISR1 : TWL4030_MADC_ISR2;
	ret = twl4030_madc_set_power(madc, 1);
	if (ret < 0)
		goto err_power;
	ret = twl4030_madc_set_current_generator(madc, 0, 1);
	if (ret < 0)
		goto err_current_generator;

	ret = twl_i2c_read_u8(TWL4030_MODULE_MAIN_CHARGE,
			      &regval, TWL4030_BCI_BCICTL1);
	if (ret) {
		dev_err(&pdev->dev,
		"unable to read reg BCI CTL1 0x%X\n", TWL4030_BCI_BCICTL1);
		goto err_i2c;
	}
	regval |= TWL4030_BCI_MESBAT;

	ret = twl_i2c_write_u8(TWL4030_MODULE_MAIN_CHARGE,
			       regval, TWL4030_BCI_BCICTL1);
	if (ret) {
		dev_err(&pdev->dev,
		"unable to write reg BCI Ctl1 0x%X\n", TWL4030_BCI_BCICTL1);
		goto err_i2c;
	}

	platform_set_drvdata(pdev, madc);
	mutex_init(&madc->lock);

	ret = request_threaded_irq(platform_get_irq(pdev, 0), NULL,
				   twl4030_madc_threaded_irq_handler,
				   IRQF_TRIGGER_RISING, "twl4030_madc", madc);
	if (ret) {
		dev_dbg(&pdev->dev, "could not request irq\n");
		goto err_irq;
	}

	ret = sysfs_create_group(&pdev->dev.kobj, &twl4030_madc_group);
	if (ret)
		goto err_sysfs;

	madc->hwmon_dev = hwmon_device_register(&pdev->dev);
	if (IS_ERR(madc->hwmon_dev)) {
		dev_err(&pdev->dev, "hwmon_device_register failed.\n");
		status = PTR_ERR(madc->hwmon_dev);
		goto err_reg;
	}

	twl4030_madc = madc;
	return 0;

err_reg:
	sysfs_remove_group(&pdev->dev.kobj, &twl4030_madc_group);

err_sysfs:
	free_irq(platform_get_irq(pdev, 0), madc);
err_irq:
	platform_set_drvdata(pdev, NULL);

err_i2c:
	twl4030_madc_set_current_generator(madc, 0, 0);

err_current_generator:
	twl4030_madc_set_power(madc, 0);
err_power:
	kfree(madc);

	return ret;
}

static int __devexit twl4030_madc_remove(struct platform_device *pdev)
{
	struct twl4030_madc_data *madc = platform_get_drvdata(pdev);
	hwmon_device_unregister(&pdev->dev);
	sysfs_remove_group(&pdev->dev.kobj, &twl4030_madc_group);
	free_irq(platform_get_irq(pdev, 0), madc);
	platform_set_drvdata(pdev, NULL);
	twl4030_madc_set_current_generator(madc, 0, 0);
	twl4030_madc_set_power(madc, 0);
	kfree(madc);

	return 0;
}

static struct platform_driver twl4030_madc_driver = {
	.probe = twl4030_madc_probe,
	.remove = __exit_p(twl4030_madc_remove),
	.driver = {
		   .name = "twl4030_madc",
		   .owner = THIS_MODULE,
		   },
};

static int __init twl4030_madc_init(void)
{
	return platform_driver_register(&twl4030_madc_driver);
}

module_init(twl4030_madc_init);

static void __exit twl4030_madc_exit(void)
{
	platform_driver_unregister(&twl4030_madc_driver);
}

module_exit(twl4030_madc_exit);

MODULE_DESCRIPTION("TWL4030 ADC driver");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("J Keerthy");
