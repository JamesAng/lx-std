/******************************************************************************

  Driver for the ZeroG Wireless G2100 series wireless devices.

  Copyright(c) 2008 ZeroG Wireless Inc. All rights reserved.

  Confidential and proprietary software of ZeroG Wireless, Inc.
  Do no copy, forward or distribute.

******************************************************************************/

#ifndef G2100_H_
#define G2100_H_

/******************************************************************************
 * Includes
 *****************************************************************************/

// Network
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/wireless.h>
#include <linux/ieee80211.h>
#include <linux/if_arp.h>
#include <net/iw_handler.h>

// SPI
#include <linux/spi/spi.h>
#include <mach/gpio.h>

// Others
#include <linux/completion.h>
#include <linux/dma-mapping.h>

/******************************************************************************
 * Build options
 *****************************************************************************/

#undef ZG_DEBUG				// Debug
#define ZG_ONCHIP_SUPP		// If defined, On-Chip supplicant is used
#undef ZG_CONN_SIMPLIFIED	// Use simplified connect; NOTE: supported on
							// G2100-C1 with FW 1003 and above only
#define ZG_ROAMING_ON		// Enable or disable roaming support
							// (WiFi Cert needs roaming)

/******************************************************************************
 * Constants & #defines
 *****************************************************************************/

#define DRIVER_NAME			"g2100" 
#define DRIVER_MAJOR		1
#define DRIVER_MINOR		1

/*
 * 802_11 specific
 */
// Supported regulatory domain values
#define ZG_REG_DOMAIN_FCC		0x00
#define ZG_REG_DOMAIN_IC		0x01
#define ZG_REG_DOMAIN_ETSI		0x02
#define ZG_REG_DOMAIN_SPAIN		0x03
#define ZG_REG_DOMAIN_FRANCE	0x04
#define ZG_REG_DOMAIN_JAPAN_A	0x05
#define ZG_REG_DOMAIN_JAPAN_B	0x06

// Security keys
#define ZG_MAX_ENCRYPTION_KEYS 		4
#define ZG_MAX_ENCRYPTION_KEY_SIZE	13
#define ZG_MAX_WPA_PASSPHRASE_LEN	64
#define ZG_MAX_PMK_LEN				32

// Max characters in network SSID
#define ZG_MAX_SSID_LENGTH		32

// Max entries in scan results
#define ZG_MAX_BSS_ENTRIES		64

// Preamble type
#define ZG_LONG_PREAMBLE		0
#define ZG_SHORT_PREAMBLE		1

// Security types
#define ZG_SECURITY_TYPE_NONE	0x00
#define ZG_SECURITY_TYPE_WEP	0x01
#define ZG_SECURITY_TYPE_WPA	0x02
#define ZG_SECURITY_TYPE_WPA2	0x03

// Adhoc roles for the device
#define ZG_ADHOC_ROLE_JOINER	0x01
#define ZG_ADHOC_ROLE_CREATOR	0x02

// Multicast address slots on device
#define ZG_TOTAL_MULTICAST_SLOTS	0x04	// # of multicast addresses stored
											// in driver
#define ZG_MULTICAST_SLOTS_AVAIL	0x02	// multicast slots available for
											// use on the device
#define ZG_MULTICAST_SLOTS_START	4		// first multicast slot
#define ZG_ADDR_TYPE_MULTICAST		6		// type of address

/*
 * endian conversion macros for 16-bit ints
 *
 * G2100 uses the big-endian format for all multi-byte integers
 * that pass between G2100 and the host. These macros should be
 * setup to reflect the endianness of the host.
 */

//Host to Zero G long
#define HTOZGL(a) (	 ((a & 0x000000ff)<<24) \
					|((a & 0x0000ff00)<<8)  \
					|((a & 0x00ff0000)>>8)  \
					|((a & 0xff000000)>>24)	)
#define ZGTOHL(a) HTOZGL(a)

// Host to Zero G short
#define HSTOZGS(a) (u16)(((a)<<8) | ((a)>>8))
#define ZGSTOHS(a) HSTOZGS(a)
#define HTONS(a) HSTOZGS(a)

// Defined debug levels : 1 (critical) > 2 > 3 > 4 > 5 (less critical)
#ifdef ZG_DEBUG
#define ZG_PRINT(n, args...) do{if (zg_debug_level>=(n)) printk(args);}while(0)
#else
#define ZG_PRINT(n, args...) do{}while (0)
#endif	/* ZG_DEBUG */

#ifndef ZG_ONCHIP_SUPP		// If on-chip supplicant disabled
// locally defined PSK; SSID : IEEE; Passphrase : 12345678
u8 zg_psk_key[32] = {	0xd9, 0x53, 0x30, 0x2c, 0xe5, 0x48, 0xa0, 0xf8,
						0x2f, 0xf8, 0x8a, 0xe5, 0x82, 0xb0, 0x1e, 0x81,
						0xf9, 0xa8, 0x76, 0xd9, 0x20, 0x69, 0x3f, 0x84,
						0x2e, 0xdd, 0xae, 0x6b, 0x14, 0x65, 0x3d, 0xea	};
#endif /* ZG_ONCHIP_SUPP */

#define ZG_INTERRUPT_PIN	160	//FIXME - dummy gpio - select actual gpio!!
/*
 * SPI message structure
 */
#define ZG_PREAMBLE_LEN					(3)

// Host to G2100
#define ZG_PREAMBLE_CMD_LEN				(1)
#define ZG_PREAMBLE_CMD_IDX				(0)

// G2100 to Host
#define ZG_PREAMBLE_STATUS_LEN			(1)
#define ZG_PREAMBLE_STATUS_IDX			(0)

