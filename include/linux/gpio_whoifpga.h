#ifndef LINUX_GPIO_WHOIFPGA_H
#define LINUX_GPIO_WHOIFPGA_H

#define WHOIFPGA_NR_GPIOS 11

struct whoifpga_platform_data {
	unsigned	gpio_base;
	void 		*fpga_base_address;
};

#endif
