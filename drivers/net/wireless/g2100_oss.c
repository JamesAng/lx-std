/******************************************************************************

  Driver for the ZeroG Wireless G2100 series wireless devices.

  Copyright(c) 2008 ZeroG Wireless, Inc. All rights reserved.

  Confidential and proprietary software of ZeroG Wireless, Inc.
  Do no copy, forward or distribute.

******************************************************************************/

#include "g2100_oss.h"

/*
 * FUNCTION	:	zg_open
 *
 * NOTES	:	Takes the device through the process of connecting to a network
 * 				First function called after initializing the HW and network
 * 				interface initialization
 */
int zg_open (struct net_device *dev)
{
	struct zg_private *priv = netdev_priv(dev);

	// device not ready
	netif_carrier_off(priv->dev);

	// send disconnect frame to AP when disassociating
	priv->disconn_tx_frame = ZG_TX_DEAUTH;

	// refresh available buffer space on device
	zg_read_fifo_byte_cnt(priv);

	// reset data id
	// currently used just for house-keeping
	priv->data_req_id = 0;

	// scan for APs and try associating
	if ( !schedule_work(&priv->zg_conn_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");

	return 0;
}

/*
 * FUNCTION	:	zg_close
 *
 * NOTES	:	Called when the network interface is closed
 */
static int zg_close(struct net_device *dev)
{
	struct zg_private *priv = netdev_priv(dev);

	// Send event to userspace that we are disassociating
	if (priv->station_state == STATION_STATE_READY) {
		union iwreq_data wrqu;

		wrqu.data.length = 0;
		wrqu.data.flags = 0;
		wrqu.ap_addr.sa_family = ARPHRD_ETHER;
		memset(wrqu.ap_addr.sa_data, 0, ETH_ALEN);
		wireless_send_event(priv->dev, SIOCGIWAP, &wrqu, NULL);
	}

	// stop stack
	netif_carrier_off(priv->dev);
	if (netif_running(priv->dev))
		netif_stop_queue(priv->dev);

	// change station state
	priv->station_state = STATION_STATE_DOWN;

	return 0;
}

/*
 * FUNCTION	:	zg_change_mtu
 *
 * PARAMS	:
 *				dev	-		pointer to the network device structure
 * 				new_mtu -	new MTU value to be used for this network interface
 *
 * NOTES	:	To change the network MTU, default ethernet MTU = 1500 bytes
 */
static int zg_change_mtu(struct net_device *dev, int new_mtu)
{
	// check if the new value is valid
	if ( (new_mtu < 68) || (new_mtu > 2312) )
		return -EINVAL;

	// set the new value
	dev->mtu = new_mtu;
	return 0;
}

/*
 * FUNCTION	:	zg_set_mac_address
 *
 * NOTES	:	To change the HW MAC address of this network interface
 */
static int zg_set_mac_address(struct net_device *dev, void *p)
{
	// not supported
	return (-EOPNOTSUPP);
}

/*
 * FUNCTION	:	start_tx
 *
 * PARAMS	:
 *				skb	-	pointer to the socket buffer
 *				dev	-	pointer to the network device structure
 *
 * NOTES	:	Called by stack when it has data for transmission
 */
static int start_tx(struct sk_buff *skb, struct net_device *dev)
{
	struct zg_private *priv = netdev_priv(dev);
	struct net_device_stats *stats = &priv->stats;

	// verify if station is currently associated
	if ( priv->station_state != STATION_STATE_READY ) {
		ZG_PRINT(1, "%s: Tx on unassociated device!\n", dev->name);
		return NETDEV_TX_BUSY;
	}

	// verify if stack is enabled
	if ( !netif_running(dev) ) {
		ZG_PRINT(1, "%s: Tx on stopped device!\n", dev->name);
		return NETDEV_TX_BUSY;
	}

	if (netif_queue_stopped(dev)) {
		ZG_PRINT(1, "%s: Tx while transmitter busy!\n", dev->name);
		return NETDEV_TX_BUSY;
	}

	if ( !netif_carrier_ok(dev) ) {
		goto tx_drop;
	}

	// pause all traffic until the current packet is handled
	netif_stop_queue(dev);

	// save socket buffer and schedule data TX
	priv->skb = skb;
	if ( !schedule_work(&priv->zg_data_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_data_wrk_q failed\n");
	goto tx_ok;

tx_drop:
	// silently drops packet
	ZG_PRINT(1, "%s: Packet dropped!\n", dev->name);
	dev_kfree_skb(skb);
	stats->tx_errors++;
	stats->tx_dropped++;

tx_ok:
	return NETDEV_TX_OK;
}

/*
 * FUNCTION	:	zg_get_stats
 *
 * NOTES	:	Used to obtain network statistics from the network device
 */
static struct net_device_stats *zg_get_stats(struct net_device *dev)
{
	struct zg_private *priv = netdev_priv(dev);
	return &priv->stats;
}

/*
 * FUNCTION	:	zg_ioctl
 *
 * NOTES	:	Implements any IOCTL commands
 */
static int zg_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	return (-EOPNOTSUPP);
}

/*
 * FUNCTION	:	zg_interrupt
 *
 * NOTES	:	Interrupt handler
 */
static irqreturn_t zg_interrupt(int irq, void *handle)
{
	struct net_device *dev = (struct net_device *)handle;
	struct zg_private *priv = netdev_priv(dev);

	// do nothing if chip is not configured
	if (priv->chip_state == CHIP_STATE_DOWN)
		return IRQ_HANDLED;

	// disable interrupt
	disable_irq(ZG_INTERRUPT_PIN);

	// schedule bh
	if ( !schedule_work(&priv->zg_int_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_int_wrk_q failed\n");

	return IRQ_HANDLED;
}

/*
 * FUNCTION	:	zg_lock
 *
 * NOTES	:	Manages the device lock
 * 				Not to be used inside interrupt handlers
 */
static inline int zg_lock(struct zg_private *priv)
{
	spin_lock(&priv->lock);
	if (priv->device_busy) {
		ZG_PRINT(1, "zg_lock called when device_busy\n");
		spin_unlock(&priv->lock);
		return -EBUSY;
	}
	else {
		priv->device_busy++;
		spin_unlock(&priv->lock);
		return 0;
	}
}

/*
 * FUNCTION	:	zg_unlock
 *
 * NOTES	:	Manages the device lock
 * 				Not to be used inside interrupt handlers
 */
static inline void zg_unlock(struct zg_private *priv)
{
	spin_lock(&priv->lock);
	if (priv->device_busy) {
		priv->device_busy--;
		spin_unlock(&priv->lock);
	}
	else {
		ZG_PRINT(1, "zg_unlock called when not locked\n");
		spin_unlock(&priv->lock);
	}
}

/*
 * FUNCTION	:	zg_txrx
 *
 * NOTES	:	Manage SPI TX-RX operation
 */
static bool zg_txrx(struct zg_private *priv)
{
	struct spi_message	*m;
	struct spi_transfer	*x;
	int status;

	m = &priv->msg[priv->msg_idx];
	x = priv->xfer;

	memset(x, 0, sizeof x);
	spi_message_init(m);

	// setup SPI transfer parameters
	x->tx_buf = priv->tx_buf;
	x->rx_buf = priv->rx_buf;
	x->len = (priv->tx_len > priv->rx_len) ? priv->tx_len : priv->rx_len;
	x->tx_dma = priv->tx_dma;
	x->rx_dma = priv->rx_dma;
	x->cs_change = 1;	// toggle chip select; 1 : toggle, 0 : do not toggle
	spi_message_add_tail(x, m);

	INIT_COMPLETION(priv->spi_done);
	m->complete = zg_txrx_complete;		// completion callback function
	m->context = &priv->spi_done;		// callback argument
	m->is_dma_mapped = 1;

	// start SPI operation and wait for completion
	status = spi_async(priv->spi, m);
	if (status == 0)
		wait_for_completion_interruptible(&priv->spi_done);

	if (status) {
		ZG_PRINT(1, "spi_sync error\n");
		return 0;
	}

	// cycle through the SPI message buffers
	if ( priv->msg_idx < 9 )
		priv->msg_idx++;
	else
		priv->msg_idx = 0;

	return 1;
}

/*
 * FUNCTION	:	zg_read_reg
 *
 * PARAMS	:
 * 				reg -	register to read
 * 				len -	len of the register in bytes
 *
 * NOTES	:	Read a specific register
 */
static bool zg_read_reg(struct zg_private *priv, u8 reg, u8 len)
{
	priv->com_cxt.cmd_buf[0] = reg | ZG_CMD_RD_REG;
	priv->tx_len = ZG_PREAMBLE_CMD_LEN;
	priv->rx_len = ZG_PREAMBLE_STATUS_LEN + len;

	return zg_txrx(priv);
}

/*
 * FUNCTION	:	zg_write_reg
 *
 * PARAMS	:
 * 				reg -	register to read
 * 				len -	len of the register in bytes
 *
 * NOTES	:	Write a specific register
 */
static bool zg_write_reg(struct zg_private *priv, u8 reg, u8 len)
{
	priv->com_cxt.cmd_buf[0] = reg | ZG_CMD_WT_REG;
	priv->tx_len = ZG_PREAMBLE_CMD_LEN + len;
	priv->rx_len = ZG_PREAMBLE_STATUS_LEN;

	return zg_txrx(priv);
}

/*
 * FUNCTION	:	zg_write_cmd
 *
 * PARAMS	:
 * 				cmd -	command to write
 *
 * NOTES	:	Send a command to G2100
 */
static bool zg_write_cmd(struct zg_private *priv, u8 cmd)
{
	priv->com_cxt.cmd_buf[0] = cmd;
	priv->tx_len = ZG_PREAMBLE_CMD_LEN;
	priv->rx_len = 0;

	return zg_txrx(priv);
}

/*
 * FUNCTION	:	zg_hw_reset
 *
 * NOTES	:	Takes G2100 through a reset process
 */
static bool zg_hw_reset(struct net_device *dev)
{
	struct zg_private *priv = netdev_priv(dev);

	// Reset the chip
	if ( !zg_chip_reset(priv) ) {
		ZG_PRINT(1, "%s: chip reset failed\n", dev->name);
		return 0;
	}

	// Disable interrupts gated by intr2 register
	if ( !zg_interrupt2_reg(priv, ZG_INTR2_MASK_ALL, ZG_INTR_DISABLE) ) {
		ZG_PRINT(1, "%s: interrupt 2 register init failed\n", dev->name);
		return 0;
	}

	// Disable interrupts gated by intr register
	if ( !zg_interrupt_reg(priv, ZG_INTR_MASK_ALL, ZG_INTR_DISABLE) ) {
		ZG_PRINT(1, "%s: interrupt register init failed\n", dev->name);
		return 0;
	}

	// Enable the fifo interrupts
	if ( !zg_interrupt_reg(priv, ZG_INTR_MASK_FIFO1 | ZG_INTR_MASK_FIFO0, ZG_INTR_ENABLE) ) {
		ZG_PRINT(1, "%s: interrupt register init failed\n", dev->name);
		return 0;
	}

	return 1;
}

/*
 * FUNCTION	:	zg_chip_reset
 *
 * NOTES	:	Reset G2100
 * 				This function also implements a delay so that it will not
 * 				return until the G2100 is ready to receive messages again
 */
static bool zg_chip_reset(struct zg_private *priv)
{
	u8 loop = 0;

	// lock buffers
	if ( zg_lock(priv) != 0 )
		return 0;

	// this loop writes the necessary G2100 registers to start a reset
	do {
		priv->com_cxt.tx_reg_buf[0] = 0x00;
		priv->com_cxt.tx_reg_buf[1] = ZG_RESET_REG;
		zg_write_reg(priv, ZG_INDEX_ADDR_REG, ZG_INDEX_ADDR_REG_LEN);

		priv->com_cxt.tx_reg_buf[0] = (loop == 0) ? (0x80) : (0x0f);
		priv->com_cxt.tx_reg_buf[1] = 0xff;
		zg_write_reg(priv, ZG_INDEX_DATA_REG, ZG_INDEX_DATA_REG_LEN);
	} while(loop++ < 1);

	// after reset is started the host should poll a register
	// that indicates when the HW reset is complete
	priv->com_cxt.tx_reg_buf[0] = 0x00;
	priv->com_cxt.tx_reg_buf[1] = ZG_RESET_STATUS_REG;
	zg_write_reg(priv, ZG_INDEX_ADDR_REG, ZG_INDEX_ADDR_REG_LEN);

	// read status register to determine when the G2100 is no longer in reset
	do {
		priv->com_cxt.rx_reg_buf[0] = 0;
        priv->com_cxt.rx_reg_buf[1] = 0;
		zg_read_reg(priv, ZG_INDEX_DATA_REG, ZG_INDEX_DATA_REG_LEN);
	} while ( (priv->com_cxt.rx_reg_buf[0] & ZG_RESET_MASK) == 0 );

	// after G2100 comes out of reset, the chip must complete initialization
	// the following loop reads the byte count register which will be non-zero
	// when the device initialization has finished
	do {
		priv->com_cxt.rx_reg_buf[0] = 0;
        priv->com_cxt.rx_reg_buf[1] = 0;
		zg_read_reg(priv, ZG_BYTE_COUNT_REG, ZG_BYTE_COUNT_REG_LEN);
	} while ( (priv->com_cxt.rx_reg_buf[0] == 0) && (priv->com_cxt.rx_reg_buf[1] == 0) );

	// release buffers
	zg_unlock(priv);

	return 1;
}

/*
 * FUNCTION	:	zg_interrupt2_reg
 *
 * PARAMS	:
 * 				mask -	the bit mask to be modified
 * 				state -	ZG_INTR_DISABLE, ZG_INTR_ENABLE
 * 						disable implies clear the bits and enable sets the bits
 *
 * NOTES	:	Initializes the Interrupt2 register on the G2100 with the
 *				specified mask value either setting or clearing the mask
 * 				register as determined by the input parameter state
 */
static bool zg_interrupt2_reg(struct zg_private *priv, u16 mask, u8 state)
{
	u8 maskMSB;
	u8 maskLSB;

	// lock buffers
	if ( zg_lock(priv) != 0 )
		return 0;

	// read the interrupt2 mask register
	zg_read_reg(priv, ZG_INTR2_MASK_REG, ZG_INTR2_MASK_REG_LEN);

	maskMSB = ((u8)((mask & 0xff00) >> 8));
	maskLSB = ((u8)(mask & 0x00ff));

	// modify the interrupt mask value and re-write the value to the interrupt
	// mask register clearing the interrupt register first
	priv->com_cxt.tx_reg_buf[3] = (priv->com_cxt.rx_reg_buf[1] & (~maskLSB)) |
									((state == ZG_INTR_DISABLE)? 0 : maskLSB);
	priv->com_cxt.tx_reg_buf[2] = (priv->com_cxt.rx_reg_buf[0] & (~maskMSB)) |
									((state == ZG_INTR_DISABLE)? 0 : maskMSB);
	priv->com_cxt.tx_reg_buf[1] = maskLSB;
	priv->com_cxt.tx_reg_buf[0] = maskMSB;

	// write both registers with a single call as they are adjacent
	zg_write_reg(priv, ZG_INTR2_REG, ZG_INTR2_REG_LEN + ZG_INTR2_MASK_REG_LEN);

	// release buffers
	zg_unlock(priv);

	return 1;
}

/*
 * FUNCTION	:	zg_interrupt_reg
 *
 * PARAMS	:
 * 				mask -	the bit mask to be modified
 * 				state -	ZG_INTR_DISABLE, ZG_INTR_ENABLE
 * 						disable implies clear the bits and enable sets the bits
 *
 * NOTES	:	Initializes the Interrupt register on the G2100 with the
 *				specified mask value either setting or clearing the mask
 * 				register as determined by the input parameter state
 */
static bool zg_interrupt_reg(struct zg_private *priv, u16 mask, u8 state)
{
	// lock buffers
	if ( zg_lock(priv) != 0 )
		return 0;

	// read the interrupt register
	zg_read_reg(priv, ZG_INTR_MASK_REG, ZG_INTR_MASK_REG_LEN);

	// now regBuf[0] contains the current setting for the
	// interrupt mask register
	priv->com_cxt.tx_reg_buf[1] = (priv->com_cxt.rx_reg_buf[0] & (~mask)) |
									((state == ZG_INTR_DISABLE)? 0 : mask);

	// this is to clear any currently set interrupts of interest
	priv->com_cxt.tx_reg_buf[0] = mask;

	// write both registers with a single call as they are adjacent
	zg_write_reg(priv, ZG_INTR_REG, ZG_INTR_REG_LEN + ZG_INTR_MASK_REG_LEN);

	// release buffers
	zg_unlock(priv);

	return 1;
}

/*
 * FUNCTION	:	zg_interrupt_bh
 *
 * NOTES	:	Process the interrupt
 */
static void zg_interrupt_bh (struct work_struct *work)
{
	struct zg_private *priv = container_of(work, struct zg_private, zg_int_wrk_q);
	struct net_device *dev = priv->dev;
	u8 irq_handled = 0;
	u8 *buf;
	u16 status = 0;

	// lock buffers
	if ( zg_lock(priv) != 0 ) {
		if ( !schedule_work(&priv->zg_int_wrk_q) )
			ZG_PRINT(1, "schedule_work zg_int_wrk_q failed\n");
		return;
	}

	// if a previous TX packet waiting, schedule it for transmission
	if (priv->txpacket_waiting) {
		zg_read_fifo_byte_cnt(priv);

		if ( (priv->skb->len + ZG_DATA_REQ_HDR) < priv->fifo_byte_cnt ) {
			if ( !schedule_work(&priv->zg_data_wrk_q) )
				ZG_PRINT(1, "schedule_work zg_data_wrk_q failed\n");

			// reset
			priv->txpacket_waiting = 0;
		}
	}

	zg_read_reg(priv, ZG_INTR_REG, ZG_INTR_REG_LEN + ZG_INTR_MASK_REG_LEN);
	priv->com_cxt.int_state = ZG_INTR_ST_RD_INTR_REG;

	do {
		switch(priv->com_cxt.int_state) {
			case ZG_INTR_ST_RD_INTR_REG:
				priv->com_cxt.host_int = priv->com_cxt.rx_reg_buf[0] & priv->com_cxt.rx_reg_buf[1];

				if((priv->com_cxt.host_int & ZG_INTR_MASK_FIFO1) == ZG_INTR_MASK_FIFO1)
				{
					// clear this interrupt
					priv->com_cxt.tx_reg_buf[0] = ZG_INTR_MASK_FIFO1;
					zg_write_reg(priv, ZG_INTR_REG, 1);
					priv->com_cxt.int_state = ZG_INTR_ST_WT_INTR_REG;
					// prepare to read the control register
					priv->com_cxt.next_cmd = ZG_BYTE_COUNT_FIFO1_REG;
					priv->com_cxt.next_len = ZG_BYTE_COUNT_FIFO1_REG_LEN;
				}
				else if((priv->com_cxt.host_int & ZG_INTR_MASK_FIFO0) == ZG_INTR_MASK_FIFO0)
				{
					// clear this interrupt
					priv->com_cxt.tx_reg_buf[0] = ZG_INTR_MASK_FIFO0;
					zg_write_reg(priv, ZG_INTR_REG, 1);
					priv->com_cxt.int_state = ZG_INTR_ST_WT_INTR_REG;
					// prepare to read the control register
					priv->com_cxt.next_cmd = ZG_BYTE_COUNT_FIFO0_REG;
					priv->com_cxt.next_len = ZG_BYTE_COUNT_FIFO0_REG_LEN;
				}
				else if(priv->com_cxt.host_int)
				{
					// unhandled interrupt
					ZG_PRINT(1, "%s: unhandled interrupt\n", dev->name);
					irq_handled = 2;
				}
				else
				{
					// spurious interrupt
					ZG_PRINT(1, "%s: spurious interrupt\n", dev->name);
					irq_handled = 2;
				}
				break;
			case ZG_INTR_ST_WT_INTR_REG:
				// read the respective control register
				zg_read_reg(priv, priv->com_cxt.next_cmd, priv->com_cxt.next_len);
				priv->com_cxt.int_state = ZG_INTR_ST_RD_CTRL_REG;
				break;
			case ZG_INTR_ST_RD_CTRL_REG:
				// store the number of bytes in the rx message
				priv->com_cxt.rx_byte_cnt =
					(u16)((((u16)priv->com_cxt.rx_reg_buf[0] & 0x00ff) << 8) | ((u16)priv->com_cxt.rx_reg_buf[1] & 0x00ff)) & ZG_BYTE_COUNT_FIFO_MASK;

				priv->com_cxt.cmd_buf[0] = ZG_CMD_RD_FIFO;
				priv->tx_len = ZG_PREAMBLE_CMD_LEN;
				priv->rx_len = ZG_PREAMBLE_STATUS_LEN + priv->com_cxt.rx_byte_cnt;

				if ( zg_txrx(priv) ) {
					zg_write_cmd(priv, ZG_CMD_RD_FIFO_DONE);
				}

				irq_handled = 1;
				break;
		}
	} while (irq_handled == 0);

	enable_irq(ZG_INTERRUPT_PIN);

	// if spurious/unhandled interrupt
	if (irq_handled != 1)
		goto err_int_fail;

	// check the type
	switch(priv->rx_buf[ZG_PREAMBLE_TYPE_IDX]) {
		case ZG_MAC_TYPE_TXDATA_CONFIRM:		// TX Data Confirm received
			switch (priv->rx_buf[ZG_PREAMBLE_SUBTYPE_IDX]) {
				case ZG_MAC_SUBTYPE_TXDATA_REQ_STD:
					break;
				default:
					ZG_PRINT(1, "default subtype : ZG_MAC_TYPE_TXDATA_CONFIRM\n");
					break;
			}
			break;
		case ZG_MAC_TYPE_MGMT_CONFIRM:			// Management confirm received
			// start stack; stopped in mgmt_handler
			if (priv->station_state == STATION_STATE_READY && priv->mgmt_cmd.stack_stopped)
				if (netif_queue_stopped(priv->dev))
					netif_wake_queue(priv->dev);

			// check if we received the subtype that we were expecting
			if(priv->com_cxt.wait_for_cnf == priv->rx_buf[ZG_PREAMBLE_SUBTYPE_IDX]) {
				if ( priv->com_cxt.rx_msg_buf[ZG_MGMT_CONFIRM_RESULT_IDX] != ZG_RESULT_SUCCESS ) {
					ZG_PRINT(1, "Mgmt req %02x failed, result : %02x (%s)\n", priv->com_cxt.wait_for_cnf, priv->com_cxt.rx_msg_buf[ZG_MGMT_CONFIRM_RESULT_IDX], confirm_result[priv->com_cxt.rx_msg_buf[ZG_MGMT_CONFIRM_RESULT_IDX] - 1].cnf_name);

					switch (priv->com_cxt.rx_msg_buf[ZG_MGMT_CONFIRM_RESULT_IDX]) {
						case ZG_RESULT_AUTH_REFUSED:
							// to handle case where AP is using shared authentication
							// and we fail our first attempt using open authentication
							if (priv->auth_mode == ZG_AUTH_ALG_OPEN) {
								priv->trial_num = 2;		// cause zg_authenticate to use shared authentication
								priv->station_state = STATION_STATE_SECURE_DONE;
								if ( !schedule_work(&priv->zg_conn_wrk_q) )
									ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
							}
							break;
						default:
							break;
					}

					goto err_int_fail;
				}
			}
			else {
				ZG_PRINT(1, "Unexpected Mgmt confirm received; Wait : %02x, RX : %02x\n", priv->com_cxt.wait_for_cnf, priv->rx_buf[ZG_PREAMBLE_SUBTYPE_IDX]);
				goto err_int_fail;
			}

			switch (priv->rx_buf[ZG_PREAMBLE_SUBTYPE_IDX]) {
				case ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM:		// received confirm for a set parameter operation
					complete(&priv->done);

					switch (priv->com_cxt.wait_for_sp_cnf) {
						case ZG_PARAM_MULTICAST_ADDR:
							// loading multicast list to device
							// driver maintains a list of multicast addresses; list depth = 4
							if (priv->mc_list_idx < priv->mc_list_count) {
								zg_set_mcast_address(priv, priv->mc_list_idx);
								priv->mc_list_idx++;
							}

							// done loading multicast list, release lock
							if (priv->mc_list_idx == priv->mc_list_count) {

								spin_lock(&priv->mc_lock);
								if (priv->mc_running) {
									priv->mc_running--;
									spin_unlock(&priv->mc_lock);
								}
								else {
									ZG_PRINT(1, "mc_unlock called when not locked\n");
									spin_unlock(&priv->mc_lock);
								}

							}
							break;
						default:
							break;
					}

					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_GET_PARAM:		// received confirm for a get parameter operation
					switch (priv->com_cxt.wait_for_sp_cnf) {
						case ZG_PARAM_SYSTEM_VERSION:		// chip version info
							buf = &(priv->rx_buf[ZG_PREAMBLE_LEN]);
							priv->chip_version = (((u16) buf[ZG_GET_PARAM_CNF_SIZE])<<8) | buf[ZG_GET_PARAM_CNF_SIZE + 1];
							complete(&priv->done);
							break;
						case ZG_PARAM_STAT_COUNTER:		// statistics counters
							buf = &(priv->rx_buf[ZG_PREAMBLE_LEN]);

							if ((u8)buf[2] == (u8)sizeof(priv->stat_cntr))
								memcpy(&(priv->stat_cntr), &buf[ZG_GET_PARAM_CNF_SIZE], sizeof(priv->stat_cntr));
							else
								ZG_PRINT(1, "stat counter length mismatch : driver update needed\n");

							complete(&priv->done);

							break;
						case ZG_PARAM_MAC_ADDRESS:
							buf = &(priv->rx_buf[ZG_PREAMBLE_LEN]);
							
							// load MAC address from device
							dev->dev_addr[0] = buf[ZG_GET_PARAM_CNF_SIZE];
							dev->dev_addr[1] = buf[ZG_GET_PARAM_CNF_SIZE + 1];
							dev->dev_addr[2] = buf[ZG_GET_PARAM_CNF_SIZE + 2];
							dev->dev_addr[3] = buf[ZG_GET_PARAM_CNF_SIZE + 3];
							dev->dev_addr[4] = buf[ZG_GET_PARAM_CNF_SIZE + 4];
							dev->dev_addr[5] = buf[ZG_GET_PARAM_CNF_SIZE + 5];
							
							complete(&priv->done);
							break;
						case ZG_PARAM_REG_DOMAIN:
							buf = &(priv->rx_buf[ZG_PREAMBLE_LEN]);
							priv->reg_domain = buf[ZG_GET_PARAM_CNF_SIZE];
							complete(&priv->done);
							break;
						default:
							break;
					}

					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_SCAN:			// received confirm for a scan operation
					priv->num_bss_entries = 0;		// empty BSS entries
					zg_store_bss_info(priv);		// store network info returned by scan operation

					if (priv->scan_notify) {		// if scan was initiated by user, send notification to user
						priv->bss_list_valid = 1;
						zg_send_event(priv);
						priv->scan_notify = 0;
					}
					else {							// if scan was driver initiated to connect to an AP
						priv->station_state = STATION_STATE_SCAN_DONE;
						if ( !schedule_work(&priv->zg_conn_wrk_q) )
							ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					}

					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_JOIN:			// received confirm for a join operation
					// store capability info
					memcpy(priv->cap_info, &((zg_join_cnf_t*)priv->com_cxt.rx_msg_buf)->capInfo, 2);

					// store security info
					priv->security_info[0] = ((zg_join_cnf_t*)priv->com_cxt.rx_msg_buf)->securityInfo[0];
					priv->security_info[1] = ((zg_join_cnf_t*)priv->com_cxt.rx_msg_buf)->securityInfo[1];

					// Security enabled
					if(priv->cap_info[0] & ZG_CAP_BIT_PRIVACY) {
						// defaults to WPA2 if both available
						if(priv->security_info[1] & ZG_SEC_INFO_AUTH_PSK)
							priv->security_type = ZG_SECURITY_TYPE_WPA2;
						else if(priv->security_info[0] & ZG_SEC_INFO_AUTH_PSK)
							priv->security_type = ZG_SECURITY_TYPE_WPA;
						else
							priv->security_type = ZG_SECURITY_TYPE_WEP;
					}
					else {
						priv->security_type = ZG_SECURITY_TYPE_NONE;
					}

					// check if configured security type matches the available security on the AP
					if ( priv->security_type != priv->configured_security_type ) {
						ZG_PRINT(1, "configured security does not match available security\n");
						priv->station_state = STATION_STATE_NOT_ASSOCIATED;
						break;
					}

					priv->station_state = STATION_STATE_JOIN_DONE;
					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_AUTH:			// received confirm for authentication operation
					priv->station_state = STATION_STATE_AUTHENTICATE_DONE;
					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_ASSOC:			// received confirm for association operation
					priv->station_state = STATION_STATE_ASSOCIATE_DONE;
					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_DEAUTH:		// received confirm for disconnect operation
					priv->station_state = STATION_STATE_DOWN;
					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_CALC_PSK:			// received confirm for PSK calculation operation
					memcpy(priv->wpa_psk_key, ((zg_psk_calc_cnf_t*)priv->com_cxt.rx_msg_buf)->psk, 32);

					priv->station_state = STATION_STATE_CALC_PSK_DONE;
					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_WEP_KEY:			// received confirm for WEP key write operation
					priv->station_state = STATION_STATE_SECURE_DONE;
					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_PMK_KEY:			// recieved confirm for PSK write operation
					priv->station_state = STATION_STATE_SECURE_DONE;
					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_CONNECT:	// received confirm for connect to an adhoc network operation
					priv->station_state = STATION_STATE_JOIN_ADHOC_DONE;
					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_START:		// received confirm for a start adhoc network operation
					priv->station_state = STATION_STATE_START_ADHOC_DONE;
					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_PWR_MODE:		// received confirm for setting the power mode
					priv->station_state = STATION_STATE_PS_MODE_DONE;
					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
#ifdef ZG_CONN_SIMPLIFIED
				case ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT:
					// check if configured security type matches the available security on the AP
					if ( ((zg_connect_cnf_t*)priv->com_cxt.rx_msg_buf)->secType != priv->configured_security_type ) {
						ZG_PRINT(1, "configured security does not match available security\n");
						
						// disconnect
						priv->disconn_tx_frame = ZG_TX_DEAUTH;
						priv->station_state = STATION_STATE_READY;
					}
					else
						priv->station_state = STATION_STATE_ASSOCIATE_DONE;

					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
				case ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT_MANAGE:	// received confirm for setting connection manage
					priv->station_state = STATION_STATE_MANAGE_DONE;
					if ( !schedule_work(&priv->zg_conn_wrk_q) )
						ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					break;
#endif /* ZG_CONN_SIMPLIFIED */
				default:
					ZG_PRINT(1, "default subtype : ZG_MAC_TYPE_MGMT_CONFIRM\n");
					break;
			}

			break;
		case ZG_MAC_TYPE_RXDATA_INDICATE:		// data received on the RX path
			switch (priv->rx_buf[ZG_PREAMBLE_SUBTYPE_IDX]) {
				case ZG_MAC_SUBTYPE_RXDATA_IND_STD:
					zg_process_rx_data(priv);
					break;
				default:
					ZG_PRINT(1, "default subtype : ZG_MAC_TYPE_RXDATA_INDICATE\n");
					break;
			}
			break;
		case ZG_MAC_TYPE_MGMT_INDICATE:		// received a Management indicate from the device
			switch (priv->rx_buf[ZG_PREAMBLE_SUBTYPE_IDX]) {
				case ZG_MAC_SUBTYPE_MGMT_IND_DISASSOC:
					// device rcvd disassoc from AP
					ZG_PRINT(1, "%s: ZG_MAC_SUBTYPE_MGMT_IND_DISASSOC\n", dev->name);
					break;
				case ZG_MAC_SUBTYPE_MGMT_IND_DEAUTH:
					// device rcvd deauth from AP
					ZG_PRINT(1, "%s: ZG_MAC_SUBTYPE_MGMT_IND_DEAUTH\n", dev->name);

					if (priv->reconn_on_disconn) {
						// if configured to reconnect on disconnection
						ZG_PRINT(1, "%s: Trying to re-associate\n", dev->name);

						memcpy(priv->desired_essid, priv->essid, priv->essid_len);
						priv->d_essid_len = priv->essid_len;
						priv->specific_essid = 1;

						netif_carrier_off(priv->dev);

						priv->disconn_tx_frame = ZG_DO_NOT_TX_DEAUTH;

						if ( !schedule_work(&priv->zg_conn_wrk_q) )
							ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
					}
					else {
						// disconnection reason : possible security issue
						// do not try reconnection
						priv->reconn_on_disconn = 1;
					}

					break;
				case ZG_MAC_SUBTYPE_MGMT_IND_CONN_STATUS:		// connection status changed
					buf = &(priv->rx_buf[ZG_PREAMBLE_LEN]);
					status = (((u16) buf[0])<<8) | buf[1];

					if (status == 1) {
						ZG_PRINT(1, "Connection lost\n");

						/*
						 * this can happen if :
						 * 		-	the number of missed beacons exceeds the missed
						 * 			beacon threshold
						 *
						 * counting of missed beacons starts as soon as the device enters
						 * the join state.
						 */

						netif_carrier_off(priv->dev);

#ifdef ZG_ROAMING_ON
						ZG_PRINT(1, "%s: Trying to re-associate\n", dev->name);

						// the following for roaming support
						memcpy(priv->desired_essid, priv->essid, priv->essid_len);
						priv->d_essid_len = priv->essid_len;
						priv->specific_essid = 1;

						// dont tx deauth frame, AP might be down
						priv->disconn_tx_frame = ZG_DO_NOT_TX_DEAUTH;

						if ( !schedule_work(&priv->zg_conn_wrk_q) )
							ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
#endif /* ZG_ROAMING_ON */
					}
					else if (status == 2) {
						ZG_PRINT(1, "Connection re-established\n");
						netif_carrier_on(priv->dev);
					}
					else if (status == 3) {
						// WiFi Certification
						ZG_PRINT(1, "Bad MIC frame 1 received\n");
					}
					else if (status == 4) {
						ZG_PRINT(1, "Bad MIC frame 2 received\n");

						// WiFi Certification
						// to prevent the device from reassociating with the AP
						priv->reconn_on_disconn = 0;
					}
					else if (status == 5) {
						// If connection management enabled on G2100 using the
						// zg_connect_manage function,
						// This is an indication message from G2100 informing
						// the host that the device has started reconnection
						// procedure. The host should stop all data traffic
						// until it receives a stop indicate 
						ZG_PRINT(1, "Reconn start\n");
						netif_carrier_off(priv->dev);
					}
					else if (status == 6) {
						// If connection management enabled on G2100 using the
						// zg_connect_manage function,
						// This is an indication message from G2100 informing
						// the host that the device has completed reconnection
						// procedure. It is possible that the reconnection was
						// unsuccessful in which case a connection lost indicate
						// message will follow this indicate
						ZG_PRINT(1, "Reconn stop\n");
						netif_carrier_on(priv->dev);
					}

					break;
				default:
					ZG_PRINT(1, "default subtype : ZG_MAC_TYPE_MGMT_INDICATE\n");
					break;
			}

			break;
		default:
			ZG_PRINT(1, "default type received\n");
			break;
	}

err_int_fail:
	// release buffers
	zg_unlock(priv);

	return;
}

/*
 * FUNCTION	:	zg_send_mgmt_req
 *
 * PARAMS	:
 * 				command -	management command to send
 * 				param_cmd -	if command is get/set cmd, this specifies
 * 							the parameter
 * 				cmd_size -	size of the command payload
 *
 * NOTES	:	Setup the management command and send to G2100
 */
static bool zg_send_mgmt_req(struct zg_private *priv, u8 command, u8 param_cmd, u16 cmd_size)
{
	if (priv->sleep_state == ZG_SLEEP_ENABLE) {
		// if sleep state enabled, disable sleep state
		zg_set_sleep_state(priv, ZG_SLEEP_DISABLE);
		priv->sleep_state = ZG_SLEEP_DISABLE;
	}

	priv->tx_buf[ZG_PREAMBLE_CMD_IDX]	= ZG_CMD_WT_FIFO_MGMT;
	priv->tx_buf[ZG_PREAMBLE_TYPE_IDX]	= ZG_MAC_TYPE_MGMT_REQ;

	switch(command) {
		case ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM;
			switch(param_cmd) {
				case ZG_PARAM_MULTICAST_ADDR:
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX+1] = ZG_PARAM_MULTICAST_ADDR;
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX] = 0;
					priv->com_cxt.wait_for_sp_cnf = ZG_PARAM_MULTICAST_ADDR;
					break;
				case ZG_PARAM_RTS_THRESHOLD:
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX+1] = ZG_PARAM_RTS_THRESHOLD;
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX] = 0;
					priv->com_cxt.wait_for_sp_cnf = ZG_PARAM_RTS_THRESHOLD;
					break;
				case ZG_PARAM_LONG_RETRY_LIMIT:
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX+1] = ZG_PARAM_LONG_RETRY_LIMIT;
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX] = 0;
					priv->com_cxt.wait_for_sp_cnf = ZG_PARAM_LONG_RETRY_LIMIT;
					break;
				case ZG_PARAM_SHORT_RETRY_LIMIT:
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX+1] = ZG_PARAM_SHORT_RETRY_LIMIT;
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX] = 0;
					priv->com_cxt.wait_for_sp_cnf = ZG_PARAM_SHORT_RETRY_LIMIT;
					break;
				case ZG_PARAM_MISSED_BEACON_THRESH:
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX+1] = ZG_PARAM_MISSED_BEACON_THRESH;
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX] = 0;
					priv->com_cxt.wait_for_sp_cnf = ZG_PARAM_MISSED_BEACON_THRESH;
					break;
				case ZG_PARAM_TX_RATE_TABLE_ONOFF:
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX+1] = ZG_PARAM_TX_RATE_TABLE_ONOFF;
					priv->tx_buf[ZG_PREAMBLE_SET_PARAM_ID_IDX] = 0;
					priv->com_cxt.wait_for_sp_cnf = ZG_PARAM_TX_RATE_TABLE_ONOFF;
					break;
				default:
					break;
					
			}
			priv->tx_len = ZG_SET_PARAM_REQ_SIZE + ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_GET_PARAM:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_GET_PARAM;
			switch(param_cmd) {
				case ZG_PARAM_MAC_ADDRESS:
					priv->tx_buf[ZG_PREAMBLE_GET_PARAM_ID_IDX+1] = ZG_PARAM_MAC_ADDRESS;
					priv->tx_buf[ZG_PREAMBLE_GET_PARAM_ID_IDX] = 0;
					priv->com_cxt.wait_for_sp_cnf = ZG_PARAM_MAC_ADDRESS;
					break;
				case ZG_PARAM_REG_DOMAIN:
					priv->tx_buf[ZG_PREAMBLE_GET_PARAM_ID_IDX+1] = ZG_PARAM_REG_DOMAIN;
					priv->tx_buf[ZG_PREAMBLE_GET_PARAM_ID_IDX] = 0;
					priv->com_cxt.wait_for_sp_cnf = ZG_PARAM_REG_DOMAIN;
					break;
				case ZG_PARAM_STAT_COUNTER:
					priv->tx_buf[ZG_PREAMBLE_GET_PARAM_ID_IDX+3] = 0x01;
					priv->tx_buf[ZG_PREAMBLE_GET_PARAM_ID_IDX+2] = 0;
					priv->tx_buf[ZG_PREAMBLE_GET_PARAM_ID_IDX+1] = ZG_PARAM_STAT_COUNTER;
					priv->tx_buf[ZG_PREAMBLE_GET_PARAM_ID_IDX] = 0;
					priv->com_cxt.wait_for_sp_cnf = ZG_PARAM_STAT_COUNTER;
					break;
				case ZG_PARAM_SYSTEM_VERSION:
					priv->tx_buf[ZG_PREAMBLE_GET_PARAM_ID_IDX+1] = ZG_PARAM_SYSTEM_VERSION;
					priv->tx_buf[ZG_PREAMBLE_GET_PARAM_ID_IDX] = 0;
					priv->com_cxt.wait_for_sp_cnf = ZG_PARAM_SYSTEM_VERSION;
					break;
				default:
					break;
			}
			priv->tx_len = ZG_GET_PARAM_REQ_SIZE + ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_GET_PARAM;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_SCAN:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_SCAN;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_SCAN;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_JOIN:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_JOIN;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_JOIN;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_AUTH:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_AUTH;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_AUTH;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_ASSOC:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_ASSOC;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_ASSOC;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_DEAUTH:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_DEAUTH;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_DEAUTH;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_CALC_PSK:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_CALC_PSK;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_CALC_PSK;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_WEP_KEY:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_WEP_KEY;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_WEP_KEY;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_PMK_KEY:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_PMK_KEY;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_PMK_KEY;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_CONNECT:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_CONNECT;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_CONNECT;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_START:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_START;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_START;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_PWR_MODE:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_PWR_MODE;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_PWR_MODE;
			break;
