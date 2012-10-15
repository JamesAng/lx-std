/*
 *  whoifpga_gpio.c - GPIO interface for WHOI FPGA
 *
 *  Author: Steve Sakoman <steve@sakoman.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License 2 as published
 *  by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/io.h>
#include <linux/errno.h>
#include <linux/platform_device.h>

#include <linux/gpio.h>
#include <linux/watchdog.h>
#include <linux/gpio_whoifpga.h>

#define WHOIFPGA_DEBUG 1

//Define the Hardware Spin Lock
static DEFINE_SPINLOCK(gpio_lock);

//GPIO Data
void __iomem *fpga_base;

//HW Verification Function
static int whoifpga_hw_verification(struct platform_device *pdev)
{
	u16 err = 0;
	u16 magic1, magic2;
	u16 test1, test2;
	u16 test3_1, test3_2;
	u16 addrtest1, addrtest2;

	spin_lock(&gpio_lock);

	//Verify Magic Numbers
	magic1 = __raw_readw(fpga_base + MAGIC1);
	if (magic1 != 0x4572)
	{
		dev_err(&pdev->dev, "WHOI FPGA MAGIC1 Mismatch: expected 0x4572, %x\n", magic1);
		err +=1;

	}

	magic2 = __raw_readw(fpga_base + MAGIC2);
	if (magic2 != 0x6963)
	{
		dev_err(&pdev->dev, "WHOI FPGA MAGIC2 Mismatch: expected 0x6963, %x\n", magic2);
		err +=2;
	}


#ifdef WHOIFPGA_DEBUG
	//Test Data Bus
	test1  = __raw_readw(fpga_base + TEST1);
	if (test1 != 0xa5a5)
	{
		dev_err(&pdev->dev, "WHOI FPGA DATA BUS TEST1 Mismatch: expected 0xA5A5, %x\n", test1);
		err +=4;
	}
	test2  = __raw_readw(fpga_base + TEST2);
	if (test2 != 0x5a5a)
	{
		dev_err(&pdev->dev, "DATA BUS TEST2 Mismatch: expected 0x5A5A, %x\n", test2);
		err +=8;
	}

	//Test for Floating Bits
	__raw_writew(0xFFFF,fpga_base + TEST3);
	test3_1  = __raw_readw(fpga_base + TEST3);
	if (test3_1 != 0x0000)
	{
		dev_err(&pdev->dev, "WHOI FPGA FLOATING BIT TEST1 Mismatch: expected 0x0000l, %x\n", test3_1);
		err +=16;
	}

	__raw_writew(0x0000,fpga_base + TEST3);
	test3_2  = __raw_readw(fpga_base + TEST3);
	if (test3_2 != 0xFFFF)
	{
		dev_err(&pdev->dev, "WHOI FPGA FLOATING BIT TEST1 Mismatch: expected 0xFFFF, %x\n", test3_1);
		err +=32;
	}

	//Test for Bad Address Lines
	addrtest1 = __raw_readw(fpga_base + ADDRTEST1);
	if (addrtest1 != 0xAD01)
	{
		dev_err(&pdev->dev, "WHOI FPGA ADDRESS BUS TEST1 Mismatch: expected 0xAD01, %x\n", addrtest1);
		err +=64;
	}

	addrtest2 = __raw_readw(fpga_base + ADDRTEST2);
	if (addrtest2 != 0xAD02)
	{
		dev_err(&pdev->dev, "WHOI FPGA ADDRESS BUS TEST2 Mismatch: expected 0xAD02, %x\n", addrtest2);
		err +=128;
	}
#endif
	spin_unlock(&gpio_lock);
	return err;
}

// GPIO Functions
static int whoifpga_gpio_direction_in(struct gpio_chip *gc, unsigned  gpio_num)
{
	void __iomem *reg = fpga_base;
	u16 dat;

	spin_lock(&gpio_lock);

	reg += IO_CTRL_BASE;
	reg += gpio_num * 2;

	dat = __raw_readw(reg);
	dat &= 0x02;
	__raw_writew(dat, reg);

	spin_unlock(&gpio_lock);
	return 0;
}

static int whoifpga_gpio_get(struct gpio_chip *gc, unsigned gpio_num)
{
	void __iomem *reg = fpga_base;
	u16 dat;

	reg += IO_CTRL_STATUS;
	reg += gpio_num * 2;

	dat = __raw_readw(reg) & 0x01;
	return (int) dat;
}

static void whoifpga_gpio_set(struct gpio_chip *gc, unsigned gpio_num, int val)
{
	void __iomem *reg = fpga_base;
	u16 dat;

	spin_lock(&gpio_lock);

	reg += IO_CTRL_BASE;
	reg += gpio_num * 2;

	dat = __raw_readw(reg);
	if (val)
		dat |= 1;
	else
		dat &= ~1;

	__raw_writew(dat, reg);
	spin_unlock(&gpio_lock);
}

static int whoifpga_gpio_direction_out(struct gpio_chip *gc,
					unsigned gpio_num, int val)
{
	void __iomem *reg = fpga_base;
	u16 dat;

	spin_lock(&gpio_lock);

	reg += IO_CTRL_BASE;
	reg += gpio_num * 2;

	dat = __raw_readw(reg);
	dat |= 0x02;
	__raw_writew(dat, reg);

	spin_unlock(&gpio_lock);
	return 0;
}

static struct gpio_chip whoifpga_gpio = {
	.label			= "whoifpga_gpio",
	.owner			= THIS_MODULE,
	.direction_input	= whoifpga_gpio_direction_in,
	.get			= whoifpga_gpio_get,
	.direction_output	= whoifpga_gpio_direction_out,
	.set			= whoifpga_gpio_set,
};

//Watchdog Functions

static int whoifpga_wd_start(struct watchdog_device * wd)
{
	void __iomem *reg = fpga_base;
	u16 dat;

	spin_lock(&gpio_lock);

	reg += WATCHDOG_ENABLE;

	dat = 0x0001;

	__raw_writew(dat, reg);
	spin_unlock(&gpio_lock);

	return 0;
}

static int whoifpga_wd_stop(struct watchdog_device * wd)
{
	void __iomem *reg = fpga_base;
	u16 dat;

	spin_lock(&gpio_lock);

	reg += WATCHDOG_ENABLE;

	dat = 0x0000;

	__raw_writew(dat, reg);
	spin_unlock(&gpio_lock);

	return 0;
}

static int whoifpga_wd_kick(struct watchdog_device * wd)
{
	void __iomem *reg = fpga_base;
	u16 dat;

	spin_lock(&gpio_lock);

	reg += WATCHDOG_KICK;

	dat = 0x0001;

	__raw_writew(dat, reg);
	spin_unlock(&gpio_lock);

	return 0;
}
/* Future Proofing
static int whoifpga_wd_get_timeleft(struct watchdog_device * wd)
{
	void __iomem *reg = fpga_base;
	u16 time_elapsed, timeout;

	spin_lock(&gpio_lock);

	reg += WATCHDOG_TIME;

	time_elapsed = __raw_readw(reg) ;

	reg= fpga_base + WATCHDOG_INTERVAL;

	timeout = __raw_readw(reg);

	return timeout - time_elapsed;
}
*/
static int whoifpga_wd_set_timeout(struct watchdog_device * wd, unsigned int t)
{
	void __iomem *reg = fpga_base;
	u16 dat;

	spin_lock(&gpio_lock);

	reg += WATCHDOG_INTERVAL;

	//Mask off higher bits and set the timeout
	dat = t & 0xFFF;

	__raw_writew(dat, reg);
	spin_unlock(&gpio_lock);

	wd->timeout = dat;

	return 0;
}

