/*
 * Minion EtherTAP Linux driver for the Minion Ethernet TAP device.
 *
 * This is an experimental driver which is based on the original emac_lite
 * driver from John Williams <john.williams@xilinx.com>.
 *
 * 2007 - 2013 (c) Xilinx, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */
/*
 * Copyright (C) 2015 Microchip Technology
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/phy.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/phy.h>
#include <linux/interrupt.h>
#include <linux/timer.h>
#include <linux/spinlock.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/platform_data/mdio-gpio.h>
#include "lowrisc_100MHz.h"

#define DRIVER_AUTHOR	"WOOJUNG HUH <woojung.huh@microchip.com>"
#define DRIVER_DESC	"Microchip LAN8720 PHY driver"
#define DRIVER_NAME "minion_etherTAP"

/* General Ethernet Definitions */
#define XEL_ARP_PACKET_SIZE		28	/* Max ARP packet size */
#define XEL_HEADER_IP_LENGTH_OFFSET	16	/* IP Length Offset */

#define TX_TIMEOUT		(60*HZ)		/* Tx timeout is 60 seconds. */
#define ALIGNMENT		4

/* BUFFER_ALIGN(adr) calculates the number of bytes to the next alignment. */
#define BUFFER_ALIGN(adr) ((ALIGNMENT - ((size_t) adr)) % ALIGNMENT)

struct timer_list tap_timer;

/**
 * struct net_local - Our private per device data
 * @ndev:		instance of the network device
 * @next_tx_buf_to_use:	next Tx buffer to write to
 * @next_rx_buf_to_use:	next Rx buffer to read from
 * @reset_lock:		lock used for synchronization
 * @deferred_skb:	holds an skb (for transmission at a later time) when the
 *			Tx buffer is not free
 * @phy_dev:		pointer to the PHY device
 * @phy_node:		pointer to the PHY device node
 * @mii_bus:		pointer to the MII bus
 * @last_link:		last link status
 */
struct net_local {
  struct mdiobb_ctrl ctrl; /* must be first for bitbang driver to work */
  void __iomem *ioaddr;
  struct pci_dev *pdev;
  struct net_device *dev;
  
  dma_addr_t rx_dma_addr;
  dma_addr_t tx_dma_addr;
  int tx_ring_head, tx_ring_tail;
  int rx_ring_head, rx_ring_tail;
  
  spinlock_t int_lock;
  spinlock_t phy_lock;
  
  struct napi_struct napi;
  
  bool software_irq_signal;
  bool rx_csum;
  u32 msg_enable;
  
  struct phy_device *phy_dev;
  struct mii_bus *mii_bus;
  int last_duplex;
  int last_carrier;
  
  struct net_device *ndev;
  
  u32 next_tx_buf_to_use;
  u32 next_rx_buf_to_use;
  
  spinlock_t reset_lock;
  struct sk_buff *deferred_skb;
  
  int last_link;
  /* Spinlock */
  spinlock_t lock;
  spinlock_t stats_lock;
  /* Counter */
  int count;
  
};

static volatile unsigned int *eth_base;

static void axi_write(size_t addr, int data)
{
  if (addr < 0x2000)
    eth_base[addr >> 2] = data;
  else
    printk("axi_write(%lx,%x) out of range\n", addr, data);
}

static int axi_read(size_t addr)
{
  if (addr < 0x2000)
    return eth_base[addr >> 2];
  else
    printk("axi_read(%lx) out of range\n", addr);
  return -1;
}

/**
 * minion_send_data - Send an Ethernet frame
 * @drvdata:	Pointer to the EtherTAP device private data
 * @data:	Pointer to the data to be sent
 * @byte_count:	Total frame size, including header
 *
 * This function checks if the Tx buffer of the EtherTAP device is free to send
 * data. If so, it fills the Tx buffer with data for transmission. Otherwise, it
 * returns an error.
 *
 * Return:	0 upon success or -1 if the buffer(s) are full.
 *
 * Note:	The maximum Tx packet size can not be more than Ethernet header
 *		(14 Bytes) + Maximum MTU (1500 bytes). This is excluding FCS.
 */