// Common to (Host -> G2100) and (G2100 -> Host)
#define ZG_PREAMBLE_TYPE_IDX			(1)
#define ZG_PREAMBLE_SUBTYPE_IDX			(2)
#define ZG_PREAMBLE_SET_PARAM_ID_IDX	(3)
#define ZG_PREAMBLE_GET_PARAM_ID_IDX	(3)

#define ZG_GET_PARAM_REQ_SIZE	(4)
#define ZG_SET_PARAM_REQ_SIZE	(2)

#define ZG_GET_PARAM_CNF_SIZE	(4)

// Command values which appear in ZG_PREAMBLE_CMD_IDX for each SPI message
#define ZG_CMD_FIFO_ACCESS			(0x80)
#define ZG_CMD_WT_FIFO_DATA			(ZG_CMD_FIFO_ACCESS | 0x20)
#define ZG_CMD_WT_FIFO_MGMT			(ZG_CMD_FIFO_ACCESS | 0x30)
#define ZG_CMD_RD_FIFO				(ZG_CMD_FIFO_ACCESS | 0x00)
#define ZG_CMD_WT_FIFO_DONE			(ZG_CMD_FIFO_ACCESS | 0x40)
#define ZG_CMD_RD_FIFO_DONE			(ZG_CMD_FIFO_ACCESS | 0x50)
#define ZG_CMD_WT_REG				(0x00)
#define ZG_CMD_RD_REG				(0x40)

// Type values which appear in ZG_PREAMBLE_TYPE_IDX for each SPI message
#define ZG_MAC_TYPE_TXDATA_REQ		((u8)1)
#define ZG_MAC_TYPE_MGMT_REQ		((u8)2)

#define ZG_MAC_TYPE_TXDATA_CONFIRM	((u8)1)
#define ZG_MAC_TYPE_MGMT_CONFIRM	((u8)2)
#define ZG_MAC_TYPE_RXDATA_INDICATE	((u8)3)
#define ZG_MAC_TYPE_MGMT_INDICATE	((u8)4)

// Subtype values which appear in ZG_PREAMBLE_SUBTYPE_IDX for each SPI message
// Subtype for ZG_MAC_TYPE_TXDATA_REQ and ZG_MAC_TYPE_TXDATA_CONFIRM
#define ZG_MAC_SUBTYPE_TXDATA_REQ_STD			((u8)1)

// Subtype for ZG_MAC_TYPE_MGMT_REQ and ZG_MAC_TYPE_MGMT_CONFIRM
#define ZG_MAC_SUBTYPE_MGMT_REQ_SCAN			((u8)1)
#define ZG_MAC_SUBTYPE_MGMT_REQ_JOIN			((u8)2)
#define ZG_MAC_SUBTYPE_MGMT_REQ_AUTH			((u8)3)
#define ZG_MAC_SUBTYPE_MGMT_REQ_ASSOC			((u8)4)
#define ZG_MAC_SUBTYPE_MGMT_REQ_DEAUTH			((u8)5)
#define ZG_MAC_SUBTYPE_MGMT_REQ_DISASSOC		((u8)6)
#define ZG_MAC_SUBTYPE_MGMT_REQ_PWR_MODE		((u8)7)
#define ZG_MAC_SUBTYPE_MGMT_REQ_PMK_KEY			((u8)8)
#define ZG_MAC_SUBTYPE_MGMT_REQ_WEP_KEY			((u8)10)
#define ZG_MAC_SUBTYPE_MGMT_REQ_CALC_PSK		((u8)12)
#define ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM		((u8)15)
#define ZG_MAC_SUBTYPE_MGMT_REQ_GET_PARAM		((u8)16)
#define ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_CONNECT	((u8)17)
#define ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_START		((u8)18)
#define ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT			((u8)19)
#define ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT_MANAGE	((u8)20)

// Subtype for ZG_MAC_TYPE_RXDATA_INDICATE
#define ZG_MAC_SUBTYPE_RXDATA_IND_STD			((u8)1)

// Subtype for ZG_MAC_TYPE_MGMT_INDICATE
#define ZG_MAC_SUBTYPE_MGMT_IND_DISASSOC		((u8)1)
#define ZG_MAC_SUBTYPE_MGMT_IND_DEAUTH			((u8)2)
#define ZG_MAC_SUBTYPE_MGMT_IND_CONN_STATUS		((u8)4)

// Parameter IDs for ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM
#define ZG_PARAM_MAC_ADDRESS			(1)
#define ZG_PARAM_REG_DOMAIN				(2)
#define ZG_PARAM_RTS_THRESHOLD			(3)
#define ZG_PARAM_LONG_RETRY_LIMIT		(4)
#define ZG_PARAM_SHORT_RETRY_LIMIT		(5)
#define ZG_PARAM_MISSED_BEACON_THRESH	(12)
#define ZG_PARAM_MULTICAST_ADDR			(19)
#define ZG_PARAM_STAT_COUNTER			(22)
#define ZG_PARAM_TX_RATE_TABLE_ONOFF	(24)
#define ZG_PARAM_SYSTEM_VERSION			(26)

// Result for ZG_MAC_TYPE_MGMT_CONFIRM
#define ZG_MGMT_CONFIRM_RESULT_IDX		(0)

