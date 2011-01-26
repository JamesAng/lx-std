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
#include <linux/gpio_whoifpga.h>

static DEFINE_SPINLOCK(gpio_lock);

#define IO_CTRL_BASE	(0x42)
#define IO_CTRL_STATUS	(0x62)

void __iomem *fpga_base;

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

static int __devinit whoifpga_gpio_probe(struct platform_device *pdev)
{
	struct whoifpga_platform_data *pdata;
	int err;

	pdata = pdev->dev.platform_data;
	if (!pdata || !pdata->gpio_base || !pdata->fpga_base_address) {
		dev_dbg(&pdev->dev, "incorrect or missing platform data\n");
		return -EINVAL;
	}

	/* Static mapping, never released */
	fpga_base = ioremap(pdata->fpga_base_address, 1024);
	if (!fpga_base) {
		dev_dbg(&pdev->dev, "Could not ioremap fpga_base\n");
		goto err_whoifpga_gpio;
	}

	whoifpga_gpio.base = pdata->gpio_base;
	whoifpga_gpio.ngpio = WHOIFPGA_NR_GPIOS;
	whoifpga_gpio.dev = &pdev->dev;

	err = gpiochip_add(&whoifpga_gpio);
	if (err < 0)
		goto err_whoifpga_gpio;

	dev_info(&pdev->dev, "FPGA at 0x%08x, %d GPIO's based at %d\n",
		pdata->fpga_base_address, WHOIFPGA_NR_GPIOS, whoifpga_gpio.base);

	return 0;

err_whoifpga_gpio:
	fpga_base = 0;

	return err;
}

static int __devexit whoifpga_gpio_remove(struct platform_device *pdev)
{
	if (fpga_base) {
		int err;

		err  = gpiochip_remove(&whoifpga_gpio);
		if (err)
			dev_err(&pdev->dev, "%s failed, %d\n",
				"gpiochip_remove()", err);

		fpga_base = 0;

		return err;
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
MODULE_DESCRIPTION("GPIO interface for WHOI FPGA");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:whoifpga_gpio");