#ifdef ZG_CONN_SIMPLIFIED
		case ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT;
			break;
		case ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT_MANAGE:
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX] = ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT_MANAGE;
			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT_MANAGE;
			break;
#endif /* ZG_CONN_SIMPLIFIED */
		default:
			break;
	}

	// load cmd
	memcpy(&priv->tx_buf[priv->tx_len - priv->mgmt_cmd.cmd_len], priv->mgmt_cmd.cmd, priv->mgmt_cmd.cmd_len);

	priv->rx_len = 0;

	if ( zg_txrx(priv) ) {
		zg_write_cmd(priv, ZG_CMD_WT_FIFO_DONE);
	}

	// enable device to go into sleep state
	if (priv->sleep_state == ZG_SLEEP_DISABLE) {
		zg_set_sleep_state(priv, ZG_SLEEP_ENABLE);
		priv->sleep_state = ZG_SLEEP_ENABLE;
	}

	return 1;
}

/*
 * FUNCTION	:	zg_conn_handler
 *
 * NOTES	:	Takes the device through the process of discovering,
 * 				associating and maintaining association with a network
 */
#ifdef ZG_CONN_SIMPLIFIED
static void zg_conn_handler (struct work_struct *work)
{
	struct zg_private *priv = container_of(work, struct zg_private, zg_conn_wrk_q);

	if (priv->chip_state != CHIP_STATE_READY) {
		if ( !schedule_work(&priv->zg_conn_wrk_q) )
			ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
		return;
	}

	switch (priv->station_state) {
		case STATION_STATE_READY:
			if (netif_running(priv->dev))
				netif_stop_queue(priv->dev);

			priv->station_state = STATION_STATE_DOWN;

			if (priv->iw_mode == IW_MODE_INFRA)
				zg_disconnect(priv, priv->disconn_tx_frame);
			else if (priv->iw_mode == IW_MODE_ADHOC)
				zg_disconnect(priv, ZG_DO_NOT_TX_DEAUTH);

			break;
		case STATION_STATE_DOWN:
		case STATION_STATE_NOT_ASSOCIATED:
			zg_setup_security(priv);
			break;
		case STATION_STATE_CALC_PSK_DONE:
			zg_write_psk_key(priv);
			break;
		case STATION_STATE_SECURE_DONE:
			zg_connect_manage(priv);
			break;
		case STATION_STATE_MANAGE_DONE:
			zg_connect(priv, 0xff);
			break;
		case STATION_STATE_ASSOCIATE_DONE:
			priv->station_state = STATION_STATE_READY;
			netif_start_queue(priv->dev);
			netif_carrier_on(priv->dev);

			printk("Ready\n");
			break;
		default:
			if (netif_running(priv->dev))
				netif_stop_queue(priv->dev);

			priv->station_state = STATION_STATE_DOWN;

			zg_disconnect(priv, 0);
			break;
	}

	return;
}
#else /* ZG_CONN_SIMPLIFIED */
static void zg_conn_handler (struct work_struct *work)
{
	struct zg_private *priv = container_of(work, struct zg_private, zg_conn_wrk_q);
	u8 ap_found;

	if (priv->chip_state != CHIP_STATE_READY) {
		if ( !schedule_work(&priv->zg_conn_wrk_q) )
			ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
		return;
	}

	switch (priv->station_state) {
		case STATION_STATE_READY:
			if (netif_running(priv->dev))
				netif_stop_queue(priv->dev);

			priv->station_state = STATION_STATE_DOWN;

			if (priv->iw_mode == IW_MODE_INFRA)
				zg_disconnect(priv, priv->disconn_tx_frame);
			else if (priv->iw_mode == IW_MODE_ADHOC)
				zg_disconnect(priv, ZG_DO_NOT_TX_DEAUTH);

			break;
		case STATION_STATE_DOWN:
		case STATION_STATE_NOT_ASSOCIATED:
			// to put the device into idle mode
			if (priv->disconnect) {
				// enable device to go into sleep state
				if (priv->sleep_state == ZG_SLEEP_DISABLE) {
					zg_set_sleep_state(priv, ZG_SLEEP_ENABLE);
					priv->sleep_state = ZG_SLEEP_ENABLE;
				}

				priv->disconnect = 0;
				break;
			}

			priv->station_state = STATION_STATE_SCANNING;
			if (priv->specific_essid) {
				if (priv->new_bssid) {
					memcpy(priv->bssid, priv->desired_bssid, ETH_ALEN);
				}
				else {
					memcpy(priv->essid, priv->desired_essid, priv->d_essid_len);
					priv->essid_len = priv->d_essid_len;
				}

				if (priv->iw_mode == IW_MODE_INFRA)
					zg_scan(priv, 1, ZG_BSS_INFRA);
				else if (priv->iw_mode == IW_MODE_ADHOC)
					zg_scan(priv, 1, ZG_BSS_ADHOC);
			}
			else {
				if (priv->iw_mode == IW_MODE_INFRA)
					zg_scan(priv, 0, ZG_BSS_INFRA);
				else if (priv->iw_mode == IW_MODE_ADHOC)
					zg_scan(priv, 0, ZG_BSS_ADHOC);
			}
			break;
		case STATION_STATE_SCAN_DONE:
			priv->station_state = STATION_STATE_SECURE;
			// select an AP and setup parameters to initiate connection process
			if (priv->specific_essid)
				ap_found = zg_setup_ap_params(priv, 1);
			else
				ap_found = zg_setup_ap_params(priv, 0);

			// if no suitable AP found
			if (!ap_found) {
				if (priv->iw_mode == IW_MODE_INFRA) {
					priv->station_state = STATION_STATE_NOT_ASSOCIATED;
					printk("Station not found\n");

					// try multiple times before giving up
					if (priv->conn_trial < 3) {

						if ( !schedule_work(&priv->zg_conn_wrk_q) )
							ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");

							priv->conn_trial++;
					}
					else
						priv->conn_trial = 0;

					break;
				}
				else {
					// start an adhoc network
					priv->adhoc_role = ZG_ADHOC_ROLE_CREATOR;

					// the device chooses a random bssid for the adhoc network
					// this is a dummy bssid and doesnot match the actual
					// bssid chosen by the device
					memcpy(priv->bssid, priv->dev->dev_addr, 6);

					priv->iw_mode = IW_MODE_ADHOC;
					priv->beacon_period = ZG_ADHOC_BEACON_PERIOD;
					if ( !(priv->security_type == ZG_SECURITY_TYPE_NONE || priv->security_type == ZG_SECURITY_TYPE_WEP) )
						priv->security_type = ZG_SECURITY_TYPE_NONE;
				}
			}

			zg_setup_security(priv);

			break;
		case STATION_STATE_CALC_PSK_DONE:
			zg_write_psk_key(priv);
			break;
		case STATION_STATE_SECURE_DONE:
			if (priv->iw_mode == IW_MODE_INFRA) {
				priv->station_state = STATION_STATE_JOINING;
				zg_join(priv);
			}
			else if (priv->iw_mode == IW_MODE_ADHOC) {
				if (priv->adhoc_role == ZG_ADHOC_ROLE_JOINER) {
					priv->station_state = STATION_STATE_JOINING_ADHOC;
					zg_join_adhoc(priv);
				}
				else if (priv->adhoc_role == ZG_ADHOC_ROLE_CREATOR) {
					priv->station_state = STATION_STATE_STARTING_ADHOC;
					zg_start_adhoc(priv);
				}
			}
			break;
		case STATION_STATE_JOIN_DONE:
			priv->station_state = STATION_STATE_AUTHENTICATING;
			zg_authenticate(priv, priv->trial_num);
			break;
		case STATION_STATE_AUTHENTICATE_DONE:
			priv->station_state = STATION_STATE_ASSOCIATING;
			zg_associate(priv);
			break;
		case STATION_STATE_ASSOCIATE_DONE:
		case STATION_STATE_JOIN_ADHOC_DONE:
		case STATION_STATE_START_ADHOC_DONE:
			priv->station_state = STATION_STATE_PS_MODE;
			zg_set_power_mode(priv);
			break;
		case STATION_STATE_PS_MODE_DONE:
			priv->station_state = STATION_STATE_READY;
			netif_start_queue(priv->dev);
			netif_carrier_on(priv->dev);

			printk("Ready\n");
			break;
		default:
			if (netif_running(priv->dev))
				netif_stop_queue(priv->dev);

			priv->station_state = STATION_STATE_DOWN;

			zg_disconnect(priv, 0);
			break;
	}

	return;
}
#endif /* ZG_CONN_SIMPLIFIED */