// MAC result code
enum {
    ZG_RESULT_SUCCESS = 1,
    ZG_RESULT_INVALID_SUBTYPE,
    ZG_RESULT_CANCELLED,
    ZG_RESULT_FRAME_EOL,
    ZG_RESULT_FRAME_RETRY_LIMIT,
    ZG_RESULT_FRAME_NO_BSS,
    ZG_RESULT_FRAME_TOO_BIG,
    ZG_RESULT_FRAME_ENCRYPT_FAILURE,
    ZG_RESULT_INVALID_PARAMS,
    ZG_RESULT_ALREADY_AUTH,
    ZG_RESULT_ALREADY_ASSOC,
    ZG_RESULT_INSUFFICIENT_RSRCS,
    ZG_RESULT_TIMEOUT,
    ZG_RESULT_BAD_EXCHANGE,	// frame exchange problem with peer (AP or STA)
    ZG_RESULT_AUTH_REFUSED,		// authenticating node refused our request
    ZG_RESULT_ASSOC_REFUSED,	// associating node refused our request
    ZG_RESULT_REQ_IN_PROGRESS,	// only one mlme request at a time allowed
    ZG_RESULT_NOT_JOINED,			// operation requires that device be joined
    								// with target
    ZG_RESULT_NOT_ASSOC,			// operation requires that device be
    								// associated with target
    ZG_RESULT_NOT_AUTH,				// operation requires that device be
    								// authenticated with target
    ZG_RESULT_SUPPLICANT_FAILED,
    ZG_RESULT_UNSUPPORTED_FEATURE,
    ZG_RESULT_REQUEST_OUT_OF_SYNC	// Returned when a request is recognized
    								// but invalid given the current state
    								// of the MAC 
};

/*
 * G2100 command registers
 */
#define ZG_INTR_REG					(0x01)	// 8-bit register containing interrupt bits
#define ZG_INTR_MASK_REG			(0x02)	// 8-bit register containing interrupt mask
#define ZG_SYS_INFO_DATA_REG		(0x21)	// 8-bit register to read system info data window
#define ZG_SYS_INFO_IDX_REG			(0x2b)
#define ZG_INTR2_REG				(0x2d)	// 16-bit register containing interrupt bits
#define ZG_INTR2_MASK_REG			(0x2e)	// 16-bit register containing interrupt mask
#define ZG_BYTE_COUNT_REG			(0x2f)	// 16-bit register containing available write size for fifo0
#define ZG_BYTE_COUNT_FIFO0_REG		(0x33)	// 16-bit register containing bytes ready to read on fifo0
#define ZG_BYTE_COUNT_FIFO1_REG		(0x35)	// 16-bit register containing bytes ready to read on fifo1
#define ZG_PWR_CTRL_REG				(0x3d)	// 16-bit register used to control low power mode
#define ZG_INDEX_ADDR_REG			(0x3e)	// 16-bit register to move the data window
#define ZG_INDEX_DATA_REG			(0x3f)	// 16-bit register to read the address in the ZG_INDEX_ADDR_REG

#define ZG_INTR_REG_LEN				(1) 
#define ZG_INTR_MASK_REG_LEN		(1)
#define ZG_SYS_INFO_DATA_REG_LEN	(1)
#define ZG_SYS_INFO_IDX_REG_LEN		(2)
#define ZG_INTR2_REG_LEN			(2)
#define ZG_INTR2_MASK_REG_LEN		(2)
#define ZG_BYTE_COUNT_REG_LEN		(2)
#define ZG_BYTE_COUNT_FIFO0_REG_LEN	(2)
#define ZG_BYTE_COUNT_FIFO1_REG_LEN	(2)
#define ZG_PWR_CTRL_REG_LEN			(2)
#define ZG_INDEX_ADDR_REG_LEN		(2)
#define ZG_INDEX_DATA_REG_LEN		(2)

// Registers accessed through ZG_INDEX_ADDR_REG
#define ZG_RESET_STATUS_REG			(0x2a)	// 16-bit read only register providing HW status bits
#define ZG_RESET_REG				(0x2e)	// 16-bit register used to initiate hard reset
#define ZG_PWR_STATUS_REG			(0x3e)	// 16-bit register read to determine when device
											// out of sleep state

#define ZG_RESET_MASK				(0x10)	// the first byte of the ZG_RESET_STATUS_REG
											// used to determine when the G2100 is in reset

#define ZG_ENABLE_LOW_PWR_MASK		(0x01)	// used by the Host to enable/disable sleep state
											// indicates to G2100 that the Host has completed
											// transactions and the device can go into sleep
											// state if possible

/*
 * Miscellaneous
 */
// valid fifo byte count bits
#define ZG_BYTE_COUNT_FIFO_MASK	(0x0fff)

// states for interrupt state machine
#define ZG_INTR_ST_RD_INTR_REG	(1)
#define ZG_INTR_ST_WT_INTR_REG	(2)
#define ZG_INTR_ST_RD_CTRL_REG	(3)

// interrupt state
#define ZG_INTR_DISABLE		((u8)0)
#define ZG_INTR_ENABLE		((u8)1)

// mask values for ZG_INTR_REG and ZG_INTR2_REG
#define	ZG_INTR_MASK_FIFO1		(0x80)
#define ZG_INTR_MASK_FIFO0		(0x40)
#define ZG_INTR_MASK_ALL		(0xff)
#define ZG_INTR2_MASK_ALL		(0xffff)
 
// DMA buffer size
#define ZG_BUFFER_SIZE		2400

// Header length of zg_tx_data_req_t
#define ZG_DATA_REQ_HDR		(2 + 2)		// Type (1) + Subtype (1) +
										// reqID (1) + reserved (1)
										

// Types of 802.11 Authentication algorithms
enum {
	ZG_AUTH_ALG_OPEN = 0,  /* used for everything */
	ZG_AUTH_ALG_SHARED = 1 /* used for WEP only */
};

// 802.11 Capability Info bits
#define ZG_CAP_BIT_BSS				(0x01)	// infrastructure network
#define ZG_CAP_BIT_IBSS				(0x02)	// adhoc network
#define ZG_CAP_BIT_PRIVACY			(0x10)	// security enabled
#define ZG_CAP_BIT_SHORT_PREAMBLE	(0x20)	// using short preamble
#define ZG_CAP_BIT_MASK				(0x33)