const struct watchdog_info whoifpga_wd_info = {
	.identity = "WHOI FPGA Watchdog",
	.options = WDIOF_SETTIMEOUT | WDIOF_MAGICCLOSE | WDIOF_KEEPALIVEPING,
};

static struct watchdog_ops whoifpga_wd_ops = {
	.owner 			= THIS_MODULE,
	.start			= whoifpga_wd_start,
	.stop			= whoifpga_wd_stop,
	.ping			= whoifpga_wd_kick,
	.set_timeout		= whoifpga_wd_set_timeout,
// Future Proofing
//	.get_timeleft		= whoifpga_wd_get_timeleft,
};

//Watchdog Data
static struct watchdog_device whoifgpa_wd = {
	.info 			= &whoifpga_wd_info,
	.ops			= &whoifpga_wd_ops,
	.min_timeout		= 3,
	.max_timeout		= 4094,
};


static int __devinit whoifpga_gpio_probe(struct platform_device *pdev)
{
	struct whoifpga_platform_data *pdata;
	int err;
	u16 api, major, minor, mile, devel;
	char flag;

	char version_string[26];

#ifdef WHOIFPGA_DEBUG
	dev_info(&pdev->dev, "Initializing WHOI FPGA Driver\n");
#endif

	pdata = pdev->dev.platform_data;
	if (!pdata || !pdata->gpio_base || !pdata->fpga_base_address) {
		dev_err(&pdev->dev, "incorrect or missing platform data\n");
		return -EINVAL;
	}

	/* Static mapping, never released */
	fpga_base = ioremap(pdata->fpga_base_address, 1024);
	if (!fpga_base) {
		dev_err(&pdev->dev, "Could not ioremap fpga_base\n");
		goto err_whoifpga_gpio;
	}

	//GPIO Configuration
	whoifpga_gpio.base = pdata->gpio_base;
	whoifpga_gpio.ngpio = WHOIFPGA_NR_GPIOS;
	whoifpga_gpio.dev = &pdev->dev;

	err = gpiochip_add(&whoifpga_gpio);
	if (err < 0)
	{
		dev_err(&pdev->dev, "WHOI FPGA: gpiochip_add failed: %d", err);
		goto err_whoifpga_gpio;
	}

	err = whoifpga_hw_verification(pdev);
#ifndef WHOIFPGA_DEBUG
	if(err != 0)
	{

		goto err_whoifpga_gpio;

	}
#endif

	//Watchdog Configuration
	//Read our timeout from the chip
	whoifgpa_wd.timeout = __raw_readw(fpga_base + WATCHDOG_INTERVAL) & 0xFFF;

	//Register our watchdog with the Kernel Subsystems.
	err = watchdog_register_device(&whoifgpa_wd);
	if(err)
	{
		dev_err(&pdev->dev, "WHOI FPGA: watchdog_register_device failed: %d", err);
		goto err_whoifpga_gpio;

	}

	//Let's Read the Current Version info of the FPGA API.
	api = __raw_readw(fpga_base + FPGA_API_LEVEL);
	major = __raw_readw(fpga_base + FPGA_VER_MAJOR);
	mile = __raw_readw(fpga_base + FPGA_VER_MILE);
	minor = __raw_readw(fpga_base + FPGA_VER_MINOR);
	devel = __raw_readw(fpga_base + FPGA_VER_DEVEL);
	flag = (char)__raw_readw(fpga_base + FPGA_VER_FLAG);

	//Print Version information
	if(flag != '\0')
	{
		snprintf(version_string, sizeof(version_string),"%d.%d.%d.%d-%c",
			 major,mile,minor,devel,flag);
	}
	else
	{
		snprintf(version_string, sizeof(version_string),"%d.%d.%d.%d",
			 major,mile,minor,devel);
	}

	//Print Info about the Location of the GPIO base
	dev_info(&pdev->dev, "WHOI FPGA(Version %s) at 0x%08x, %d GPIO's based at %d\n", version_string,
		pdata->fpga_base_address, WHOIFPGA_NR_GPIOS, whoifpga_gpio.base);

	//Print Info about the WD
	dev_info(&pdev->dev, "WHOI FPGA WD(Version %s) with timeout:%d\n", version_string, whoifgpa_wd.timeout);

	return 0;

err_whoifpga_gpio:
	fpga_base = 0;

	return err;

}