/*
 * FUNCTION	:	zg_scan
 *
 * PARAMS	:
 * 				specific_ssid -	1 : if scanning for specific SSID
 * 								0 : if scanning for any network
 * 				bss_type -		type of network; Infrastructure or Adhoc
 *
 * NOTES	:	Setup management command to initiate scan operation
 */
static void zg_scan(struct zg_private *priv, int specific_ssid, int bss_type)
{
	zg_scan_req_t* cmd = (zg_scan_req_t*)priv->mgmt_cmd.cmd;
	u8 i, chnl;

	cmd->probe_delay = HSTOZGS(scan_probe_delay);
	cmd->min_channel_time = HSTOZGS(scan_min_chan_time);
	cmd->max_channel_time = HSTOZGS(scan_max_chan_time);

	cmd->bss = bss_type;

	cmd->scan_type = ZG_SCAN_TYPE_ACTIVE;		// active scan

	memset(cmd->bssid, 0xff, 6);
	memset(cmd->ssid, 0, 32);
	memset(cmd->channel_list, 0, 14);

	// if looking for a specific network
	if (specific_ssid) {
		// if BSSID of network specified
		if (priv->new_bssid) {
			memcpy(cmd->bssid, priv->bssid, ETH_ALEN);
			priv->new_bssid = 0;
		}
		else {
			// if SSID of network specified
			memcpy(cmd->ssid, priv->essid, priv->essid_len);
			cmd->ssid_len = priv->essid_len;
		}
	}
	else {
		// if scanning for any available network
		memset(cmd->ssid, 0, 32);
		cmd->ssid_len = 0;
	}

	// setup channels used for scanning
	for(i = 0, chnl = channel_table[priv->reg_domain].min; chnl <= channel_table[priv->reg_domain].max; i++, chnl++) {
		cmd->channel_list[i] = chnl;
	}
	cmd->channel_len = i;

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_SCAN;
	priv->mgmt_cmd.param_cmd = 0;
	priv->mgmt_cmd.cmd_len = ZG_SCAN_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_store_bss_info
 *
 * NOTES	:	Store results returned by G2100 from the scan operation
 */
static void zg_store_bss_info(struct zg_private *priv)
{
	u8* desc;
	u16 i;
	zg_scan_result_t* hdr;
	zg_bss_desc_t* bss_desc;

	desc = &(((u8*)priv->com_cxt.rx_msg_buf)[ZG_SCAN_RESULT_SIZE]);
	hdr = (zg_scan_result_t*)priv->com_cxt.rx_msg_buf;

	for(i=0; i < hdr->num_bss_desc ; i++)
	{
		bss_desc = (zg_bss_desc_t*) desc;

		priv->num_bss_entries++;

		memcpy(priv->bss_info[i].bssid, bss_desc->bssid, 6);
		priv->bss_info[i].rssi = bss_desc->rssi;

		priv->bss_info[i].channel = bss_desc->channel;
		priv->bss_info[i].beacon_period = ZGSTOHS(bss_desc->beacon_period);
		priv->bss_info[i].security_enabled = (u8)(ZGSTOHS(bss_desc->cap_info) & (WLAN_CAPABILITY_PRIVACY << 8));
		memcpy(priv->bss_info[i].ssid, bss_desc->ssid, bss_desc->ssid_len);
		priv->bss_info[i].ssid_len = bss_desc->ssid_len;

		if (bss_desc->bss_type == ZG_BSS_INFRA)
			priv->bss_info[i].bss_type = IW_MODE_INFRA;
		else if (bss_desc->bss_type == ZG_BSS_ADHOC)
			priv->bss_info[i].bss_type =IW_MODE_ADHOC;

		priv->bss_info[i].preamble = ZGSTOHS(bss_desc->cap_info) & ZG_CAP_BIT_SHORT_PREAMBLE ? ZG_SHORT_PREAMBLE : ZG_LONG_PREAMBLE;

		desc += ZG_BSS_DESC_SIZE;
	}

	return;
}

/*
 * FUNCTION	:	zg_send_event
 *
 * NOTES	:	Send event to userspace indicating end of scan
 */
static void zg_send_event(struct zg_private *priv)
{
	union iwreq_data data;

	data.data.length = 0;
	data.data.flags = 0;
	wireless_send_event(priv->dev, SIOCGIWSCAN, &data, NULL);

	return;
}

/*
 * FUNCTION	:	zg_join
 *
 * NOTES	:	Setup management command to join a network
 */
static void zg_join(struct zg_private *priv)
{
	zg_join_req_t* cmd = (zg_join_req_t*)priv->mgmt_cmd.cmd;

	cmd->to				= HSTOZGS(ZG_JOIN_TIMEOUT);
	cmd->beaconPeriod	= HSTOZGS(priv->beacon_period);
	cmd->channel 		= priv->channel;
	cmd->ssidLen		= priv->essid_len;
	memcpy(cmd->bssid, priv->bssid, ETH_ALEN);
	memset(cmd->ssid, 0, 32);
	memcpy(cmd->ssid, priv->essid, priv->essid_len);

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_JOIN;
	priv->mgmt_cmd.param_cmd = 0;
	priv->mgmt_cmd.cmd_len = ZG_JOIN_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_authenticate
 *
 * PARAMS	:
 * 				trial_num -	trial number of the authentication process
 * 							trial 1 uses open authentication. if the AP is
 * 							using shared authentication, this operation fails
 * 							and trial 2 uses shared authentication
 *
 * NOTES	:	Setup management command to authenticate with network
 */
static void zg_authenticate(struct zg_private *priv, int trial_num)
{
	zg_auth_req_t* cmd = (zg_auth_req_t*)priv->mgmt_cmd.cmd;

	memcpy(cmd->addr, priv->bssid, ETH_ALEN);
	cmd->to = HSTOZGS(ZG_AUTH_TIMEOUT);

	if (trial_num == 1) {
			cmd->alg = ZG_AUTH_ALG_OPEN;
			priv->auth_mode = ZG_AUTH_ALG_OPEN;
	}
	else {
		cmd->alg = ZG_AUTH_ALG_SHARED;
		priv->auth_mode = ZG_AUTH_ALG_SHARED;

		// reset
		priv->trial_num = 1;
	}

	cmd->reserved = 0;

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_AUTH;
	priv->mgmt_cmd.param_cmd = 0;
	priv->mgmt_cmd.cmd_len = ZG_AUTH_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_associate
 *
 * NOTES	:	Setup management command to associate with network
 */
static void zg_associate(struct zg_private *priv)
{
	zg_assoc_req_t* cmd = (zg_assoc_req_t*)priv->mgmt_cmd.cmd;

	memcpy(cmd->addr, priv->bssid, ETH_ALEN);
	cmd->channel = 0;	// select the channel used by AP
	// security selection bits to instruct construction
	// of a security IE
	if((priv->cap_info[0] & ZG_CAP_BIT_PRIVACY) != 0) {
		if (priv->security_type == ZG_SECURITY_TYPE_WPA2)
			cmd->secReq = (priv->security_info[1] & ZG_SEC_INFO_GROUP_TKIP)? ZG_SEC_INFO_RSN_PSK_TKIP : ZG_SEC_INFO_RSN_PSK_CCMP;
		else if (priv->security_type == ZG_SECURITY_TYPE_WPA)
			cmd->secReq = (priv->security_info[0] & ZG_SEC_INFO_GROUP_CCMP)? ZG_SEC_INFO_WPA_PSK_CCMP : ZG_SEC_INFO_WPA_PSK_TKIP;
		else if (priv->security_type == ZG_SECURITY_TYPE_WEP)
			cmd->secReq = 0;
	}
	else {
		cmd->secReq = 0;
	}

	cmd->to = HSTOZGS(ZG_ASSOC_TIMEOUT);
	// the capabilities of this station
	cmd->capInfo = priv->cap_info[0] & ZG_CAP_BIT_MASK;
	
	// the number of beacons that may pass between
	// listen attempts by this Station. Indicates to
	// the AP how how much resources may be required to
	// buffer for this station while it sleeps
	cmd->listenIntval = HSTOZGS((u16)priv->ps_wakeup_interval);
	// length of additional elements to be included in
	// the assoc frame that immediately follows this
	// data structure
	cmd->elemLen = HSTOZGS(0);

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_ASSOC;
	priv->mgmt_cmd.param_cmd = 0;
	priv->mgmt_cmd.cmd_len = ZG_ASSOC_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_disconnect
 *
 * PARAMS	:
 * 				tx_frame -	ZG_TX_DEAUTH : transmit a disconnect frame to the AP
 * 							ZG_DO_NOT_TX_DEAUTH : do not transmit a disconnect frame to the AP
 *
 * NOTES	:	Setup management command to disconnect from a network
 */
static void zg_disconnect(struct zg_private *priv, u8 tx_frame)
{
	zg_disconnect_req_t* cmd = (zg_disconnect_req_t*)priv->mgmt_cmd.cmd;

	cmd->reasonCode = HSTOZGS(3);	// Station leaving network
	cmd->disconnect = 1;
	cmd->txFrame = tx_frame;

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_DEAUTH;
	priv->mgmt_cmd.param_cmd = 0;
	priv->mgmt_cmd.cmd_len = ZG_DISCONNECT_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_data_handler
 *
 * NOTES	:	Handle transmission of the data packet
 */
static void zg_data_handler (struct work_struct *work)
{
	struct zg_private *priv = container_of(work, struct zg_private, zg_data_wrk_q);
	struct net_device *dev = priv->dev;

	zg_tx_data_req_t* cmd = (zg_tx_data_req_t*)&(priv->tx_buf[ZG_PREAMBLE_LEN]);
	struct sk_buff *skb = priv->skb;
	int i;
	int data_req_len;
	u16 num_chunks;

	data_req_len = skb->len + ZG_DATA_REQ_HDR;	

	if ( (data_req_len) < priv->fifo_byte_cnt ) {
		// if sufficient buffer space available

		// 10 bytes go into head buffer
		// removed to calculate number of chunks
		num_chunks = ((data_req_len) - 10) / priv->write_buffer_size;

		if (num_chunks * priv->write_buffer_size < ((data_req_len) - 10) )
			num_chunks++;

		// account for buffer space used
		priv->fifo_byte_cnt -= num_chunks * priv->write_buffer_size;

		// wake stack
		netif_wake_queue(priv->dev);
	}
	else {
		// updating available buffer space
		zg_read_fifo_byte_cnt(priv);

		if ( (data_req_len) < priv->fifo_byte_cnt ) {
			if ( !schedule_work(&priv->zg_data_wrk_q) )
				ZG_PRINT(1, "schedule_work zg_data_wrk_q failed\n");
		}
		else
			priv->txpacket_waiting = 1;

		return;
	}

	// lock buffers
	if ( zg_lock(priv) != 0 ) {
		dev_kfree_skb(skb);
		(priv->stats).tx_errors++;
		(priv->stats).tx_dropped++;
		netif_wake_queue(priv->dev);
		return;
	}

	cmd->reqID = priv->data_req_id++;
	cmd->reserved = 0x00;

	// load data payload
	// using the incoming ethernet frame to create a zg_tx_data_req_t
	// for the device
	for (i = 0; i < skb->len; i++)
		cmd->da[i] = ((u8*)(skb->data))[i];

	// LLC header
	cmd->da[ZG_LLC_DEST_SAP] = 0xaa;
	cmd->da[ZG_LLC_SRC_SAP] = 0xaa;
	cmd->da[ZG_LLC_CMD] = 0x03;
	cmd->da[ZG_LLC_VENDOR0] = cmd->da[ZG_LLC_VENDOR1] = cmd->da[ZG_LLC_VENDOR2] = 0x00;

	dev->trans_start = jiffies;

	// start SPI operation
	// the incoming ethernet frame was morphed into a zg_tx_data_req_t
	// so add 2 to account for the reqID and reserved bytes
	zg_send_data_req(priv, ZG_MAC_SUBTYPE_TXDATA_REQ_STD, (skb->len + 2) );

	// update network statistics
	priv->stats.tx_packets++;
	priv->stats.tx_bytes += skb->len;

	// free socket buffer
	dev_kfree_skb(skb);

	// release buffers
	zg_unlock(priv);

	return;
}

/*
 * FUNCTION	:	zg_send_data_req
 *
 * PARAMS	:
 * 				command -	type of data transmissing; standard or proprietary
 * 				cmd_size -	length of SPI transmission in bytes
 *
 * NOTES	:	Initiate SPI operation to send data packet to G2100
 */
static void zg_send_data_req(struct zg_private *priv, int command, int cmd_size)
{
	if (priv->sleep_state == ZG_SLEEP_ENABLE) {
		// if sleep state enabled, disable sleep state
		zg_set_sleep_state(priv, ZG_SLEEP_DISABLE);
		priv->sleep_state = ZG_SLEEP_DISABLE;
	}

	switch(command) {
		case ZG_MAC_SUBTYPE_TXDATA_REQ_STD:
			priv->tx_buf[ZG_PREAMBLE_CMD_IDX]		= ZG_CMD_WT_FIFO_DATA;
			priv->tx_buf[ZG_PREAMBLE_TYPE_IDX]		= ZG_MAC_TYPE_TXDATA_REQ;
			priv->tx_buf[ZG_PREAMBLE_SUBTYPE_IDX]	= ZG_MAC_SUBTYPE_TXDATA_REQ_STD;

			priv->tx_len = ZG_PREAMBLE_LEN + cmd_size;
			priv->com_cxt.wait_for_cnf = ZG_MAC_SUBTYPE_TXDATA_REQ_STD;
			break;
		default:
			break;
	}

	priv->rx_len = 0;

	if ( zg_txrx(priv) ) {
		zg_write_cmd(priv, ZG_CMD_WT_FIFO_DONE);
	}
	
	// enable device to go into sleep state
	if (priv->sleep_state == ZG_SLEEP_DISABLE) {
		zg_set_sleep_state(priv, ZG_SLEEP_ENABLE);
		priv->sleep_state = ZG_SLEEP_ENABLE;
	}

	return;
}

/*
 * FUNCTION	:	zg_process_rx_data
 *
 * NOTES	:	Process data packet received from G2100
 */
static void zg_process_rx_data(struct zg_private *priv)
{
	zg_rx_data_ind_t* ptr = (zg_rx_data_ind_t*)&(priv->rx_buf[ZG_PREAMBLE_LEN]);
	u16 rx_len;
	struct sk_buff	*skb;
	unsigned char *skbp;

	rx_len = ZGSTOHS(ptr->dataLen);

	if (!(skb = dev_alloc_skb(rx_len + 6))) {
		ZG_PRINT(1, "Failed to get SKB : dropped %d bytes\n", rx_len + 6);
		priv->stats.rx_dropped++;
		return;
	}

	// removing LLC
	skbp = skb_put(skb, rx_len + 6);
	memcpy(&skbp[12], ((u8*)ptr) + 26, rx_len - 6);

	memcpy(skbp, ptr->dstAddr, 6);
	memcpy(&skbp[6], ptr->srcAddr, 6);

	priv->dev->last_rx = jiffies;
	skb->protocol = eth_type_trans(skb, priv->dev);
	skb->ip_summed = CHECKSUM_NONE;
	netif_rx(skb);
	priv->stats.rx_bytes += 6 + rx_len;
	priv->stats.rx_packets++;

	return;
}

/*
 * FUNCTION	:	zg_txrx_complete
 *
 * NOTES	:	Callback function to indicate end of SPI operation
 */
static void zg_txrx_complete(void *arg)
{
	complete(arg);
}

/*
 * FUNCTION	:	zg_set_txrate_onoff
 *
 * PARAMS	:
 * 				value -	enable/disable auto txrate or set fixed txrate
 *
 * NOTES	:	Setup management command to enable/disable auto txrate
 * 				throttling or to set txrate to a specific value
 */
static void zg_set_txrate_onoff(struct zg_private *priv, u8 value)
{
	u8 *buf = priv->mgmt_cmd.cmd;

	buf[0] = value;		// 0xf0 - rate mask : 0x40 - 2Mbps; 0x20 - 1Mbps
						// OR
						// 0x01 - enable auto

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM;
	priv->mgmt_cmd.param_cmd = ZG_PARAM_TX_RATE_TABLE_ONOFF;
	priv->mgmt_cmd.cmd_len = 1;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_get_chip_version
 *
 * NOTES	:	Setup management command to get chip FW version number from G2100
 */
static void zg_get_chip_version(struct zg_private *priv)
{
	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_GET_PARAM;
	priv->mgmt_cmd.param_cmd = ZG_PARAM_SYSTEM_VERSION;
	priv->mgmt_cmd.cmd_len = 0;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_calc_psk_key
 *
 * NOTES	:	Setup management command to send the passphrase to G2100
 * 				and initiate PSK calculation using on-chip supplicant
 */
static void zg_calc_psk_key(struct zg_private *priv)
{
	zg_psk_calc_req_t* cmd = (zg_psk_calc_req_t*)priv->mgmt_cmd.cmd;

	cmd->configBits = 0;
	cmd->phraseLen = priv->wpa_passphrase_len;
	cmd->ssidLen = priv->essid_len;
	cmd->reserved = 0;
	memcpy(cmd->ssid, priv->essid, priv->essid_len);
	memcpy(cmd->passPhrase, priv->wpa_passphrase, priv->wpa_passphrase_len);

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_CALC_PSK;
	priv->mgmt_cmd.param_cmd = 0;
	priv->mgmt_cmd.cmd_len = ZG_PSK_CALC_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_write_wep_key
 *
 * NOTES	:	Setup management command to transfer WEP keys to G2100
 */
static void zg_write_wep_key(struct zg_private *priv)
{
	zg_wep_key_req_t* cmd = (zg_wep_key_req_t*)priv->mgmt_cmd.cmd;

	cmd->slot = ZG_SEC_KEY_SLOT_WEP_DEF;
	cmd->keyLen = priv->wep_key_len[priv->default_key];
	cmd->defID = priv->default_key;
	cmd->ssidLen = priv->essid_len;
	memcpy(cmd->ssid, priv->essid, cmd->ssidLen);
	memcpy(cmd->key, priv->wep_keys, ZG_MAX_ENCRYPTION_KEYS * ZG_MAX_ENCRYPTION_KEY_SIZE);

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_WEP_KEY;
	priv->mgmt_cmd.param_cmd = 0;
	priv->mgmt_cmd.cmd_len = ZG_WEP_KEY_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_setup_security
 *
 * NOTES	:	Setup required security parameters
 */
static void zg_setup_security(struct zg_private *priv)
{
	switch (priv->configured_security_type) {
		case ZG_SECURITY_TYPE_NONE:
			priv->station_state = STATION_STATE_SECURE_DONE;
			if ( !schedule_work(&priv->zg_conn_wrk_q) )
				ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
			break;
		case ZG_SECURITY_TYPE_WEP:
			zg_write_wep_key(priv);
			break;
		case ZG_SECURITY_TYPE_WPA:
		case ZG_SECURITY_TYPE_WPA2:
#ifndef ZG_ONCHIP_SUPP
			priv->station_state = STATION_STATE_CALC_PSK_DONE;
			if ( !schedule_work(&priv->zg_conn_wrk_q) )
				ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
#else
			zg_calc_psk_key(priv);
#endif /* ZG_ONCHIP_SUPP */
			break;
		default:
			break;
	}

	return;
}

/*
 * FUNCTION	:	zg_setup_ap_params
 *
 * PARAMS	:
 * 				specific_ssid -	1 : select a specific SSID
 * 								0 : select any SSID
 *
 * NOTES	:	Select a network to connect to from the network list returned
 * 				by the scan operation
 */
static bool zg_setup_ap_params(struct zg_private *priv, int specific_ssid)
{
	struct _bss_info *bss;
	int max_rssi = 0;
	int max_index = -1;
	int i;

	if (priv->num_bss_entries == 0) {
		return 0;
	}

	for (i=0; i < priv->num_bss_entries; i++)
		if (!priv->bss_info[i].security_enabled && priv->bss_info[i].rssi >= max_rssi) {
			max_rssi = priv->bss_info[i].rssi;
			max_index = i;
		}

	if (max_index != -1)
		bss = &priv->bss_info[max_index];
	else {
		if (specific_ssid)
			bss = &priv->bss_info[0];
		else {
			return 0;
		}
	}

	memcpy(priv->bssid, bss->bssid, 6);
	memcpy(priv->essid, bss->ssid, priv->essid_len = bss->ssid_len);

	priv->iw_mode = bss->bss_type;
	priv->channel = bss->channel & 0x7f;
	priv->preamble = bss->preamble;
	priv->beacon_period = bss->beacon_period;

	return 1;
}

/*
 * FUNCTION	:	zg_set_beacon_miss_thr
 *
 * PARAMS	:
 * 				value - number of missed beacons before assuming connection
 * 						lost
 *
 * NOTES	:	Setup management command to configure beacon missed threshold
 * 				on G2100
 */
static void  zg_set_beacon_miss_thr(struct zg_private *priv, u8 value)
{
	u8 *buf = priv->mgmt_cmd.cmd;

	buf[0] = value;		// number of missed beacons that can be missed
						// before assuming that the connection to the AP
						// is lost

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM;
	priv->mgmt_cmd.param_cmd = ZG_PARAM_MISSED_BEACON_THRESH;
	priv->mgmt_cmd.cmd_len = 1;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_write_psk_key
 *
 * NOTES	:	Setup management command to send PSK to G2100
 */
static void zg_write_psk_key(struct zg_private *priv)
{
	zg_pmk_key_req_t* cmd = (zg_pmk_key_req_t*)priv->mgmt_cmd.cmd;

	cmd->slot = ZG_SEC_KEY_SLOT_PMK0;
	cmd->ssidLen = priv->essid_len;
	memcpy(cmd->ssid, priv->essid, cmd->ssidLen);
#ifndef ZG_ONCHIP_SUPP
	memcpy(cmd->keyData, zg_psk_key, ZG_MAX_PMK_LEN);
#else
	memcpy(cmd->keyData, priv->wpa_psk_key, ZG_MAX_PMK_LEN);
#endif /* ZG_ONCHIP_SUPP */

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_PMK_KEY;
	priv->mgmt_cmd.param_cmd = 0;
	priv->mgmt_cmd.cmd_len = ZG_PMK_KEY_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_join_adhoc
 *
 * NOTES	:	Setup management command to initiate G2100 to join
 * 				an adhoc network
 */
static void zg_join_adhoc(struct zg_private *priv)
{
	zg_adhoc_connect_req_t* cmd = (zg_adhoc_connect_req_t*)priv->mgmt_cmd.cmd;

	cmd->timeout = HSTOZGS(ZG_ADHOC_JOIN_TIMEOUT);
	cmd->beaconPrd = HSTOZGS(ZG_ADHOC_BEACON_PERIOD);
	cmd->channel = priv->channel;
	memcpy(cmd->bssid, priv->bssid, ETH_ALEN);
	cmd->ssidLen = priv->essid_len;
	memcpy(cmd->ssid, priv->essid, priv->essid_len);

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_CONNECT;
	priv->mgmt_cmd.param_cmd = 0;
	priv->mgmt_cmd.cmd_len = ZG_ADHOC_CONNECT_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_start_adhoc
 *
 * NOTES	:	Setup management command to initiate G2100 to start
 * 				an adhoc network
 */
static void zg_start_adhoc(struct zg_private *priv)
{
	zg_adhoc_start_req_t* cmd = (zg_adhoc_start_req_t*)priv->mgmt_cmd.cmd;

	cmd->channel = priv->channel;
	cmd->ssidLen = priv->essid_len;
	cmd->beaconPrd = HSTOZGS(ZG_ADHOC_BEACON_PERIOD);
	cmd->capInfo = ZG_CAP_BIT_IBSS;

	if (priv->security_type == ZG_SECURITY_TYPE_WEP)
		cmd->capInfo |= ZG_CAP_BIT_PRIVACY;

	memcpy(cmd->ssid, priv->essid, priv->essid_len);

	cmd->dataRateLen = 2;			// number of data rates
	cmd->dataRates[0] = 0x82;		// 1 Mbps
	cmd->dataRates[1] = 0x84;		// 2 Mbps

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_ADHOC_START;
	priv->mgmt_cmd.param_cmd = 0;
	priv->mgmt_cmd.cmd_len = ZG_ADHOC_START_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_set_power_mode
 *
 * NOTES	:	Configure G2100 to go into Power Save mode
 */
static void zg_set_power_mode(struct zg_private *priv)
{
	zg_pwr_mode_req_t* cmd = (zg_pwr_mode_req_t*)priv->mgmt_cmd.cmd;

	// setup parameters to go into PS Poll mode
	if(priv->desired_ps_mode == 0) {
		cmd->mode = ZG_PWR_MODE_ACTIVE;
		cmd->wake = 1;
		cmd->rcvDtims = 1;
	}
	else {
		cmd->mode = ZG_PWR_MODE_SAVE;
		cmd->wake = 0;
		cmd->rcvDtims = 1;
	}

	priv->ps_mode = priv->desired_ps_mode;

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_PWR_MODE;
	priv->mgmt_cmd.param_cmd = 0;
	priv->mgmt_cmd.cmd_len = ZG_PWR_MODE_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_set_mcast_address
 *
 * PARAMS	:
 * 				index -	indicates the slot number used to write the address
 *
 * NOTES	:	Setup management command to write multicast addresses to G2100
 */
static void zg_set_mcast_address(struct zg_private *priv, u8 index)
{
	u8 *buf = priv->mgmt_cmd.cmd;

	buf[0] = index + ZG_MULTICAST_SLOTS_START;
	buf[1] = ZG_ADDR_TYPE_MULTICAST;
	buf[2] = priv->mc_list[index][0];
	buf[3] = priv->mc_list[index][1];
	buf[4] = priv->mc_list[index][2];
	buf[5] = priv->mc_list[index][3];
	buf[6] = priv->mc_list[index][4];
	buf[7] = priv->mc_list[index][5];

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM;
	priv->mgmt_cmd.param_cmd = ZG_PARAM_MULTICAST_ADDR;
	priv->mgmt_cmd.cmd_len = 8;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_set_multicast_list
 *
 * NOTES	:	Obtain configured multicast addresses for the device
 * 				and initiate procedure to configure G2100
 */
static void zg_set_multicast_list(struct net_device *dev)
{
	struct zg_private *priv = netdev_priv(dev);
	struct dev_mc_list *mc_list;
	u8 i;

	mc_list = dev->mc_list;

	for (i = 0; i < ZG_TOTAL_MULTICAST_SLOTS; i++) {
		if (mc_list) {
			memcpy(priv->mc_list[i], mc_list->dmi_addr, 6);
			mc_list = mc_list->next;
		} else {
			memset(priv->mc_list[i], 0, 6);
		}
	}

	// write mc_list to HW
	if ( priv->station_state == STATION_STATE_READY ) {

		spin_lock(&priv->mc_lock);
		if (priv->mc_running) {
			spin_unlock(&priv->mc_lock);
		}
		else {
			priv->mc_running++;
			priv->mc_list_idx = 0;

			zg_set_mcast_address(priv, priv->mc_list_idx);
			priv->mc_list_idx++;

			spin_unlock(&priv->mc_lock);
		}

	}
}

/*
 * FUNCTION	:	zg_set_retry_limit
 *
 * PARAMS	:
 * 				type -	ZG_RETRY_LIMIT_LONG, ZG_RETRY_LIMIT_SHORT
 * 				value -	retry limit
 *
 * NOTES	:	Setup management command to send retry limit to G2100
 */
static void zg_set_retry_limit(struct zg_private *priv, u8 type, u8 value)
{
	u8 *buf = priv->mgmt_cmd.cmd;

	buf[0] = value;

	if (type == ZG_RETRY_LIMIT_LONG) {
		priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM;
		priv->mgmt_cmd.param_cmd = ZG_PARAM_LONG_RETRY_LIMIT;
		priv->mgmt_cmd.cmd_len = 1;
	}
	else if (type == ZG_RETRY_LIMIT_SHORT) {
		priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM;
		priv->mgmt_cmd.param_cmd = ZG_PARAM_SHORT_RETRY_LIMIT;
		priv->mgmt_cmd.cmd_len = 1;
	}

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_set_rts_threshold
 *
 * PARAMS	:
 * 				value -	RTS threshold
 *
 * NOTES	:	Setup management command to send RTS threshold to G2100
 */
static void zg_set_rts_threshold(struct zg_private *priv, u16 value)
{
	u8 *buf = priv->mgmt_cmd.cmd;

	buf[0] = (u8)(value >> 8);
	buf[1] = (u8)(value & 0x00ff);

	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_SET_PARAM;
	priv->mgmt_cmd.param_cmd = ZG_PARAM_RTS_THRESHOLD;
	priv->mgmt_cmd.cmd_len = 2;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_set_sleep_state
 *
 * PARAMS	:
 * 				state -	ZG_SLEEP_ENABLE, ZG_SLEEP_DISABLE
 *
 * NOTES	:	Enables G2100 to go into sleep state
 * 				This function sets a bit on G2100 indicating that the host has
 * 				completed its transactions and the device can go to the sleep
 * 				state if desired. The device will have to be brought out of the
 * 				sleep state before the host can send any management or data
 * 				requests
 */
static void zg_set_sleep_state(struct zg_private *priv, u8 state)
{
	// modify power bit to enable/disable sleep state
	priv->com_cxt.tx_reg_buf[0] = 0;
	priv->com_cxt.tx_reg_buf[1] = (state == ZG_SLEEP_ENABLE) ? ZG_ENABLE_LOW_PWR_MASK : 0;
	zg_write_reg(priv, ZG_PWR_CTRL_REG, ZG_PWR_CTRL_REG_LEN);

	// if active state desired
	if (state == ZG_SLEEP_DISABLE) {
		// wait for G2100 to come out of sleep by polling the response bit
		do {
			priv->com_cxt.tx_reg_buf[0] = 0;
			priv->com_cxt.tx_reg_buf[1] = ZG_PWR_STATUS_REG;
			zg_write_reg(priv, ZG_INDEX_ADDR_REG, ZG_INDEX_ADDR_REG_LEN);

			priv->com_cxt.rx_reg_buf[0] = 0xff;
       		priv->com_cxt.rx_reg_buf[1] = 0xff;
			zg_read_reg(priv, ZG_INDEX_DATA_REG, ZG_INDEX_DATA_REG_LEN);
		} while ( (priv->com_cxt.rx_reg_buf[1] & ZG_ENABLE_LOW_PWR_MASK) );
	}

	return;
}

/*
 * FUNCTION	:	zg_get_stat_counters
 *
 * NOTES	:	Setup management command to obtain the statistics
 * 				counters from G2100
 */
static void zg_get_stat_counters(struct zg_private *priv)
{
	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_GET_PARAM;
	priv->mgmt_cmd.param_cmd = ZG_PARAM_STAT_COUNTER;
	priv->mgmt_cmd.cmd_len = 0;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_read_fifo_byte_cnt
 *
 * NOTES	:	Read available write buffer space from G2100
 */
static void zg_read_fifo_byte_cnt(struct zg_private *priv)
{
	u16 new_val;
	u16 old_val;

	new_val = old_val = 0;

	// wait until we get two consecutive values which are identical
	// for stability
	do {
		old_val = new_val;
		priv->com_cxt.rx_reg_buf[0] = 0;
       	priv->com_cxt.rx_reg_buf[1] = 0;
		zg_read_reg(priv, ZG_BYTE_COUNT_REG, ZG_BYTE_COUNT_REG_LEN);
		new_val = ((priv->com_cxt.rx_reg_buf[0] & 0x0f) << 8) | priv->com_cxt.rx_reg_buf[1];
	} while ( new_val != old_val);

	priv->fifo_byte_cnt = new_val;

	return;
}

/*
 * FUNCTION	:	zg_read_chip_info_block
 *
 * NOTES	:	Read configured chunk size from G2100
 * 				The device maintains a block of data that can be read via the
 * 				ZG_SYS_INFO_DATA_REG register. Within this block are useful
 * 				parameters namely the system version number, buffer sizes.
 * 				This function reads	the write buffer size and stores it
 * 				for later use.
 */
static void zg_read_chip_info_block(struct zg_private *priv)
{
	int i;

	// lock buffers
	if ( zg_lock(priv) != 0 )
		return;

	priv->com_cxt.tx_reg_buf[0] = 0;
	priv->com_cxt.tx_reg_buf[1] = ZG_SYS_INFO_OFFSET_RX_CONT_SIZE;
	zg_write_reg(priv, ZG_SYS_INFO_IDX_REG, ZG_SYS_INFO_IDX_REG_LEN);

	for(i = 0 ; i < ZG_SYS_INFO_LEN_RX_CONT_SIZE ; i++) {
		priv->com_cxt.rx_reg_buf[0] = 0xff;
       	priv->com_cxt.rx_reg_buf[1] = 0xff;
		zg_read_reg(priv, ZG_SYS_INFO_DATA_REG, ZG_SYS_INFO_DATA_REG_LEN);

		if (i == 0)
			priv->write_buffer_size = priv->com_cxt.rx_reg_buf[0] << 8;
		else
			priv->write_buffer_size |= priv->com_cxt.rx_reg_buf[0] & 0xff;
	}

	// release buffers
	zg_unlock(priv);

	return;
}

/*
 * FUNCTION	:	zg_mgmt_handler
 *
 * NOTES	:	Setup management command and initiate SPI operation to
 * 				send to G2100
 */
static void zg_mgmt_handler (struct work_struct *work)
{
	struct zg_private *priv = container_of(work, struct zg_private, zg_mgmt_wrk_q);
	u16 num_chunks;

	if ( (priv->mgmt_cmd.cmd_len + ZG_PREAMBLE_LEN + ZG_GET_PARAM_REQ_SIZE) < priv->fifo_byte_cnt ) {	/* max_preamble_len */
		// if sufficient buffer space available
		num_chunks = (priv->mgmt_cmd.cmd_len + ZG_PREAMBLE_LEN + ZG_GET_PARAM_REQ_SIZE) / priv->write_buffer_size;

		if (num_chunks * priv->write_buffer_size < (priv->mgmt_cmd.cmd_len + ZG_PREAMBLE_LEN + ZG_GET_PARAM_REQ_SIZE) )
			num_chunks++;

		// account for buffer space used
		priv->fifo_byte_cnt -= num_chunks * priv->write_buffer_size;

		// stop stack
		if (netif_running(priv->dev)) {
			netif_stop_queue(priv->dev);
			priv->mgmt_cmd.stack_stopped = 1;
		}
		else
			priv->mgmt_cmd.stack_stopped = 0;
	}
	else {
		// updating available write buffer space
		zg_read_fifo_byte_cnt(priv);

		if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
			ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

		return;
	}

	// lock buffers
	if ( zg_lock(priv) != 0 ) {
		ZG_PRINT(1, "mgmt_handler lock failed\n");
		return;
	}

	zg_send_mgmt_req(priv, priv->mgmt_cmd.cmd_name, priv->mgmt_cmd.param_cmd, priv->mgmt_cmd.cmd_len);

	// release buffers
	zg_unlock(priv);

	return;
}

/*
 * FUNCTION	:	zg_get_mac_address
 *
 * NOTES	:	Setup management command to get MAC address from G2100
 */
static void zg_get_mac_address(struct zg_private *priv)
{
	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_GET_PARAM;
	priv->mgmt_cmd.param_cmd = ZG_PARAM_MAC_ADDRESS;
	priv->mgmt_cmd.cmd_len = 0;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_get_reg_domain
 *
 * NOTES	:	Setup management command to get Region domain from G2100
 */
static void zg_get_reg_domain(struct zg_private *priv)
{
	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_GET_PARAM;
	priv->mgmt_cmd.param_cmd = ZG_PARAM_REG_DOMAIN;
	priv->mgmt_cmd.cmd_len = 0;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_connect
 *
 * NOTES	:	Setup management command to initiate connection to an AP
 * 				or adhoc network (if no adhoc network found, the command
 * 				sets up G2100 to start an adhoc network)
 * 				This is a simplified command that can be used instead of
 * 				the more detailed method of connecting to a BSS
 */
static void zg_connect(struct zg_private *priv, u8 sec_type)
{
	zg_connect_req_t* cmd = (zg_connect_req_t*)priv->mgmt_cmd.cmd;

	cmd->secType = sec_type;	// 0x00 - no security
								// 0x01 - WEP
								// 0x02 - WPA (TKIP)
								// 0x03 - WPA2 (AES)
								// 0xff - use best available security 

	cmd->ssidLen = priv->essid_len;
	memset(cmd->ssid, 0, 32);
	memcpy(cmd->ssid, priv->essid, priv->essid_len);
	
	// assumes that the default beacon_period set in zg_init;
	// needs modification if that value is changed
	cmd->sleepDuration = HSTOZGS( ((priv->ps_wakeup_interval * priv->beacon_period)/100) );		// units of 100 milliseconds
	
	if (priv->iw_mode == IW_MODE_INFRA)
		cmd->modeBss = ZG_BSS_INFRA;
	else if (priv->iw_mode == IW_MODE_ADHOC)
		cmd->modeBss = ZG_BSS_ADHOC;
	
	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT;
	priv->mgmt_cmd.cmd_len = ZG_CONNECT_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * FUNCTION	:	zg_connect_manage
 *
 * NOTES	:	Setup management command to configure G2100 to manage
 * 				the connection to the AP or adhoc network
 * 				If conection management is enabled, the G2100 will monitor
 * 				connection status by listening to beacons and start
 * 				reconnection procedure if connection lost
 * 				This feature can be used in addition to/as a replacement for
 * 				the connection manager implemented in the driver
 */
static void zg_connect_manage(struct zg_private *priv)
{
	zg_connect_manage_req_t* cmd = (zg_connect_manage_req_t*)priv->mgmt_cmd.cmd;

	cmd->connManageState = 1;	// 1 - enable; 0 - disable
    cmd->numRetries = 10;		// specifies the number of retries performed to
    							// try and reconnect to the network

    cmd->flags = 0x10 | 0x02 | 0x01;	// 0x10 -	enable start and stop indication messages
    									// 		 	from G2100 during reconnection
    									// 0x02 -	start reconnection on receiving a deauthentication
    									// 			message from the AP
    									// 0x01 -	start reconnection when the missed beacon count
    									// 			exceeds the threshold. uses default value of
    									//			100 missed beacons if not set during initialization
    cmd->reserved = 0;	
	
	priv->mgmt_cmd.cmd_name = ZG_MAC_SUBTYPE_MGMT_REQ_CONNECT_MANAGE;
	priv->mgmt_cmd.cmd_len = ZG_CONNECT_MANAGE_REQ_SIZE;

	if ( !schedule_work(&priv->zg_mgmt_wrk_q) )
		ZG_PRINT(1, "schedule_work zg_mgmt_wrk_q failed\n");

	return;
}

/*
 * Definition of Wireless Extension handlers
 */

static struct iw_statistics *zg_get_wireless_stats(struct net_device *dev)
{
	struct zg_private *priv = netdev_priv(dev);
	return &priv->wstats;
}

static int zg_config_commit(struct net_device *dev, struct iw_request_info *info, void *zwrq, char *extra)
{
	return zg_open(dev);
}

static int zg_get_name(struct net_device *dev, struct iw_request_info *info, char *cwrq, char *extra)
{
	strcpy(cwrq, "IEEE 802.11-DS");
	return 0;
}

static int zg_set_freq(struct net_device *dev, struct iw_request_info *info, struct iw_freq *freq, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);
	int ret_val = 0;
	int i;

	if ((freq->flags & IW_FREQ_FIXED)) {
		// Setting by frequency, convert to a channel
		if ((freq->e == 1) && (freq->m >= (int) 241200000) && (freq->m <= (int) 248700000)) {
			int f = freq->m / 100000;
			int i = 0;
			
			while ((i < 14) && (f != frequency_list[i]))
				i++;

			freq->e = 0;
			freq->m = i + 1;
		}
		
		// Setting by channel number
		if ((freq->m > 14) || (freq->e > 0))
			ret_val = -EOPNOTSUPP;
		else {
			priv->channel = freq->m;

			// verify if channel defined in reg domain, if not set to minimum
			for (i = 0; i < ARRAY_SIZE(channel_table); i++)
				if (priv->reg_domain == channel_table[i].reg_domain) {
					if (priv->channel < channel_table[i].min || priv->channel > channel_table[i].max)
						priv->channel = channel_table[i].min;

					break;
				}
		}

		ret_val = 0;
	}
	else if (freq->flags & IW_FREQ_AUTO) {
		ZG_PRINT(1, "zg_set_freq called with IW_FREQ_AUTO flag\n");
	}

	return ret_val;
}

static int zg_get_freq(struct net_device *dev, struct iw_request_info *info, struct iw_freq *freq, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	freq->m = priv->channel;
	freq->e = 0;
	
	return 0;
}

static int zg_set_mode(struct net_device *dev, struct iw_request_info *info, __u32 *mode, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);
	int ret_val = 0;

	if (*mode != IW_MODE_ADHOC && *mode != IW_MODE_INFRA)
		ret_val = -EINVAL;

	priv->iw_mode = *mode;

	return ret_val;
}

static int zg_get_mode(struct net_device *dev, struct iw_request_info *info, __u32 *mode, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	*mode = priv->iw_mode;
	return 0;
}

static int zg_get_range(struct net_device *dev, struct iw_request_info *info, struct iw_point *point, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);
	struct iw_range *range = (struct iw_range *)extra;
	int i, j, k;

	point->length = sizeof(struct iw_range);
	memset(range, 0, sizeof(struct iw_range));
	range->min_nwid = 0x0000;
	range->max_nwid = 0x0000;
	range->num_channels = 0;
	
	for (j = 0; j < ARRAY_SIZE(channel_table); j++) {
		if (priv->reg_domain == channel_table[j].reg_domain) {
			range->num_channels = channel_table[j].max - channel_table[j].min + 1;
			
			for (k = 0, i = channel_table[j].min; i <= channel_table[j].max; i++) {
				range->freq[k].i = i;
				range->freq[k].m = frequency_list[i - 1] * 100000;
				range->freq[k++].e = 1;
			}
			range->num_frequency = k;
						
			break;
		}
	}

	range->max_qual.qual = 100;
	range->max_qual.level = 100;
	range->max_qual.noise = 0;
	range->max_qual.updated = IW_QUAL_QUAL_INVALID | IW_QUAL_LEVEL_INVALID | IW_QUAL_NOISE_INVALID;

	range->avg_qual.qual = 50;
	range->avg_qual.level = 50;
	range->avg_qual.noise = 0;
	range->avg_qual.updated = IW_QUAL_QUAL_INVALID | IW_QUAL_LEVEL_INVALID | IW_QUAL_NOISE_INVALID;

	range->sensitivity = 0;

	range->bitrate[0] =  1000000;
	range->bitrate[1] =  2000000;
	range->num_bitrates = 2;

	range->min_rts = 0;
	range->max_rts = 2347;
	range->min_frag = 256;
	range->max_frag = 2346;

	range->encoding_size[0] = 5;
	range->encoding_size[1] = 13;
	range->num_encoding_sizes = 2;
	range->max_encoding_tokens = 4;

	range->pmp_flags = IW_POWER_ON;
	range->pmt_flags = IW_POWER_ON;
	range->pm_capa = 0;

	range->we_version_source = WIRELESS_EXT;
	range->we_version_compiled = WIRELESS_EXT;
	range->retry_capa = IW_RETRY_ON ;
	range->retry_flags = IW_RETRY_ON;
	range->r_time_flags = 0;
	range->min_retry = 1;
	range->max_retry = 255;

	return 0;
}

static int zg_set_wap(struct net_device *dev, struct iw_request_info *info, union iwreq_data *data, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);
	static const u8 any[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
	static const u8 off[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

	if (data->ap_addr.sa_family != ARPHRD_ETHER) {
		ZG_PRINT(1, "Invalid address family\n");
		return -EINVAL;
	}

	// same as current bssid
	if (!memcmp(priv->bssid, data->ap_addr.sa_data, ETH_ALEN))
		return 0;

	// any network
	if (!memcmp(any, data->ap_addr.sa_data, 6)) {
		priv->specific_essid = 0;
	}
	else if (!memcmp(off, data->ap_addr.sa_data, 6)) {		// station off
		printk("Station off\n");

		priv->disconnect = 1;

		if (priv->station_state == STATION_STATE_READY) {
			return -EINPROGRESS;
		}
		else {
			if ( !schedule_work(&priv->zg_conn_wrk_q) )
				ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
			return 0;
		}
	}
	else {
		// new bssid specified
		memcpy(priv->desired_bssid, data->ap_addr.sa_data, ETH_ALEN);
		priv->new_bssid = 1;
		priv->specific_essid = 1;
	}

	return -EINPROGRESS;
}

static int zg_get_wap(struct net_device *dev, struct iw_request_info *info, struct sockaddr *addr, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	memcpy(addr->sa_data, priv->bssid, 6);
	addr->sa_family = ARPHRD_ETHER;

	return 0;
}

static int zg_set_scan(struct net_device *dev, struct iw_request_info *info, struct iw_param *param, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	if (priv->station_state == STATION_STATE_DOWN)
		return -EAGAIN;

	priv->bss_list_valid = 0;

	priv->scan_notify = 1;
	zg_scan(priv, 0, 3);

	return 0;
}

static int zg_get_scan(struct net_device *dev, struct iw_request_info *info, struct iw_point *point, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);
	int i;
	char *current_ev = extra;
	struct iw_event	iw_evt;

	if (!priv->bss_list_valid)
		return -EAGAIN;

	for (i = 0; i < priv->num_bss_entries; i++) {
		iw_evt.cmd = SIOCGIWAP;
		iw_evt.u.ap_addr.sa_family = ARPHRD_ETHER;
		memcpy(iw_evt.u.ap_addr.sa_data, priv->bss_info[i].bssid, 6);
		current_ev = iwe_stream_add_event(current_ev, extra + IW_SCAN_MAX_DATA, &iw_evt, IW_EV_ADDR_LEN);

		iw_evt.u.data.length =  priv->bss_info[i].ssid_len;
		if (iw_evt.u.data.length > 32)
			iw_evt.u.data.length = 32;
		iw_evt.cmd = SIOCGIWESSID;
		iw_evt.u.data.flags = 1;
		current_ev = iwe_stream_add_point(current_ev, extra + IW_SCAN_MAX_DATA, &iw_evt, priv->bss_info[i].ssid);

		iw_evt.cmd = SIOCGIWMODE;
		iw_evt.u.mode = priv->bss_info[i].bss_type;
		current_ev = iwe_stream_add_event(current_ev, extra + IW_SCAN_MAX_DATA, &iw_evt, IW_EV_UINT_LEN);

		iw_evt.cmd = SIOCGIWFREQ;
		iw_evt.u.freq.m = priv->bss_info[i].channel;
		iw_evt.u.freq.e = 0;
		current_ev = iwe_stream_add_event(current_ev, extra + IW_SCAN_MAX_DATA, &iw_evt, IW_EV_FREQ_LEN);

		// Add quality statistics
		iw_evt.cmd = IWEVQUAL;
		iw_evt.u.qual.qual  = priv->bss_info[i].rssi;
		iw_evt.u.qual.noise  = 0;
		iw_evt.u.qual.updated = IW_QUAL_QUAL_UPDATED | IW_QUAL_LEVEL_INVALID | IW_QUAL_NOISE_INVALID;
		current_ev = iwe_stream_add_event(current_ev, extra + IW_SCAN_MAX_DATA , &iw_evt, IW_EV_QUAL_LEN);


		iw_evt.cmd = SIOCGIWENCODE;
		if (priv->bss_info[i].security_enabled)
			iw_evt.u.data.flags = IW_ENCODE_ENABLED | IW_ENCODE_NOKEY;
		else
			iw_evt.u.data.flags = IW_ENCODE_DISABLED;
		iw_evt.u.data.length = 0;
		current_ev = iwe_stream_add_point(current_ev, extra + IW_SCAN_MAX_DATA, &iw_evt, NULL);
	}

	// Length of data
	point->length = (current_ev - extra);
	point->flags = 0;

	return 0;
}

static int zg_set_essid(struct net_device *dev, struct iw_request_info *info, struct iw_point *point, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);
	int ret_val = 0;

	// connect to any available BSS
	if(point->flags == 0) {
		priv->specific_essid = 0;
		ret_val = -EINPROGRESS;
	}
	else {
		if ( !memcmp(extra, "none", 4) ) {
			printk("Station off\n");

			priv->disconnect = 1;

			if (priv->station_state == STATION_STATE_READY) {
				ret_val = -EINPROGRESS;
			}
			else {
				if ( !schedule_work(&priv->zg_conn_wrk_q) )
					ZG_PRINT(1, "schedule_work zg_conn_wrk_q failed\n");
				ret_val = 0;
			}
		}
		else {
			int index = (point->flags & IW_ENCODE_INDEX) - 1;

			priv->specific_essid = 1;

			// Check the size of the string
			if (point->length > ZG_MAX_SSID_LENGTH)
				return -E2BIG;
			if (index != 0)
				return -EINVAL;

			memcpy(priv->desired_essid, extra, point->length);
			priv->d_essid_len = point->length;

			ret_val = -EINPROGRESS;
		}
	}

	return ret_val;
}

static int zg_get_essid(struct net_device *dev, struct iw_request_info *info, struct iw_point *point, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	// return SSID
	if (priv->d_essid_len != 0) {
		memcpy(extra, priv->desired_essid, priv->d_essid_len);
		point->length = priv->d_essid_len;
	}
	else {
		memcpy(extra, priv->essid, priv->essid_len);
		point->length = priv->essid_len;
	}

	if (priv->essid_len || priv->d_essid_len)
		point->flags = 1;
	else
		point->flags = 0;

	return 0;
}

static int zg_set_rate(struct net_device *dev, struct iw_request_info *info, struct iw_param *param, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	if (param->fixed == 0) {
		priv->tx_rate = 0;
		priv->auto_tx_rate = 1;
		zg_set_txrate_onoff(priv, 0x01);	// enable auto rate selection
	}
	else {
		priv->auto_tx_rate = 0;

		if ((param->value < 2) && (param->value >= 0)) {
			// index
			priv->tx_rate = param->value;
		}
		else {
			// value
			switch (param->value) {
				case 1000000:
					priv->tx_rate = 0;
					break;
				case 2000000:
					priv->tx_rate = 1;
					break;
				default:
					return -EINVAL;
			}
		}

		// write value to device
		switch (priv->tx_rate) {
			case 0:
				zg_set_txrate_onoff(priv, 0x20);	// fix tx_rate to 1Mbps
				break;
			case 1:
				zg_set_txrate_onoff(priv, 0x40);	// fix tx_rate to 2Mbps
				break;
			default:
				break;
		}

	}

	return 0;
}

static int zg_get_rate(struct net_device *dev, struct iw_request_info *info, struct iw_param *param, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	if (priv->auto_tx_rate) {
		param->fixed = 0;
		param->value = 2000000;
	}
	else {
		param->fixed = 1;
		switch(priv->tx_rate) {
			case 0:
				param->value =  1000000;
				break;
			case 1:
				param->value =  2000000;
				break;
		}
	}
	return 0;
}

static int zg_set_rts(struct net_device *dev, struct iw_request_info *info, struct iw_param *param, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);
	int rts_thresh = param->value;

	if (param->disabled)
		rts_thresh = 2347;

	if ((rts_thresh < 0) || (rts_thresh > 2347))
		return -EINVAL;

	priv->rts_threshold = rts_thresh;
	zg_set_rts_threshold(priv, priv->rts_threshold);

	return 0;
}

static int zg_get_rts(struct net_device *dev, struct iw_request_info *info, struct iw_param *param, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	param->value = priv->rts_threshold;
	param->disabled = (param->value >= 2347);
	param->fixed = 1;

	return 0;
}

static int zg_set_retry(struct net_device *dev, struct iw_request_info *info, struct iw_param *param, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	if (!param->disabled && (param->flags & IW_RETRY_LIMIT)) {
		if (param->flags & IW_RETRY_LONG) {
			priv->long_retry = param->value;
			zg_set_retry_limit(priv, 1, priv->long_retry);
		}
		else if (param->flags & IW_RETRY_SHORT) {
			priv->short_retry = param->value;
			zg_set_retry_limit(priv, 2, priv->short_retry);
		}
		else {
			// default
			priv->short_retry = param->value;
			zg_set_retry_limit(priv, 2, priv->short_retry);
		}

		return 0;
	}

	return -EINVAL;
}

static int zg_get_retry(struct net_device *dev, struct iw_request_info *info, struct iw_param *param, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	param->disabled = 0;

	if (param->flags & IW_RETRY_LONG) {
		param->flags = IW_RETRY_LIMIT | IW_RETRY_LONG;
		param->value = priv->long_retry;
	}
	else {
		param->flags = IW_RETRY_LIMIT | IW_RETRY_SHORT;
		param->value = priv->short_retry;
	}

	return 0;
}

static int zg_set_encode(struct net_device *dev, struct iw_request_info *info, struct iw_point *point, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	if (point->length > 0) {
		int index = (point->flags & IW_ENCODE_INDEX) - 1;
		int current_index = priv->default_key;

		if (point->length > 13) {
			return -EINVAL;
		}

		if (index < 0 || index >= 4)
			index = current_index;
		else
			priv->default_key = index;

		if (point->length > 5)
			priv->wep_key_len[index] = 13;
		else
			priv->wep_key_len[index] = 5;

		if (!(point->flags & IW_ENCODE_NOKEY)) {
			memset(&priv->wep_keys[index][0], 0, 13);
			memcpy(&priv->wep_keys[index][0], extra, point->length);
		}

		priv->configured_security_type = ZG_SECURITY_TYPE_WEP;
	}
	else {
		int index = (point->flags & IW_ENCODE_INDEX) - 1;
		if (index >= 0 && index < 4)
			priv->default_key = index;
		else
			if (!point->flags & IW_ENCODE_MODE)
				return -EINVAL;
	}

	// Read the flags
	if (point->flags & IW_ENCODE_DISABLED)
		priv->configured_security_type = ZG_SECURITY_TYPE_NONE;
	else
		priv->configured_security_type = ZG_SECURITY_TYPE_WEP;

	return 0;
}

static int zg_get_encode(struct net_device *dev, struct iw_request_info *info, struct iw_point *point, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);
	int index = (point->flags & IW_ENCODE_INDEX) - 1;

	if (priv->security_type == ZG_SECURITY_TYPE_NONE)
		point->flags = IW_ENCODE_DISABLED;
	else
		point->flags = IW_ENCODE_OPEN;

	if (index < 0 || index >= 4)
		index = priv->default_key;
	point->flags |= index + 1;

	point->length = priv->wep_key_len[index];
	memset(extra, 0, 13);
	memcpy(extra, priv->wep_keys[index], point->length);

	return 0;
}

static int zg_set_power(struct net_device *dev, struct iw_request_info *info, struct iw_param *param, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	if (param->disabled) {
		priv->desired_ps_mode = 0;
		priv->ps_wakeup_interval = 4;		// units of beacon intervals
	}
	else {
		priv->desired_ps_mode = 1;

		if (param->flags & IW_POWER_PERIOD) {
			priv->ps_wakeup_interval = (param->value / 1000) / priv->beacon_period;

			if (priv->ps_wakeup_interval == 0)
				priv->ps_wakeup_interval = 1;
		}
	}

	return -EINPROGRESS;
}

static int zg_get_power(struct net_device *dev, struct iw_request_info *info, struct iw_param *param, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	if (priv->ps_mode) {
		param->disabled = 0;

		param->flags = IW_POWER_PERIOD;
		param->value = priv->ps_wakeup_interval * priv->beacon_period * 1000;
		param->flags |= IW_POWER_ALL_R;
	}
	else
		param->disabled = 1;

	return 0;
}

static const iw_handler zg_handler[] =
{
	(iw_handler) zg_config_commit,	/* SIOCSIWCOMMIT */
	(iw_handler) zg_get_name,		/* SIOCGIWNAME */
	(iw_handler) NULL,				/* SIOCSIWNWID */
	(iw_handler) NULL,				/* SIOCGIWNWID */
	(iw_handler) zg_set_freq,		/* SIOCSIWFREQ */
	(iw_handler) zg_get_freq,		/* SIOCGIWFREQ */
	(iw_handler) zg_set_mode,		/* SIOCSIWMODE */
	(iw_handler) zg_get_mode,		/* SIOCGIWMODE */
	(iw_handler) NULL,				/* SIOCSIWSENS */
	(iw_handler) NULL,				/* SIOCGIWSENS */
	(iw_handler) NULL,				/* SIOCSIWRANGE */
	(iw_handler) zg_get_range,		/* SIOCGIWRANGE */
	(iw_handler) NULL,				/* SIOCSIWPRIV */
	(iw_handler) NULL,				/* SIOCGIWPRIV */
	(iw_handler) NULL,				/* SIOCSIWSTATS */
	(iw_handler) NULL,				/* SIOCGIWSTATS */
	(iw_handler) NULL,				/* SIOCSIWSPY */
	(iw_handler) NULL,				/* SIOCGIWSPY */
	(iw_handler) NULL,				/* reserved */
	(iw_handler) NULL,				/* reserved */
	(iw_handler) zg_set_wap,		/* SIOCSIWAP */
	(iw_handler) zg_get_wap,		/* SIOCGIWAP */
	(iw_handler) NULL,				/* reserved */
	(iw_handler) NULL,				/* SIOCGIWAPLIST */
	(iw_handler) zg_set_scan,		/* SIOCSIWSCAN */
	(iw_handler) zg_get_scan,		/* SIOCGIWSCAN */
	(iw_handler) zg_set_essid,		/* SIOCSIWESSID */
	(iw_handler) zg_get_essid,		/* SIOCGIWESSID */
	(iw_handler) NULL,				/* SIOCSIWNICKN */
	(iw_handler) NULL,				/* SIOCGIWNICKN */
	(iw_handler) NULL,				/* reserved */
	(iw_handler) NULL,				/* reserved */
	(iw_handler) zg_set_rate,		/* SIOCSIWRATE */
	(iw_handler) zg_get_rate,		/* SIOCGIWRATE */
	(iw_handler) zg_set_rts,		/* SIOCSIWRTS */
	(iw_handler) zg_get_rts,		/* SIOCGIWRTS */
	(iw_handler) NULL,				/* SIOCSIWFRAG */
	(iw_handler) NULL,				/* SIOCGIWFRAG */
	(iw_handler) NULL,				/* SIOCSIWTXPOW */
	(iw_handler) NULL,				/* SIOCGIWTXPOW */
	(iw_handler) zg_set_retry,		/* SIOCSIWRETRY */
	(iw_handler) zg_get_retry,		/* SIOCGIWRETRY */
	(iw_handler) zg_set_encode,		/* SIOCSIWENCODE */
	(iw_handler) zg_get_encode,		/* SIOCGIWENCODE */
	(iw_handler) zg_set_power,		/* SIOCSIWPOWER */
	(iw_handler) zg_get_power,		/* SIOCGIWPOWER */
	(iw_handler) NULL,				/* reserved */
	(iw_handler) NULL,				/* reserved */
	(iw_handler) NULL,				/* SIOCSIWGENIE */
	(iw_handler) NULL,				/* SIOCGIWGENIE */
	(iw_handler) NULL,				/* SIOCSIWAUTH */
	(iw_handler) NULL,				/* SIOCGIWAUTH */
	(iw_handler) NULL,				/* SIOCSIWENCODEEXT */
	(iw_handler) NULL,				/* SIOCGIWENCODEEXT */
	(iw_handler) NULL,				/* SIOCSIWPMKSA */
};

static int zg_priv_set_debug(struct net_device *dev, struct iw_request_info *info, union iwreq_data *wrqu, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);
	int input;

	input = ((int *)extra)[0];

	switch (input) {
		case 0:
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
			zg_debug_level = input;		// set debug verbosity
			break;
		case 6:
			// retrieve state info
			INIT_COMPLETION(priv->done);
			ZG_PRINT(1, "Reading Stat counters\n");
			zg_get_stat_counters(priv);
			wait_for_completion_interruptible(&priv->done);

			printk("Chip counter info   :\n");
			printk("WEP exclude         : %08x\n", ZGTOHL(priv->stat_cntr.WEP_exclude));
			printk("TX bytes            : %08x\n", ZGTOHL(priv->stat_cntr.tx_bytes));
			printk("TX multicast        : %08x\n", ZGTOHL(priv->stat_cntr.tx_multicast));
			printk("TX failed           : %08x\n", ZGTOHL(priv->stat_cntr.tx_failed));
			printk("TX retry            : %08x\n", ZGTOHL(priv->stat_cntr.tx_retry));
			printk("TX multiple retry   : %08x\n", ZGTOHL(priv->stat_cntr.tx_multiple_retry));
			printk("TX success          : %08x\n", ZGTOHL(priv->stat_cntr.tx_success));
			printk("RX dup              : %08x\n", ZGTOHL(priv->stat_cntr.rx_dup));
			printk("RX CTS success      : %08x\n", ZGTOHL(priv->stat_cntr.rx_cts_success));
			printk("RX CTS fail         : %08x\n", ZGTOHL(priv->stat_cntr.rx_cts_fail));
			printk("RX ack fail         : %08x\n", ZGTOHL(priv->stat_cntr.rx_ack_fail));
			printk("RX bytes            : %08x\n", ZGTOHL(priv->stat_cntr.rx_bytes));
			printk("RX frag             : %08x\n", ZGTOHL(priv->stat_cntr.rx_frag));
			printk("RX multicast        : %08x\n", ZGTOHL(priv->stat_cntr.rx_multicast));
			printk("RX FCS error        : %08x\n", ZGTOHL(priv->stat_cntr.rx_FCS_error));
			printk("RX WEP undecrypt    : %08x\n", ZGTOHL(priv->stat_cntr.rx_WEP_undecrypt));
			printk("RX Frag aged        : %08x\n", ZGTOHL(priv->stat_cntr.rx_frag_aged));
			printk("RX MIC failure      : %08x\n", ZGTOHL(priv->stat_cntr.rx_MIC_failure));

			break;
		case 7:
			printk("State info    :\n");
			printk("Chip state    : %d\n", priv->chip_state);
			printk("Station state : %d\n", priv->station_state);
			printk("TX red ID     : %d\n", priv->data_req_id);
			break;
		case 99:
			printk("iwpriv help:\n");
			printk("0 - 5	: set debug verbosity\n");
			printk("6		: print device stat counter values\n");
			printk("7		: print driver state\n");
			printk("99		: print this help info\n");
		default:
			break;
	}

	return 0;
}

static int zg_priv_set_passphrase(struct net_device *dev, struct iw_request_info *info, union iwreq_data *data, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	memcpy(priv->wpa_passphrase, extra, data->data.length - 1);
	priv->wpa_passphrase_len = data->data.length - 1;

	priv->configured_security_type = ZG_SECURITY_TYPE_WPA2;

	return 0;
}

static int zg_priv_set_sec_mode(struct net_device *dev, struct iw_request_info *info, union iwreq_data *data, char *extra)
{
	struct zg_private *priv = netdev_priv(dev);

	priv->configured_security_type = ((int *)extra)[0];

	return 0;
}

enum {
	PRIV_WX_SET_DEBUG  = SIOCIWFIRSTPRIV,
	PRIV_WX_SET_PASSPHRASE = SIOCIWFIRSTPRIV + 2,
	PRIV_WX_SET_SEC_MODE = SIOCIWFIRSTPRIV + 4,
};

static const iw_handler zg_private_handler[] =
{
	(iw_handler) zg_priv_set_debug,				/* SIOCIWFIRSTPRIV */
	(iw_handler) NULL,
	(iw_handler) zg_priv_set_passphrase,
	(iw_handler) NULL,
	(iw_handler) zg_priv_set_sec_mode,
};

static const struct iw_priv_args zg_private_args[] = {
	{
		PRIV_WX_SET_DEBUG,
		IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
		0,
		"set_debug"
	},
	{
		PRIV_WX_SET_PASSPHRASE,
		IW_PRIV_TYPE_CHAR | 64,
		0,
		"set_passphrase"
	},
	{
		PRIV_WX_SET_SEC_MODE,
		IW_PRIV_TYPE_INT | IW_PRIV_SIZE_FIXED | 1,
		0,
		"set_sec_mode"
	},
};

static const struct iw_handler_def zg_handler_def =
{
	.num_standard	= ARRAY_SIZE(zg_handler),
	.num_private	= ARRAY_SIZE(zg_private_handler),
	.num_private_args = ARRAY_SIZE(zg_private_args),
	.standard	= (iw_handler *) zg_handler,
	.private	= (iw_handler *) zg_private_handler,
	.private_args	= (struct iw_priv_args *) zg_private_args,
	.get_wireless_stats = zg_get_wireless_stats
};

/*
 * Start of driver functions
 */

static int zg_spi_init(struct spi_device *spi)
{
	int err;

	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;

	err = spi_setup(spi);
	if (err < 0)
		return err;

	return 0;
}

struct net_device *zg_init(unsigned short irq, struct spi_device *spi)
{
	struct net_device *dev;
	struct zg_private *priv;
	int rc;

	// Create the network device object
	dev = alloc_etherdev(sizeof(*priv));
	if (!dev) {
		printk(KERN_ERR "g2100: Couldn't alloc_etherdev\n");
		return NULL;
	}
	if (dev_alloc_name(dev, dev->name) < 0) {
		printk(KERN_ERR "g2100: Couldn't get name!\n");
		goto err_out_free;
	}

	priv = netdev_priv(dev);
	priv->dev = dev;
	priv->spi = spi;

	// variable initializations
	// variables to hold network statistics
	memset(&priv->stats, 0, sizeof(priv->stats));
	memset(&priv->wstats, 0, sizeof(priv->wstats));

	// Allocate DMA buffers
	priv->tx_dma_buf = priv->rx_dma_buf = 0;
	priv->tx_dma_buf = dma_alloc_coherent(spi->master->cdev.dev, ZG_BUFFER_SIZE, &priv->tx_dma, GFP_KERNEL);
	priv->rx_dma_buf = dma_alloc_coherent(spi->master->cdev.dev, ZG_BUFFER_SIZE, &priv->rx_dma, GFP_KERNEL);

	if ( !(priv->tx_dma_buf || priv->rx_dma_buf) ) {
		printk(KERN_ERR "g2100: Could not allocate DMA buffers!\n");
		goto err_out_free;
	}
	else {
		priv->tx_buf = (u8*)priv->tx_dma_buf;
		priv->rx_buf = (u8*)priv->rx_dma_buf;
	}

	// using the universal buffer
	priv->com_cxt.cmd_buf = &(priv->tx_buf[0]);
	priv->com_cxt.tx_reg_buf = &(priv->tx_buf[1]);
	priv->com_cxt.status_buf = &(priv->rx_buf[0]);
	priv->com_cxt.rx_reg_buf = &(priv->rx_buf[1]);
	priv->com_cxt.tx_msg_buf = &(priv->tx_buf[ZG_PREAMBLE_LEN]);
	priv->com_cxt.rx_msg_buf = &(priv->rx_buf[ZG_PREAMBLE_LEN]);

	// sync stuff
	// initialize the device lock used to lock buffers for SPI operations
	spin_lock_init(&priv->lock);
	priv->device_busy = 0;
	init_completion(&priv->done);
	init_completion(&priv->spi_done);

	// interrupt stuff
	INIT_WORK(&priv->zg_int_wrk_q, zg_interrupt_bh);

	// associate and maintain
	INIT_WORK(&priv->zg_conn_wrk_q, zg_conn_handler);

	// data transfer management
	INIT_WORK(&priv->zg_data_wrk_q, zg_data_handler);

	// testing workqueue
	INIT_WORK(&priv->zg_mgmt_wrk_q, zg_mgmt_handler);

	// config parameters; set default values which can be modified later
	priv->channel = 1;
	priv->reg_domain = ZG_REG_DOMAIN_FCC;
	priv->iw_mode = IW_MODE_INFRA;
	priv->preamble = ZG_LONG_PREAMBLE;
	priv->essid[0] = '\0';
	priv->essid_len = 0;
	priv->desired_essid[0] = '\0';
	priv->d_essid_len = 0;
	priv->specific_essid = 0;

	// setup ssid from cmdline params if specified
	if (modparam_essid_len != 0) {
		memcpy(priv->essid, modparam_essid, modparam_essid_len);
		priv->essid_len = modparam_essid_len;
		memcpy(priv->desired_essid, modparam_essid, modparam_essid_len);
		priv->d_essid_len = modparam_essid_len;

		priv->specific_essid = 1;
	}

	memset(priv->bssid, 0, ETH_ALEN);
	priv->new_bssid = 0;

	// more default config parameters for the device
	priv->tx_rate = 0;				// setup for auto data rate selection
	priv->rts_threshold = 2347;
	priv->frag_threshold = 2346;
	priv->short_retry = 7;			// retry limits for the device
	priv->long_retry = 4;

	priv->beacon_period = 100;
	priv->ps_mode = 0;				// power save mode disabled
	priv->sleep_state = ZG_SLEEP_DISABLE;	// sleep state disabled
	priv->desired_ps_mode = 0;
#ifdef ZG_CONN_SIMPLIFIED
	priv->ps_wakeup_interval = 0;	// units of beacon intervals
#else
	priv->ps_wakeup_interval = 4;	// units of beacon intervals
#endif

	priv->security_type = ZG_SECURITY_TYPE_NONE;
	priv->configured_security_type = modparam_configured_security;

	// clear the WEP keys
	priv->default_key = 0;
	memset(priv->wep_keys, 0, sizeof(priv->wep_keys));
	memset(priv->wep_key_len, 0, sizeof(priv->wep_key_len));

	// load the passphrase if specified
	memcpy(priv->wpa_passphrase, modparam_passphrase, modparam_passphrase_len);
	priv->wpa_passphrase_len = modparam_passphrase_len;

	// station state
	priv->station_state = STATION_STATE_DOWN;

	// multicast
	memset(priv->mc_list, 0, sizeof(priv->mc_list));
	priv->mc_list_count = ZG_MULTICAST_SLOTS_AVAIL;		// total slots available for multicast addresses in driver
														// the device only support two multicast addresses at a time
	priv->mc_list_idx = 0;
	spin_lock_init(&priv->mc_lock);						// lock to synchronize writing the multicast addresses to the device
	priv->mc_running = 0;

	// misc
	priv->scan_notify = 0;					// do not notify the scan results to user
	priv->auto_tx_rate = 1;					// TX rate throttling enabled
	priv->chip_state = CHIP_STATE_DOWN;
	priv->adhoc_role = ZG_ADHOC_ROLE_JOINER;
	priv->auth_mode = ZG_AUTH_ALG_OPEN;		// configured for open authentication
	priv->trial_num = 1;					// trial number of authentication process
	priv->data_req_id = 0;
	priv->disconnect = 0;
	priv->fifo_byte_cnt = 0;				// available buffer space
	priv->write_buffer_size = 0;			// size of the write buffer chunks
	priv->conn_trial = 0;
	priv->reconn_on_disconn = 1;			// reconnect if device gets disconnected from network
	priv->txpacket_waiting = 0;

	dev->open = zg_open;
	dev->stop = zg_close;
	dev->change_mtu = zg_change_mtu;
	dev->set_mac_address = zg_set_mac_address;
	dev->hard_start_xmit = start_tx;
	dev->get_stats = zg_get_stats;
	dev->wireless_handlers = (struct iw_handler_def *)&zg_handler_def;
	dev->do_ioctl = zg_ioctl;
	dev->irq = irq;
	dev->set_multicast_list = zg_set_multicast_list;

	SET_NETDEV_DEV(dev, &spi->dev);

	ZG_PRINT(1, "%s: resetting device\n", dev->name);
	if ( !zg_hw_reset(dev) ) {
		printk(KERN_ERR "%s: reset failed\n", dev->name);
		goto err_out_free;
	}

	if ((rc = request_irq(dev->irq, zg_interrupt, IRQF_TRIGGER_LOW, dev->name, dev))) {
		printk(KERN_ERR "%s: register interrupt %d failed, rc %d\n", dev->name, irq, rc);
		goto err_out_free;
	}

	if (register_netdev(dev))
		goto err_out_irq;

	memset(dev->dev_addr, 0, 6);

	SET_MODULE_OWNER(dev);
	return dev;

err_out_irq:
	free_irq(dev->irq, dev);
err_out_free:
	free_netdev(dev);
	return NULL;
}

void zg_stop(struct net_device *dev)
{
	struct zg_private *priv = netdev_priv(dev);

	if (priv->tx_dma_buf)
		dma_free_coherent(priv->spi->master->cdev.dev, ZG_BUFFER_SIZE, priv->tx_dma_buf, priv->tx_dma);

	if (priv->rx_dma_buf)
		dma_free_coherent(priv->spi->master->cdev.dev, ZG_BUFFER_SIZE, priv->rx_dma_buf, priv->rx_dma);

	unregister_netdev(dev);
	free_irq(dev->irq, dev);
	free_netdev(dev);
}

static int __devinit zg_probe(struct spi_device *spi)
{
	struct net_device *dev;
	struct zg_private *priv;
	int err;

	if ( (err = zg_spi_init(spi)) < 0 ) {
		printk(KERN_ERR "SPI init error\n");
		return err;
	}

	// initialize all the default parameters and reset the HW
	dev = zg_init(ZG_INTERRUPT_PIN, spi);
	if (!dev)
		return -ENODEV;

	spi_set_drvdata(spi, dev);

	priv = netdev_priv(dev);

	priv->chip_state = CHIP_STATE_RESET_DONE;

	// read and update the write buffer space on the device
	zg_read_chip_info_block(priv);

	// Get MAC address from HW
	INIT_COMPLETION(priv->done);
	ZG_PRINT(1, "Reading MAC\n");
	zg_get_mac_address(priv);
	wait_for_completion_interruptible(&priv->done);
	
	// Get Region domain from HW
	INIT_COMPLETION(priv->done);
	ZG_PRINT(1, "Reading Reg domain\n");
	zg_get_reg_domain(priv);
	wait_for_completion_interruptible(&priv->done);

	// Get Chip Version
	INIT_COMPLETION(priv->done);
	ZG_PRINT(1, "Reading Chip Version\n");
	zg_get_chip_version(priv);
	wait_for_completion_interruptible(&priv->done);

	// Write Missed Beacon Threshold
	//INIT_COMPLETION(priv->done);
	//ZG_PRINT(1, "Writing Missed Beacon Threshold\n");
	//zg_set_beacon_miss_thr(priv, 100);
	//wait_for_completion_interruptible(&priv->done);

	printk(KERN_INFO "%s: ZeroG G2100. Chip Version %04x. Driver Version %d.%d. MAC %.2x:%.2x:%.2x:%.2x:%.2x:%.2x\n",
	       dev->name, priv->chip_version, DRIVER_MAJOR, DRIVER_MINOR,
	       dev->dev_addr[0], dev->dev_addr[1], dev->dev_addr[2],
	       dev->dev_addr[3], dev->dev_addr[4], dev->dev_addr[5] );

	priv->chip_state = CHIP_STATE_READY;

	return 0;
}

static void __devexit zg_remove(struct spi_device *spi)
{
	zg_stop(spi_get_drvdata(spi));
}

static struct spi_driver zg_driver = {
	.driver = {
		.name		= "g2100",
		.bus		= &spi_bus_type,
		.owner		= THIS_MODULE,
	},
	.probe	= zg_probe,
	.remove	= __devexit_p(zg_remove),
};

static int __init zg_init_module(void)
{
	ZG_PRINT(1, "G2100 Initialized\n");
	return spi_register_driver(&zg_driver);
}

static void __exit zg_cleanup_module(void)
{
	ZG_PRINT(1, "G2100 Unloaded\n");
	spi_unregister_driver(&zg_driver);
}

module_init(zg_init_module);
module_exit(zg_cleanup_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ZeroG");
MODULE_DESCRIPTION("G2100 Driver");