static int minion_send_data(struct net_local *drvdata, u8 *data,
			       unsigned int byte_count)
{
  uint32_t *alloc = (uint32_t *)data;
  int i, rslt = axi_read(TPLR_OFFSET);
  printk("TX Status = %x\n", rslt);
  if (rslt & TPLR_BUSY_MASK) return -1;
  for (i = 0; i < (((byte_count-1)|3)+1)/4; i++)
    {
      axi_write(TXBUFF_OFFSET+(i<<2), alloc[i]);
    }
  axi_write(TPLR_OFFSET,byte_count);
  printk("send, len=%X\n", byte_count);
  return 0;
}

/**
 * minion_recv_data - Receive a frame
 * @drvdata:	Pointer to the EtherTAP device private data
 * @data:	Address where the data is to be received
 *
 * This function is intended to be called from the interrupt context or
 * with a wrapper which waits for the receive frame to be available.
 *
 * Return:	Total number of bytes received
 */

static u16 minion_recv_data(struct net_local *drvdata, u8 *data)
{
  int i;
  int fcs = axi_read(RFCS_OFFSET);
  int rplr = axi_read(RPLR_OFFSET);
  int length = (rplr & RPLR_LENGTH_MASK) >> 16;
  if ((length >= 14) && (fcs == 0xc704dd7b))
    {
      int rnd;
      uint32_t *alloc;
      length -= 4; /* discard FCS bytes */
      rnd = (((length-1)|3)+1); /* round to a multiple of 4 */
      alloc = (uint32_t *)data;
      for (i = 0; i < rnd/4; i++)
        {
          alloc[i] = axi_read(RXBUFF_OFFSET+(i<<2));
        }
      printk("recv, len=%X\n", length);
    }
  else
    length = 0;
  axi_write(RSR_OFFSET, 0); /* acknowledge, even if an error occurs, to reset irq */
  return length;
}

/**
 * minion_update_address - Update the MAC address in the device
 * @drvdata:	Pointer to the EtherTAP device private data
 * @address_ptr:Pointer to the MAC address (MAC address is a 48-bit value)
 *
 * Tx must be idle and Rx should be idle for deterministic results.
 * It is recommended that this function should be called after the
 * initialization and before transmission of any packets from the device.
 * The MAC address can be programmed using any of the two transmit
 * buffers (if configured).
 */

static void minion_update_address(struct net_local *drvdata, u8 *address_ptr)
{
  uint32_t macaddr_lo, macaddr_hi;
  memcpy (&macaddr_lo, address_ptr+2, sizeof(uint32_t));
  memcpy (&macaddr_hi, address_ptr+0, sizeof(uint16_t));
  axi_write(MACLO_OFFSET, htonl(macaddr_lo));
  axi_write(MACHI_OFFSET, MACHI_IRQ_EN|MACHI_ALLPACKETS_MASK|MACHI_DATA_DLY_MASK|MACHI_COOKED_MASK|htons(macaddr_hi));
}

/**
 * minion_set_mac_address - Set the MAC address for this device
 * @dev:	Pointer to the network device instance
 * @addr:	Void pointer to the sockaddr structure
 *
 * This function copies the HW address from the sockaddr strucutre to the
 * net_device structure and updates the address in HW.
 *
 * Return:	Error if the net device is busy or 0 if the addr is set
 *		successfully
 */
static int minion_set_mac_address(struct net_device *dev, void *address)
{
	struct net_local *lp = netdev_priv(dev);
	struct sockaddr *addr = address;
	
	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);
	minion_update_address(lp, dev->dev_addr);
	return 0;
}

/**
 * minion_tx_timeout - Callback for Tx Timeout
 * @dev:	Pointer to the network device
 *
 * This function is called when Tx time out occurs for EtherTAP device.
 */
static void minion_tx_timeout(struct net_device *dev)
{
	struct net_local *lp = netdev_priv(dev);
	unsigned long flags;

	dev_err(&lp->ndev->dev, "Exceeded transmit timeout of %lu ms\n",
		TX_TIMEOUT * 1000UL / HZ);

	dev->stats.tx_errors++;

	/* Reset the device */
	spin_lock_irqsave(&lp->reset_lock, flags);

	/* Shouldn't really be necessary, but shouldn't hurt */
	netif_stop_queue(dev);

	if (lp->deferred_skb) {
		dev_kfree_skb(lp->deferred_skb);
		lp->deferred_skb = NULL;
		dev->stats.tx_errors++;
	}

	/* To exclude tx timeout */
	dev->trans_start = jiffies; /* prevent tx timeout */

	/* We're all ready to go. Start the queue */
	netif_wake_queue(dev);
	spin_unlock_irqrestore(&lp->reset_lock, flags);
}