// Security Info bits
// WPA/RSN authentication bits
#define ZG_SEC_INFO_AUTH_OTHER		(0x8000)
#define ZG_SEC_INFO_AUTH_8021X		(0x4000)
#define ZG_SEC_INFO_AUTH_PSK		(0x2000)
// WPA/RSN unicast/pairwise cipher bits
#define ZG_SEC_INFO_UNICAST_OTHER	(0x1000)
#define ZG_SEC_INFO_UNICAST_WEP104	(0x0800)
#define ZG_SEC_INFO_UNICAST_WEP40	(0x0400)
#define ZG_SEC_INFO_UNICAST_CCMP	(0x0200)
#define ZG_SEC_INFO_UNICAST_TKIP	(0x0100)
// WPA/RSN group/multicast cipher bits
#define ZG_SEC_INFO_GROUP_OTHER		(0x0010)
#define ZG_SEC_INFO_GROUP_WEP104	(0x0008)
#define ZG_SEC_INFO_GROUP_WEP40		(0x0004)
#define ZG_SEC_INFO_GROUP_CCMP		(0x0002)
#define ZG_SEC_INFO_GROUP_TKIP		(0x0001)

// Parameters used for Scan operation
// Types of networks
#define ZG_BSS_INFRA		(1)    // infrastructure only
#define ZG_BSS_ADHOC		(2)    // Ad-hoc only (ibss)
#define ZG_BSS_ANY			(3)    // infrastructure or independant (Ad-Hoc)

// Type of scan operation
#define ZG_SCAN_TYPE_ACTIVE		(1)	// Active Scan (uses probe requests)
#define ZG_SCAN_TYPE_PASSIVE	(2)	// Passive Scan (probe requests not used)

const u16 scan_probe_delay = 20;		// units microseconds
const u16 scan_min_chan_time = 400;		// units of 1024 microseconds
										// minimum time spent listening on a channel
const u16 scan_max_chan_time = 800;		// units of 1024 microseconds
										// maximum time spent listening on a channel

// Parameters for Join operation
#define ZG_JOIN_TIMEOUT		(100)	// units of beacon intervals

// Parameters for Authentication operation
#define ZG_AUTH_TIMEOUT		(50)	// units of 10 msec

// Parameters for Association operation
#define ZG_ASSOC_TIMEOUT	(50)	// units of 10 msec

// Parameters for Adhoc network
#define ZG_ADHOC_JOIN_TIMEOUT	(100)	// units of beacon intervals
#define ZG_ADHOC_BEACON_PERIOD	(100)	// units of milliseconds

// Security info element
// used to request appropriate IEs to be appended to association request
#define ZG_SEC_INFO_WPA_PSK_TKIP		(0x01)
#define ZG_SEC_INFO_WPA_PSK_CCMP		(0x02)
#define ZG_SEC_INFO_RSN_PSK_TKIP		(0x10)
#define ZG_SEC_INFO_RSN_PSK_CCMP		(0x20)

// Slots used to store security keys
enum {
    ZG_SEC_KEY_SLOT_PMK0 = 0,
    ZG_SEC_KEY_SLOT_PMK1,
    ZG_SEC_KEY_SLOT_PMK2,
    ZG_SEC_KEY_SLOT_WEP_DEF
};

// Parameters for Adhoc start operation
#define ZG_MAX_NUM_RATES	(8)	// The maximum number of supported data rates

// Parameters for Power mode
enum {
	ZG_PWR_MODE_SAVE = 0,
	ZG_PWR_MODE_ACTIVE
};

// Parameters for Retry limit
enum {
	ZG_RETRY_LIMIT_LONG = 1,
	ZG_RETRY_LIMIT_SHORT
};

// Offset in the system info block
#define ZG_SYS_INFO_OFFSET_RX_CONT_SIZE		(100)
#define ZG_SYS_INFO_LEN_RX_CONT_SIZE		(2)

static const struct {
	u8 cnf_val;
	char *cnf_name;
} confirm_result[] = {	{ 1,	"Success" },
						{ 2,	"Invalid subtype" },
						{ 3,	"Cancelled" },
						{ 4,	"Frame Eol" },
						{ 5,	"Frame Retry Limit" },
						{ 6,	"Not associated" },
						{ 7,	"Frame too big" },
						{ 8,	"Frame encrypt failure" },
						{ 9,	"Invalid parameter" },
						{ 10,	"Already auth" },
						{ 11,	"Already assoc" },
						{ 12,	"Insufficient resources" },
						{ 13,	"Timeout" },
						{ 14,	"Bad exchange" },
						{ 15,	"Auth refused" },
						{ 16,	"Assoc refused" },
						{ 17,	"Request in progress" },
						{ 18,	"Not joined" },
						{ 19,	"Not assoc" },
						{ 20,	"Not auth" },
						{ 21,	"Supplicant failed" },
						{ 22,	"Unsupported feature" },
						{ 23,	"Request out of sync" }
					 };

static const struct {
	u8 reg_domain;
	u8 min, max;
	char *name;
} channel_table[] = {	{ ZG_REG_DOMAIN_FCC,		1,	11,	"USA" },
		      			{ ZG_REG_DOMAIN_IC,			1,	11,	"Canada" },
		      			{ ZG_REG_DOMAIN_ETSI,		1,	13,	"Europe" },
		      			{ ZG_REG_DOMAIN_SPAIN,		10,	11,	"Spain" },
		      			{ ZG_REG_DOMAIN_FRANCE,		10,	13,	"France" },
		      			{ ZG_REG_DOMAIN_JAPAN_A,	14,	14,	"Japan A" },
		      			{ ZG_REG_DOMAIN_JAPAN_B,	1,	13,	"Japan B" }
		      		};

static const long frequency_list[] = {	2412, 2417, 2422, 2427, 2432, 2437,
										2442, 2447, 2452, 2457, 2462, 2467,
										2472, 2484
									 };

