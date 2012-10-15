#ifndef LINUX_GPIO_WHOIFPGA_H
#define LINUX_GPIO_WHOIFPGA_H

//Define Max Number of GPIO Pins
#define WHOIFPGA_NR_GPIOS 11

/*
 * Map the Version API Registers so the driver can load them at initialization time.
 */

#define FPGA_API_LEVEL (0x00) //0x00
#define FPGA_VER_MAJOR (0x02) //0x01
#define FPGA_VER_MILE  (0x04) //0x02
#define FPGA_VER_MINOR (0x06) //0x03
#define FPGA_VER_DEVEL (0x08) //0x04
#define FPGA_VER_FLAG  (0x0A) //0x05

/*
 * Define the Magic Number registers for Verification of Hardware
 */
#define MAGIC1	       (0x20) //0x010
#define MAGIC2	       (0x22) //0x011
#define TEST1	       (0x24) //0x012
#define TEST2	       (0x26) //0x013
#define TEST3	       (0x28) //0x014

#define ADDRTEST1	(0x4AA) //0x255
#define ADDRTEST2	(0x354) //0x1AA

/*
 * Define the Fixed Hibernate Register
 */
#define HIBERNATE	(0x2A) //0x15

/*
 * Watchdog Registers
 */
#define WATCHDOG_KICK		(0x80) //0x040
#define WATCHDOG_ENABLE		(0x82) //0x041
#define WATCHDOG_INTERVAL	(0x84) //0x042
#define WATCHDOG_TIME		(0x86) //0x043

/*
 * Map to the Ctrl Register in the FPGA
 */
#define IO_CTRL_BASE	(0x42) //0x21

/*
 * Map to the Status Register in the FPGA
 */
#define IO_CTRL_STATUS	(0xC2) //0x61

struct whoifpga_platform_data {
	unsigned	gpio_base;
	void 		*fpga_base_address;
};

#endif