/**********************/
/* Interrupt Handlers */
/**********************/

/**
 * minion_tx_handler - Interrupt handler for frames sent
 * @dev:	Pointer to the network device
 *
 * This function updates the number of packets transmitted and handles the
 * deferred skb, if there is one.
 */
static void minion_tx_handler(struct net_device *dev)
{
	struct net_local *lp = netdev_priv(dev);

	dev->stats.tx_packets++;
	if (lp->deferred_skb) {
		if (minion_send_data(lp,
					(u8 *) lp->deferred_skb->data,
					lp->deferred_skb->len) != 0)
			return;
		else {
			dev->stats.tx_bytes += lp->deferred_skb->len;
			dev_kfree_skb_irq(lp->deferred_skb);
			lp->deferred_skb = NULL;
			dev->trans_start = jiffies; /* prevent tx timeout */
			netif_wake_queue(dev);
		}
	}
}

/**
 * minion_rx_handler- Interrupt handler for frames received
 * @dev:	Pointer to the network device
 *
 * This function allocates memory for a socket buffer, fills it with data
 * received and hands it over to the TCP/IP stack.
 */
static void minion_rx_handler(struct net_device *ndev)
{
	u32 len = ETH_FRAME_LEN + ETH_FCS_LEN;
	struct net_local *lp = netdev_priv(ndev);
	static struct sk_buff *skb;
	unsigned int align;
	skb = netdev_alloc_skb(ndev, len + ALIGNMENT);
	if (!skb) {
		/* Couldn't get memory. */
		ndev->stats.rx_dropped++;
		dev_err(&lp->ndev->dev, "Could not allocate receive buffer\n");
		return;
	}

	/*
	 * A new skb should have the data halfword aligned, but this code is
	 * here just in case that isn't true. Calculate how many
	 * bytes we should reserve to get the data to start on a word
	 * boundary */
	align = BUFFER_ALIGN(skb->data);
	if (align)
		skb_reserve(skb, align);

	skb_reserve(skb, 2);

	len = minion_recv_data(lp, (u8 *) skb->data);

	if (!len) {
		ndev->stats.rx_errors++;
		dev_kfree_skb_irq(skb);
		return;
	}

	skb_put(skb, len);	/* Tell the skb how much data we got */

	skb->protocol = eth_type_trans(skb, ndev);
	skb_checksum_none_assert(skb);

	ndev->stats.rx_packets++;
	ndev->stats.rx_bytes += len;

	if (!skb_defer_rx_timestamp(skb))
		netif_rx(skb);	/* Send the packet upstream */
}

/**
 * minion_open - Open the network device
 * @dev:	Pointer to the network device
 *
 * This function sets the MAC address, requests an IRQ and enables interrupts
 * for the EtherTAP device and starts the Tx queue.
 * It also connects to the phy device, if MDIO is included in EtherTAP device.
 */
static int minion_open(struct net_device *dev)
{
	struct net_local *lp = netdev_priv(dev);

	if (lp->phy_dev) {
		/* EtherTAP doesn't support giga-bit speeds */
		lp->phy_dev->supported &= (PHY_BASIC_FEATURES);
		lp->phy_dev->advertising = lp->phy_dev->supported;

		phy_start(lp->phy_dev);
	}

	/* Set the MAC address each time opened */
	minion_update_address(lp, dev->dev_addr);

	/* We're ready to go */
	netif_start_queue(dev);

	return 0;
}

/**
 * minion_close - Close the network device
 * @dev:	Pointer to the network device
 *
 * This function stops the Tx queue, disables interrupts and frees the IRQ for
 * the EtherTAP device.
 * It also disconnects the phy device associated with the EtherTAP device.
 */
static int minion_close(struct net_device *dev)
{
	struct net_local *lp = netdev_priv(dev);

	netif_stop_queue(dev);
	free_irq(dev->irq, dev);

	if (lp->phy_dev)
		phy_disconnect(lp->phy_dev);
	lp->phy_dev = NULL;

	return 0;
}

/**
 * minion_send - Transmit a frame
 * @orig_skb:	Pointer to the socket buffer to be transmitted
 * @dev:	Pointer to the network device
 *
 * This function checks if the Tx buffer of the EtherTAP device is free to send
 * data. If so, it fills the Tx buffer with data from socket buffer data,
 * updates the stats and frees the socket buffer. The Tx completion is signaled
 * by an interrupt. If the Tx buffer isn't free, then the socket buffer is
 * deferred and the Tx queue is stopped so that the deferred socket buffer can
 * be transmitted when the EtherTAP device is free to transmit data.
 *
 * Return:	0, always.
 */