enum {
	ZG_DO_NOT_TX_DEAUTH = 0,
	ZG_TX_DEAUTH
};

// Used as an indication from host to G2100 that all transactions are complete
// and the device can go into sleep state if possible
enum {
	ZG_SLEEP_DISABLE = 0,
	ZG_SLEEP_ENABLE
};

// LLC SNAP header
#define ZG_LLC_DEST_SAP		ETH_ALEN
#define ZG_LLC_SRC_SAP		ZG_LLC_DEST_SAP + 1
#define ZG_LLC_CMD			ZG_LLC_SRC_SAP + 1
#define ZG_LLC_VENDOR0		ZG_LLC_CMD + 1
#define ZG_LLC_VENDOR1		ZG_LLC_VENDOR0 + 1
#define ZG_LLC_VENDOR2		ZG_LLC_VENDOR1 + 1

/******************************************************************************
 * Structures
 *****************************************************************************/

/*
 * G2100 private structure
 */
struct zg_private {
	//SPI Messages
	struct spi_transfer	xfer[1];
	struct spi_message	msg[10];
	int msg_idx;

	struct net_device	*dev;
	struct spi_device	*spi;
	struct iw_statistics	wstats;		// wireless stats
	struct net_device_stats	stats;		// network device stats

	u8 *tx_buf;
	u8 *rx_buf;
	u16 tx_len;
	u16 rx_len;

	// DMA buffers
	dma_addr_t tx_dma;
	dma_addr_t rx_dma;
	void *tx_dma_buf;
	void *rx_dma_buf;

	// pointer to socket buffer
	struct sk_buff *skb;

	// to limit access to local buffers
	spinlock_t lock;
	u8 device_busy;

	// for synchronization
	struct completion done;
	struct completion spi_done;

	// interrupt stuff
	struct work_struct zg_int_wrk_q;

	// associate and maintain
	struct work_struct zg_conn_wrk_q;

	// data transfer management
	struct work_struct zg_data_wrk_q;

	// management requests
	struct work_struct zg_mgmt_wrk_q;

	struct {
		u8 *cmd_buf;		// for transmits
		u8 *tx_reg_buf;
		u8 *status_buf;		// for receive
		u8 *rx_reg_buf;

		u8 *tx_msg_buf;			// for message exchange
		u8 *rx_msg_buf;

		u8 wait_for_cnf;	// waiting for confirm
		u8 wait_for_sp_cnf;	// waiting for confirm from set_param

		u8 int_state;		// for interrupt service state machine
		u8 host_int;
		u8 next_cmd;
		u16 next_len;
		u16 rx_byte_cnt;
	} com_cxt;

	// config parameters
	u32 iw_mode;	// wireless mode : infrastructure or adhoc
	u8 essid[IW_ESSID_MAX_SIZE], desired_essid[IW_ESSID_MAX_SIZE];
 	u8 essid_len, d_essid_len;
	u8 bssid[ETH_ALEN], desired_bssid[ETH_ALEN];
	u8 new_bssid;
	u8 channel;
	u8 reg_domain;
	u8 preamble;			// preamble type
	u16 beacon_period;
	u8 cap_info[2];			// capability info
	u16	security_info[2];

	u16 rts_threshold;
	u16 frag_threshold;
	u8 long_retry, short_retry;		// retry limit

	// data rates
	u8 tx_rate;
	u8 auto_tx_rate;

	// power management
	u8 ps_mode;					// power save mode
	u8 sleep_state;				// sleep state
	u8 desired_ps_mode;
	u16 ps_wakeup_interval;		// units of ms

	// security stuff
	u8 security_type;
	u8 configured_security_type;

	u8 wep_keys[ZG_MAX_ENCRYPTION_KEYS][ZG_MAX_ENCRYPTION_KEY_SIZE];
	int wep_key_len[ZG_MAX_ENCRYPTION_KEYS];
	u8 default_key;

	u8 wpa_passphrase[ZG_MAX_WPA_PASSPHRASE_LEN];
	u8 wpa_passphrase_len;
	u8 wpa_psk_key[32];

	struct _bss_info {
		u8 channel;
		u8 ssid_len;
		u8 rssi;
		u8 security_enabled;
		u16 beacon_period;
		u8 preamble;
		u8 bss_type;
		u8 bssid[6];
		u8 ssid[ZG_MAX_SSID_LENGTH];
	} bss_info[ZG_MAX_BSS_ENTRIES];

	int num_bss_entries;
	u8 bss_list_valid;

	u8 scan_notify;			// send scan results to user
	u8 specific_essid;		// looking for specific network

	// state info
	enum {
		STATION_STATE_DOWN,
		STATION_STATE_NOT_ASSOCIATED,
		STATION_STATE_SCANNING,
		STATION_STATE_SCAN_DONE,
		STATION_STATE_SECURE,
		STATION_STATE_CALC_PSK_DONE,
		STATION_STATE_SECURE_DONE,
		STATION_STATE_JOINING,
		STATION_STATE_JOINING_ADHOC,
		STATION_STATE_STARTING_ADHOC,
		STATION_STATE_JOIN_DONE,
		STATION_STATE_AUTHENTICATING,
		STATION_STATE_AUTHENTICATE_DONE,
		STATION_STATE_ASSOCIATING,
		STATION_STATE_MANAGE_DONE,
		STATION_STATE_ASSOCIATE_DONE,
		STATION_STATE_JOIN_ADHOC_DONE,
		STATION_STATE_START_ADHOC_DONE,
		STATION_STATE_PS_MODE,
		STATION_STATE_PS_MODE_DONE,
		STATION_STATE_READY,
	} station_state;