static int __devexit whoifpga_gpio_remove(struct platform_device *pdev)
{
	if (fpga_base)
	{
		int err;

		//GPIO Handling
		err  = gpiochip_remove(&whoifpga_gpio);
		if (err)
			dev_err(&pdev->dev, "%s failed, %d\n",
				"gpiochip_remove()", err);

		//Watchdog Handling
		watchdog_unregister_device(&whoifgpa_wd);

		fpga_base = 0;
	}

	return 0;
}

static struct platform_driver whoifpga_gpio_driver = {
	.driver = {
		.name = "whoifpga_gpio",
		.owner = THIS_MODULE,
	},
	.probe		= whoifpga_gpio_probe,
	.remove		= __devexit_p(whoifpga_gpio_remove),
};

static int __init whoifpga_gpio_init(void)
{
	return platform_driver_register(&whoifpga_gpio_driver);
}

static void __exit whoifpga_gpio_exit(void)
{
	platform_driver_unregister(&whoifpga_gpio_driver);
}

module_init(whoifpga_gpio_init);
module_exit(whoifpga_gpio_exit);

MODULE_AUTHOR("Steve Sakoman <steve@sakoman.com>");
MODULE_DESCRIPTION("GPIO and Watchdog interface for WHOI FPGA");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:whoifpga");