static int minion_send(struct sk_buff *orig_skb, struct net_device *dev)
{
	struct net_local *lp = netdev_priv(dev);
	struct sk_buff *new_skb;
	unsigned int len;
	unsigned long flags;

	len = orig_skb->len;

	new_skb = orig_skb;

	spin_lock_irqsave(&lp->reset_lock, flags);
	if (minion_send_data(lp, (u8 *) new_skb->data, len) != 0) {
		/* If the EtherTAP Tx buffer is busy, stop the Tx queue and
		 * defer the skb for transmission during the ISR, after the
		 * current transmission is complete */
		netif_stop_queue(dev);
		lp->deferred_skb = new_skb;
		/* Take the time stamp now, since we can't do this in an ISR. */
		skb_tx_timestamp(new_skb);
		spin_unlock_irqrestore(&lp->reset_lock, flags);
		return 0;
	}
	spin_unlock_irqrestore(&lp->reset_lock, flags);

	skb_tx_timestamp(new_skb);

	dev->stats.tx_bytes += len;
	dev_consume_skb_any(new_skb);

	return 0;
}

/**
 * minion_remove_ndev - Free the network device
 * @ndev:	Pointer to the network device to be freed
 *
 * This function un maps the IO region of the EtherTAP device and frees the net
 * device.
 */
static void minion_remove_ndev(struct net_device *ndev)
{
	if (ndev) {
		free_netdev(ndev);
	}
}

static void minion_poll_controller(struct net_device *dev)
{
	bool tx_complete = false;
	struct net_local *lp = netdev_priv(dev);
	u32 tx_status;
	spin_lock(&(lp->lock));

	/* Check if there is Rx Data available */
	if (axi_read(RSR_OFFSET) & RSR_RECV_DONE_MASK)
	  {
	    minion_rx_handler(dev);
	  }
	/* Check if the Transmission is completed */
	tx_status = 0;
	if (tx_status)
		tx_complete = true;

	/* If there was a Tx interrupt, call the Tx Handler */
	if (tx_complete != 0)
		minion_tx_handler(dev);
	lp->count++;
	spin_unlock(&(lp->lock));
}

enum {inc=HZ}; // 1Hz

static struct net_device_ops minion_netdev_ops;

/* Timer callback */
void tap_timer_callback(unsigned long arg)
{
  struct net_device *ndev = (struct net_device *)arg;
  minion_poll_controller(ndev);
  
  mod_timer(&tap_timer, jiffies + inc); /* restarting timer */
}

static void tap_init_timer(struct net_device *ndev)
{
  init_timer(&tap_timer);
  tap_timer.function = tap_timer_callback;
  tap_timer.data = (unsigned long)ndev;
  tap_timer.expires = jiffies + inc;
  add_timer(&tap_timer); /* Starting the timer */
  
  printk(KERN_INFO "tap_timer is started\n");
}

static uint16_t mdio_regs_cache[32];

static int smsc_mii_read(struct mii_bus *bus, int phyaddr, int regidx)
{
	struct net_local *pd = (struct net_local *)bus->priv;
	int reg = -EIO;

	spin_lock(&pd->phy_lock);

	reg = mdio_regs_cache[regidx];

	spin_unlock(&pd->phy_lock);
	return reg;
}

static int smsc_mii_write(struct mii_bus *bus, int phyaddr, int regidx,
			   u16 val)
{
	struct net_local *pd = (struct net_local *)bus->priv;

	spin_lock(&pd->phy_lock);

	mdio_regs_cache[regidx] = val;
	
	spin_unlock(&pd->phy_lock);

	return 0;
}

static void smsc_phy_adjust_link(struct net_device *dev)
{
	struct net_local *pd = netdev_priv(dev);
	struct phy_device *phy_dev = pd->phy_dev;
	int carrier;

	if (phy_dev->duplex != pd->last_duplex) {
		if (phy_dev->duplex) {
			netif_dbg(pd, link, pd->dev, "full duplex mode\n");
		} else {
			netif_dbg(pd, link, pd->dev, "half duplex mode\n");
		}

		pd->last_duplex = phy_dev->duplex;
	}

	carrier = netif_carrier_ok(dev);
	if (carrier != pd->last_carrier) {
		if (carrier)
			netif_dbg(pd, link, pd->dev, "carrier OK\n");
		else
			netif_dbg(pd, link, pd->dev, "no carrier\n");
		pd->last_carrier = carrier;
	}
}