	enum {
		CHIP_STATE_DOWN,
		CHIP_STATE_RESET_DONE,
		CHIP_STATE_INIT_DONE,
		CHIP_STATE_READY,
	} chip_state;

	// chip info
	u16 chip_version;

	// multicast support
	u8 mc_list[4][6];
	u8 mc_list_count;
	u8 mc_list_idx;
	spinlock_t mc_lock;
	u8 mc_running;

	// MAC stat counters
	// maintained by device
	struct {
		u32 WEP_exclude;
		u32 tx_bytes;
		u32 tx_multicast;
		u32 tx_failed;
		u32 tx_retry;
		u32 tx_multiple_retry;
		u32 tx_success;
		u32 rx_dup;
		u32 rx_cts_success;
		u32 rx_cts_fail;
		u32 rx_ack_fail;
		u32 rx_bytes;
		u32 rx_frag;
		u32 rx_multicast;
		u32 rx_FCS_error;
		u32 rx_WEP_undecrypt;
		u32 rx_frag_aged;
		u32 rx_MIC_failure;
	} stat_cntr;

	// misc
	u8 disconn_tx_frame;		// transmit disconnect frame
	u8 reconn_on_disconn;		// try reconnecting to AP if disconnected
	u8 adhoc_role;				// role of the device in adhoc mode
	u8 auth_mode;				// authentication mode, open or shared
	u8 trial_num;				// trial number of authentication process
	u8 data_req_id;
	u8 disconnect;				// disconnect from AP
	u8 conn_trial;				// trial number of connection attempt

	u16 fifo_byte_cnt;			// holds available write buffer space on device
	u16 write_buffer_size;		// size of buffers

	u8 txpacket_waiting;		// indicates if a packet is waiting for TX

	// management commands
	struct {
		u8 cmd_name;
		u8 param_cmd;
		u16 cmd_len;
		u8 cmd[256];
		u8 stack_stopped;
	}mgmt_cmd;
};

typedef struct 
{
    u8        bssid[ETH_ALEN];
    u8        ssid[ZG_MAX_SSID_LENGTH];
    u16       cap_info;
    u16       beacon_period;
    u16       atim_window;			/* only valid if .bssType == Ibss */
    u8        basic_rate_set[ZG_MAX_NUM_RATES];
    u8        rssi;					/* sig strength of RX beacon, probe resp */
    u8        num_rates;			/* num entries in basic_rate_set */
    u8        dtim_period;
    u8        bss_type;
    u8        channel;
    u8        ssid_len;
} zg_bss_desc_t;	/* 58 bytes */

#define ZG_BSS_DESC_SIZE	(58)

typedef struct {
	u16	probe_delay;
	u16 min_channel_time;
	u16 max_channel_time;
	u8 bssid[6];
	u8 bss;
	u8 scan_type;
	u8 ssid_len;
	u8 channel_len;
	u8 ssid[32];
	u8 channel_list[14];
} zg_scan_req_t;

#define ZG_SCAN_REQ_SIZE	(62)

/* The structure used to return the scan results back to the host system */
typedef struct
{
    u8    result;			/* the result of the scan */
    u8    reserved;			/* alignment byte */
    u8    last_channel;		/* the last channel scanned */
    u8    num_bss_desc;		/* The number of tZGMACBssDesc objects that 
                             * immediately follows this structure in memory */
} zg_scan_result_t; /* 4 bytes */

#define ZG_SCAN_RESULT_SIZE	(4)

typedef struct {
    	u16 to;					  /* timeout */
    	u16 beaconPeriod;         /* beacon period in units of TU's (1024 usec) */
    	u8 bssid[ETH_ALEN];		  /* the bssid of the target */
    	u8 channel;               /* the 2.4 GHz channel alias values 1 thru 14 */
    	u8 ssidLen;               /* num valid bytes in ssid */
    	u8 ssid[ZG_MAX_SSID_LENGTH];   /* the ssid of the target */
} zg_join_req_t;

#define ZG_JOIN_REQ_SIZE	(6 + 6 + 32)

typedef struct
{
    u8	addr[ETH_ALEN];    /* the BSSID of the network with which to authenticate */
    u16	to;                /* authentication timeout in 10's of msec */
    u8	alg;               /* auth algorithm; open or shared key */
    u8	reserved;          /* pad byte */
} zg_auth_req_t;

#define ZG_AUTH_REQ_SIZE	(4 + 6)

typedef struct
{
    u8 addr[ETH_ALEN];	/* addr of the AP that will receive the assoc frame */
    u8 channel;			/* channel 1-14 only required for re-association */
    u8 secReq;			/* security selection bits to instruct construction
    					 * of a security IE */ 
    u16 to;				/* association timeout in 10's of msec */
    u16 capInfo;		/* the capabilities of this station. They must 
						 * coincide with what the AP publishes in its beacon */
    u16 listenIntval;	/* the number of beacons that may pass between 
						 * listen attempts by this Station. Indicates to 
						 * the AP how how much resources may be required to 
						 * buffer for this station while its sleeps */
    u16 elemLen;		/* length of additional elements to be included in 
						 * the assoc frame that immediately follows this
						 * data structure */
} zg_assoc_req_t;

#define ZG_ASSOC_REQ_SIZE	(10 + 6)

/* Represents the input parameters required to 
 * conduct a Network Disconnect operation */
typedef struct
{
    u16 reasonCode;		/* specifies the reason for transmitting the 
						 * deauthentication and is included in the Deauth frame
						 * that gets sent to the AP. */
    u8    disconnect;	/* boolean if non-zero the MAC will enter into the 
						 * Idle State upon completion of the request, otherwise
						 * the MAC will enter into the Joined State and 
						 * continue monitoring network beacons.*/
    u8    txFrame;		/* boolean if non-zero the MAC will transmit a deauth
						 * frame to the network. */
} zg_disconnect_req_t; /* 4 bytes */

#define ZG_DISCONNECT_REQ_SIZE	(4)

/* Represents the input parameters required to 
 * conduct a Network Data Transmit operation */
typedef struct
{
    u8	reqID;
    u8	reserved;		/* 1-byte reserved for driver use */
    u8	da[ETH_ALEN];	/* The destination MAC address for the frame. */
    /* the data to be transmitted as the payload immediately
     * follows this structure in memory. */
} zg_tx_data_req_t;

/* The purpose of this data structure is to indicate that a peer Station or AP 
 * has sent a 802.11 DATA frame to this device. */
typedef struct
{
    u16        rssi;				/* the value of the G2100 RSSI when the data frame was received */
    u8         dstAddr[ETH_ALEN];	/* MAC Address to which the data frame was directed. */
    u8         srcAddr[ETH_ALEN];	/* MAC Address of the Station that sent the Data frame. */
    u16        arrivalTime_th;		/* the value of the 32-bit G2100 system clock when the frame arrived */
    u16        arrivalTime_bh;
    u16        dataLen;				/* the length in bytes of the payload which immediately follows this data structure */
} zg_rx_data_ind_t; /* 20 bytes */

#define ZG_RX_DATA_IND_SIZE		(20)

typedef struct
{
    u8 configBits;	/* used to dictate MAC behavior following the calculation */
    u8 phraseLen;	/* number of valid bytes in passphrase */
    u8 ssidLen;		/* number of valid bytes in ssid */
    u8 reserved;	/* alignment byte */
    u8 ssid[ZG_MAX_SSID_LENGTH];	/* the string of characters representing the ssid */
    u8 passPhrase[ZG_MAX_WPA_PASSPHRASE_LEN]; /* the string of characters representing the passphrase */
} zg_psk_calc_req_t;

#define ZG_PSK_CALC_REQ_SIZE	(4 + ZG_MAX_SSID_LENGTH + ZG_MAX_WPA_PASSPHRASE_LEN) /* 100 bytes */

typedef struct
{
    u8 slot;	/* slot index */
    u8 keyLen;
    u8 defID;	/* the default wep key id */
    u8 ssidLen;	/* num valid bytes in ssid */
    u8 ssid[ZG_MAX_SSID_LENGTH];	/* ssid of network */
    u8 key[ZG_MAX_ENCRYPTION_KEYS][ZG_MAX_ENCRYPTION_KEY_SIZE];	/* wep key data for 4 default keys */
} zg_wep_key_req_t;

#define ZG_WEP_KEY_REQ_SIZE		(4 + ZG_MAX_SSID_LENGTH + ZG_MAX_ENCRYPTION_KEYS*ZG_MAX_ENCRYPTION_KEY_SIZE) 

typedef struct
{
    u8 slot;
    u8 ssidLen;
    u8 ssid[ZG_MAX_SSID_LENGTH];
    u8 keyData[ZG_MAX_PMK_LEN];    
} zg_pmk_key_req_t;

#define ZG_PMK_KEY_REQ_SIZE		(2 + ZG_MAX_SSID_LENGTH + ZG_MAX_PMK_LEN)

typedef struct 
{
    u16 timeout;	/* the timeout in units of beacon intervals. If the STA
					 * cannot successfully connect to the adhoc network 
					 * within the specified timeout the device will fail the
					 * request. */
    u16 beaconPrd;	/* beacon period in units of TU's (1024 usec) */
    u8 bssid[ETH_ALEN];	/* the bssid of the target */
    u8 channel;		/* the 2.4 GHz channel alias values 1 thru 15 */
    u8 ssidLen;		/* num valid bytes in ssid */
    u8 ssid[ZG_MAX_SSID_LENGTH];	/* the ssid of the target */
} zg_adhoc_connect_req_t;

#define ZG_ADHOC_CONNECT_REQ_SIZE	(44)

typedef struct 
{
    u8 channel;
    u8 ssidLen;		/* num valid bytes in ssid */
    u8 dataRateLen;	/* num valid data rates */
    u8 reserved;	/* padding byte */
    u16 beaconPrd;	/* units of TU */
    u16 capInfo;	/* Cap Info must equal 0x3200 | 0x2200 | 0x1200 | 0x0200 */
    u8 dataRates[ZG_MAX_NUM_RATES];	/* the set of data rates published in the beacon. */ 
    u8 ssid[ZG_MAX_SSID_LENGTH];
} zg_adhoc_start_req_t;    /* 48 bytes */

#define ZG_ADHOC_START_REQ_SIZE		(48)

typedef struct
{
    u8 mode;		/* active or power save */
    u8 wake;		/* wake state */
    u8 rcvDtims;	/* wake up to receive DTIMs */
    u8 reserved;	/* pad byte */
} zg_pwr_mode_req_t;

#define ZG_PWR_MODE_REQ_SIZE		(4)

typedef struct
{
	u8 secType;		/* security type : 0 - none; 1 - wep; 2 - wpa; 3 - wpa2; 0xff - best available */
    u8 ssidLen;		/* num valid bytes in ssid */
    u8 ssid[ZG_MAX_SSID_LENGTH];	/* the ssid of the target */
    u16 sleepDuration;	/* power save sleep duration in units of 100 milliseconds */
    u8 modeBss;			/* 1 - infra; 2 - adhoc */
    u8 reserved;
} zg_connect_req_t;

#define ZG_CONNECT_REQ_SIZE			(38)

typedef struct
{
	u8 connManageState;	/* enable/disable connection manager */
    u8 numRetries;		/* number of retries to connect to a network */
    u8 flags;			/* enable/disable indications, reconnection reason */
    u8 reserved;
} zg_connect_manage_req_t;

#define ZG_CONNECT_MANAGE_REQ_SIZE	(4)