static int smsc_mii_probe(struct net_device *dev)
{
	struct net_local *pd = netdev_priv(dev);
	struct phy_device *phydev = NULL;
	const char *phyname;
	
	BUG_ON(pd->phy_dev);

	/* Device only supports internal PHY at address 1 */
	phydev = mdiobus_get_phy(pd->mii_bus, 1);
	if (!phydev) {
		netdev_err(dev, "no PHY found at address 1\n");
		return -ENODEV;
	}

	phyname = phydev_name(phydev);
	printk("Probing %s\n", phyname);
	
	phydev = phy_connect(dev, phyname,
			     smsc_phy_adjust_link, PHY_INTERFACE_MODE_MII);

	if (IS_ERR(phydev)) {
		netdev_err(dev, "Could not attach to PHY\n");
		return PTR_ERR(phydev);
	}

	/* mask with MAC supported features */
	phydev->supported &= (PHY_BASIC_FEATURES | SUPPORTED_Pause |
			      SUPPORTED_Asym_Pause);
	phydev->advertising = phydev->supported;

	phy_attached_info(phydev);

	pd->phy_dev = phydev;
	pd->last_duplex = -1;
	pd->last_carrier = -1;

	return 0;
}
static uint32_t last_gpio;

static void mdio_dir(struct mdiobb_ctrl *ctrl, int dir)
{
  if (dir)
    last_gpio |= 1 << 2;
  else
    last_gpio &= ~ (1 << 2);
    
  axi_write(MDIOCTRL_OFFSET, last_gpio);
}

static int mdio_get(struct mdiobb_ctrl *ctrl)
{
  uint32_t rslt = axi_read(MDIOCTRL_OFFSET);
  return rslt >> 3;
}

static void mdio_set(struct mdiobb_ctrl *ctrl, int what)
{
  if (what)
    last_gpio |= 1 << 1;
  else
    last_gpio &= ~ (1 << 1);
    
  axi_write(MDIOCTRL_OFFSET, last_gpio);
}

static void mdc_set(struct mdiobb_ctrl *ctrl, int what)
{
  if (what)
    last_gpio |= 1 << 0;
  else
    last_gpio &= ~ (1 << 0);
    
  axi_write(MDIOCTRL_OFFSET, last_gpio);
}

/* reset callback */
static int minion_reset(struct mii_bus *bus)
{
  return 0;
}

static struct mdiobb_ops mdio_gpio_ops = {
        .owner = THIS_MODULE,
        .set_mdc = mdc_set,
        .set_mdio_dir = mdio_dir,
        .set_mdio_data = mdio_set,
        .get_mdio_data = mdio_get,
};

static uint16_t mdio_regs_init[32] = {
  /* 0x0 */ 0x3100, // was 0x2100, // was 0x3100,
  /* 0x1 */ 0x782d, // was 0x780d, // was 0x782d,
/* 0x2 */ 0x0007,
/* 0x3 */ 0xc0f1,
  /* 0x4 */ 0x01e1, // was 0x0181, // was 0x1e1,
/* 0x5 */ 0x41e1,
  /* 0x6 */ 0x0001, // was 0x0000, // was 0x0001,
/* 0x7 */ 0xffff,
/* 0x8 */ 0xffff,
/* 0x9 */ 0xffff,
/* 0xa */ 0xffff,
/* 0xb */ 0xffff,
/* 0xc */ 0xffff,
/* 0xd */ 0xffff,
/* 0xe */ 0xffff,
/* 0xf */ 0x0000,
/* 0x10 */ 0x0040,
  /* 0x11 */ 0x0002, // was 0x0256, // was 0x2,
/* 0x12 */ 0x60e1,
/* 0x13 */ 0xffff,
/* 0x14 */ 0x0000,
/* 0x15 */ 0x0000,
/* 0x16 */ 0x0000,
/* 0x17 */ 0x0000,
/* 0x18 */ 0xffff,
/* 0x19 */ 0xffff,
/* 0x1a */ 0x0000,
  /* 0x1b */ 0x000a, // was 0x000b, // was 0xa,
/* 0x1c */ 0x0000,
  /* 0x1d */ 0x00c8, // was 0x00da, // was 0xc8,
/* 0x1e */ 0x0000,
  /* 0x1f */ 0x1058, // was 0x0058, // was 0x1058
};