typedef struct
{
    u8 result;
    u8 reserved;
    u16 capInfo;
    u16 securityInfo[2];
} zg_join_cnf_t;

typedef struct
{
    u8 result;		/* indicating success or other */
    u8 macState;	/* current State of the on-chip MAC */
    u8 keyReturned;	/* 1 if psk contains key data, 0 otherwise */
    u8 reserved;	/* pad byte */
    u8 psk[ZG_MAX_PMK_LEN];	/* the psk bytes */
} zg_psk_calc_cnf_t;

#define ZG_PSK_CALC_CNF_SIZE		(4 + ZG_MAX_PMK_LEN) /* 36 bytes */

typedef struct
{
    u8 result;		/* indicating success or other */
    u8 macState;	/* state of the MAC */
    u8 secType;		/* security type used for the current connection
    				 * 0x00 - none, 0x01 - WEP, 0x02 - WPA, 0x03 - WPA2 */
    u8 reserved;
} zg_connect_cnf_t;

/******************************************************************************
 * Globals
 *****************************************************************************/ 
int zg_debug_level = 0;

/*
 * Driver parameters
 */
static char modparam_essid[32] = "IEEE";
module_param_string(essid, modparam_essid, 32, 0444);
MODULE_PARM_DESC(essid, "Specific ESSID parameter");

static int modparam_essid_len = 4;
module_param_named(essid_len, modparam_essid_len, int, 0444);
MODULE_PARM_DESC(essid_len, "Specific ESSID length");

static char modparam_passphrase[64] = "12345678";
module_param_string(passphrase, modparam_passphrase, 64, 0444);
MODULE_PARM_DESC(passphrase, "WPA passphrase parameter");

static int modparam_passphrase_len = 8;
module_param_named(passphrase_len, modparam_passphrase_len, int, 0444);
MODULE_PARM_DESC(passphrase_len, "WPA passphrase length");

static int modparam_configured_security = ZG_SECURITY_TYPE_NONE;
module_param_named(configured_security, modparam_configured_security, int, 0444);
MODULE_PARM_DESC(configured_security, "Configured security type");

/******************************************************************************
 * Function prototypes
 *****************************************************************************/

static int zg_open (struct net_device *dev);
static int zg_close(struct net_device *dev);
static int zg_change_mtu(struct net_device *dev, int new_mtu);
static int zg_set_mac_address(struct net_device *dev, void *p);
static int start_tx(struct sk_buff *skb, struct net_device *dev);
static struct net_device_stats *zg_get_stats(struct net_device *dev);
static int zg_ioctl(struct net_device *dev, struct ifreq *rq, int cmd);
static irqreturn_t zg_interrupt(int irq, void *handle);
static inline int zg_lock(struct zg_private *priv);
static inline void zg_unlock(struct zg_private *priv);
static bool zg_txrx(struct zg_private *priv);
static bool zg_read_reg(struct zg_private *priv, u8 reg, u8 len);
static bool zg_write_reg(struct zg_private *priv, u8 reg, u8 len);
static bool zg_write_cmd(struct zg_private *priv, u8 cmd);
static bool zg_hw_reset(struct net_device *dev);
static bool zg_chip_reset(struct zg_private *priv);
static bool zg_interrupt2_reg(struct zg_private *priv, u16 mask, u8 state);
static bool zg_interrupt_reg(struct zg_private *priv, u16 mask, u8 state);
static void zg_interrupt_bh (struct work_struct *work);
static bool zg_send_mgmt_req(struct zg_private *priv, u8 command, u8 param_cmd, u16 cmd_size);
static void zg_conn_handler (struct work_struct *work);
static void zg_scan(struct zg_private *priv, int specific_ssid, int bss_type);
static void zg_store_bss_info(struct zg_private *priv);
static void zg_send_event(struct zg_private *priv);
static void zg_join(struct zg_private *priv);
static void zg_authenticate(struct zg_private *priv, int trial_num);
static void zg_associate(struct zg_private *priv);
static void zg_disconnect(struct zg_private *priv, u8 tx_frame);
static void zg_data_handler (struct work_struct *work);
static void zg_send_data_req(struct zg_private *priv, int command, int cmd_size);
static void zg_process_rx_data(struct zg_private *priv);
static void zg_txrx_complete(void *arg);
static void zg_set_txrate_onoff(struct zg_private *priv, u8 value);
static void zg_get_chip_version(struct zg_private *priv);
static void zg_calc_psk_key(struct zg_private *priv);
static void zg_write_wep_key(struct zg_private *priv);
static void zg_setup_security(struct zg_private *priv);
static bool zg_setup_ap_params(struct zg_private *priv, int specific_ssid);
static void  zg_set_beacon_miss_thr(struct zg_private *priv, u8 value);
static void zg_write_psk_key(struct zg_private *priv);
static void zg_join_adhoc(struct zg_private *priv);
static void zg_start_adhoc(struct zg_private *priv);
static void zg_set_power_mode(struct zg_private *priv);
static void zg_set_mcast_address(struct zg_private *priv, u8 index);
static void zg_set_multicast_list(struct net_device *dev);
static void zg_set_retry_limit(struct zg_private *priv, u8 type, u8 value);
static void zg_set_rts_threshold(struct zg_private *priv, u16 value);
static void zg_set_sleep_state(struct zg_private *priv, u8 state);
static void zg_get_stat_counters(struct zg_private *priv);
static void zg_read_fifo_byte_cnt(struct zg_private *priv);
static void zg_read_chip_info_block(struct zg_private *priv);
static void zg_mgmt_handler (struct work_struct *work);
static void zg_get_mac_address(struct zg_private *priv);
static void zg_connect(struct zg_private *priv, u8 sec_type);
static void zg_connect_manage(struct zg_private *priv);


#endif /*G2100_H_*/