static int smsc_mii_init(struct net_device *dev)
{
        struct mii_bus *new_bus;
	struct net_local *pd = netdev_priv(dev);
	int err = -ENXIO;
#ifdef BITBANG
	pd->ctrl.ops = &mdio_gpio_ops;
	pd->ctrl.reset = minion_reset;
        new_bus = alloc_mdio_bitbang(&(pd->ctrl));
#else
	new_bus = mdiobus_alloc();
	new_bus->read = smsc_mii_read;
	new_bus->write = smsc_mii_write;
	memcpy(mdio_regs_cache, mdio_regs_init, sizeof(mdio_regs_cache));
#endif	
	if (!new_bus) {
		err = -ENOMEM;
		goto err_out_1;
	}
	snprintf(new_bus->id, MII_BUS_ID_SIZE, "smsc-0");
        new_bus->name = "GPIO Bitbanged SMSC",

        new_bus->phy_mask = ~(1 << 1);
        new_bus->phy_ignore_ta_mask = 0;

	mutex_init(&(new_bus->mdio_lock));
	
	pd->mii_bus = new_bus;
	pd->mii_bus->priv = pd;

	/* Mask all PHYs except ID 1 (internal) */
	pd->mii_bus->phy_mask = ~(1 << 1);

	if (mdiobus_register(pd->mii_bus)) {
		netif_warn(pd, probe, pd->dev, "Error registering mii bus\n");
		goto err_out_free_bus_2;
	}

	if (smsc_mii_probe(dev) < 0) {
		netif_warn(pd, probe, pd->dev, "Error probing mii bus\n");
		goto err_out_unregister_bus_3;
	}

	return 0;

err_out_unregister_bus_3:
	mdiobus_unregister(pd->mii_bus);
err_out_free_bus_2:
	mdiobus_free(pd->mii_bus);
err_out_1:
	return err;
}

irqreturn_t lowrisc_ether_isr(int irq, void *dev_id)
{
  irqreturn_t rc = IRQ_NONE;
  struct net_device *ndev = dev_id;
  struct net_local *lp = netdev_priv(ndev);
  int rxstatus;
  spin_lock(&(lp->lock));
  rxstatus = axi_read(RSR_OFFSET);
  printk("lowrisc_ether_isr(%d,%p); /* status=%d */\n", irq, dev_id, rxstatus);
    
  /* Check if there is Rx Data available */
  if (rxstatus & RSR_RECV_DONE_MASK)
    {
      minion_rx_handler(ndev);
      rc = IRQ_HANDLED;
    }
  lp->count++;
  spin_unlock(&(lp->lock));
  
  return rc;
}

/**
 * minion_of_probe - Probe method for the EtherTAP device.
 * @ofdev:	Pointer to OF device structure
 * @match:	Pointer to the structure used for matching a device
 *
 * This function probes for the EtherTAP device in the device tree.
 * It initializes the driver data structure and the hardware, sets the MAC
 * address and registers the network device.
 * It also registers a mii_bus for the EtherTAP device, if MDIO is included
 * in the device.
 *
 * Return:	0, if the driver is bound to the EtherTAP device, or
 *		a negative error if there is failure.
 */
static int tap_minion_probe(struct platform_device *ofdev)
{
	struct net_device *ndev = NULL;
	struct net_local *lp = NULL;
	struct device *dev = &ofdev->dev;
        struct resource *lowrisc_ethernet = ofdev->resource;
	unsigned char mac_address[7];
	int rc = 0;
        strcpy(mac_address, "\xe0\xe1\xe2\xe3\xe4\xe5");
        lowrisc_ethernet = platform_get_resource(ofdev, IORESOURCE_MEM, 0);
        eth_base = devm_ioremap_resource(&ofdev->dev, lowrisc_ethernet);

	printk("lowrisc-digilent-ethernet: Lowrisc ethernet platform (%llX-%llX) mapped to %p\n",
               lowrisc_ethernet[0].start, lowrisc_ethernet[0].end, eth_base);

	/* Create an ethernet device instance */
	ndev = alloc_etherdev(sizeof(struct net_local));
	if (!ndev)
		return -ENOMEM;

	dev_set_drvdata(dev, ndev);
	SET_NETDEV_DEV(ndev, &ofdev->dev);

	lp = netdev_priv(ndev);
	lp->ndev = ndev;

	spin_lock_init(&lp->reset_lock);
	lp->next_tx_buf_to_use = 0x0;
	lp->next_rx_buf_to_use = 0x0;

	memcpy(ndev->dev_addr, mac_address, ETH_ALEN);

	rc = request_irq(IRQ_SOFTWARE, lowrisc_ether_isr, IRQF_SHARED, "eth0", ndev);
        
	/* Set the MAC address in the EtherTAP device */
	minion_update_address(lp, ndev->dev_addr);

	smsc_mii_init(ndev);

	ndev->netdev_ops = &minion_netdev_ops;
	ndev->flags &= ~IFF_MULTICAST;
	ndev->watchdog_timeo = TX_TIMEOUT;

	/* Finally, register the device */
	rc = register_netdev(ndev);
	if (rc) {
		dev_err(dev,
			"Cannot register network device, aborting\n");
		goto error;
	}

	dev_info(dev, "Minion EtherTAP registered\n");

	tap_init_timer(ndev);
	
	return 0;

error:
	minion_remove_ndev(ndev);
	return rc;
}

s32 minion_read_phy_reg(struct phy_device *phydev, u32 reg_addr, u16 * phy_data)
{
  u16 val = phy_read(phydev, reg_addr);
  *phy_data = val;
  return 0;
}

s32 minion_write_phy_reg(struct phy_device *phydev, u32 reg_addr, u16 data)
{
  return phy_write(phydev, reg_addr, data);
}

static int minion_mii_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
        struct net_local *lp = netdev_priv(netdev);
	struct phy_device *phy = lp->phy_dev;
        struct mii_ioctl_data *data = if_mii(ifr);
        u16 mii_reg;
        unsigned long flags;

        switch (cmd) {
        case SIOCGMIIPHY:
                data->phy_id = 1;
                break;
        case SIOCGMIIREG:
                spin_lock_irqsave(&lp->stats_lock, flags);
                if (minion_read_phy_reg(phy, data->reg_num & 0x1F,
                                   &data->val_out)) {
                        spin_unlock_irqrestore(&lp->stats_lock, flags);
                        return -EIO;
                }
                spin_unlock_irqrestore(&lp->stats_lock, flags);
                break;
        case SIOCSMIIREG:
                if (data->reg_num & ~(0x1F))
                        return -EFAULT;
                mii_reg = data->val_in;
                spin_lock_irqsave(&lp->stats_lock, flags);
                if (minion_write_phy_reg(phy, data->reg_num,
                                        mii_reg)) {
                        spin_unlock_irqrestore(&lp->stats_lock, flags);
                        return -EIO;
                }
                spin_unlock_irqrestore(&lp->stats_lock, flags);
                break;
        default:
                return -EOPNOTSUPP;
        }
        return 0;
	}

static struct net_device_ops minion_netdev_ops = {
	.ndo_open		= minion_open,
	.ndo_stop		= minion_close,
	.ndo_start_xmit		= minion_send,
	.ndo_set_mac_address	= minion_set_mac_address,
	.ndo_tx_timeout		= minion_tx_timeout,
	.ndo_do_ioctl           = minion_mii_ioctl,
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller = minion_poll_controller,
#endif
};

/* Match table for OF platform binding */
static const struct of_device_id tap_minion_of_match[] = {
	{ .compatible = "riscv,minion" },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, tap_minion_of_match);

void tap_minion_free(struct platform_device *of_dev)
{
        struct resource *iomem = platform_get_resource(of_dev, IORESOURCE_MEM, 0);
        release_mem_region(iomem->start, resource_size(iomem));
}

int tap_minion_unregister(struct platform_device *of_dev)
{
        tap_minion_free(of_dev);
        return 0;
}

static struct platform_driver tap_minion_driver = {
	.driver = {
		.name = "lowrisc_digilent_ethernet",
		.of_match_table = tap_minion_of_match,
	},
	.probe = tap_minion_probe,
	.remove = tap_minion_unregister,
};

module_platform_driver(tap_minion_driver);

struct lan8720_priv {
	int	chip_id;
	int	chip_rev;
	__u32	wolopts;
};

MODULE_AUTHOR("Jonathan Kimmitt");
MODULE_DESCRIPTION("Minion Ethernet TAP driver");
MODULE_LICENSE("GPL");
