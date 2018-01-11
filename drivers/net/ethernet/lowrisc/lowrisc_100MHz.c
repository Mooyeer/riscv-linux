/*
 * Lowrisc Ether100MHz Linux driver for the Lowrisc Ethernet 100MHz device.
 *
 * This is an experimental driver which is based on the original emac_lite
 * driver from John Williams <john.williams@xilinx.com>.
 *
 * 2007 - 2013 (c) Xilinx, Inc.
 * PHY control portions copyright (C) 2015 Microchip Technology
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
#define DRIVER_NAME "lowrisc_ether100MHz"

/* General Ethernet Definitions */
#define XEL_ARP_PACKET_SIZE		28	/* Max ARP packet size */
#define XEL_HEADER_IP_LENGTH_OFFSET	16	/* IP Length Offset */

#define TX_TIMEOUT		(60*HZ)		/* Tx timeout is 60 seconds. */
#define ALIGNMENT		4

/* BUFFER_ALIGN(adr) calculates the number of bytes to the next alignment. */
#define BUFFER_ALIGN(adr) ((ALIGNMENT - ((size_t) adr)) % ALIGNMENT)

/**
 * struct net_local - Our private per device data
 * @ndev:		instance of the network device
 * @reset_lock:		lock used for synchronization
 * @phy_dev:		pointer to the PHY device
 * @phy_node:		pointer to the PHY device node
 * @mii_bus:		pointer to the MII bus
 * @last_link:		last link status
 */
struct net_local {
  struct mdiobb_ctrl ctrl; /* must be first for bitbang driver to work */
  void __iomem *ioaddr;
  struct net_device *ndev;
  u32 msg_enable;
  
  struct phy_device *phy_dev;
  struct mii_bus *mii_bus;
  int last_duplex;
  int last_carrier;
  
  /* Spinlock */
  spinlock_t lock;
  uint16_t mdio_regs_cache[32];
  
};

static void inline eth_write(struct net_local *lp, size_t addr, int data)
{
  volatile unsigned int *eth_base = (volatile unsigned int *)(lp->ioaddr);
  eth_base[addr >> 2] = data;
}

static volatile inline int eth_read(struct net_local *lp, size_t addr)
{
  volatile unsigned int *eth_base = (volatile unsigned int *)(lp->ioaddr);
  return eth_base[addr >> 2];
}

static void inline eth_enable_irq(struct net_local *lp)
{
  volatile unsigned int *eth_base = (volatile unsigned int *)(lp->ioaddr);
  eth_base[MACHI_OFFSET >> 2] |= MACHI_IRQ_EN;
}

static void inline eth_disable_irq(struct net_local *lp)
{
  volatile unsigned int *eth_base = (volatile unsigned int *)(lp->ioaddr);
  eth_base[MACHI_OFFSET >> 2] &= ~MACHI_IRQ_EN;
}

/**
 * lowrisc_update_address - Update the MAC address in the device
 * @drvdata:	Pointer to the Ether100MHz device private data
 * @address_ptr:Pointer to the MAC address (MAC address is a 48-bit value)
 *
 * Tx must be idle and Rx should be idle for deterministic results.
 * It is recommended that this function should be called after the
 * initialization and before transmission of any packets from the device.
 * The MAC address can be programmed using any of the two transmit
 * buffers (if configured).
 */

static void lowrisc_update_address(struct net_local *lp, u8 *address_ptr)
{
  uint32_t macaddr_lo, macaddr_hi;
  uint32_t flags = 0;
  memcpy (&macaddr_lo, address_ptr+2, sizeof(uint32_t));
  memcpy (&macaddr_hi, address_ptr+0, sizeof(uint16_t));
  eth_write(lp, MACLO_OFFSET, htonl(macaddr_lo));
  eth_write(lp, MACHI_OFFSET, flags|htons(macaddr_hi));
}

/**
 * lowrisc_read_mac_address - Read the MAC address in the device
 * @drvdata:	Pointer to the Ether100MHz device private data
 * @address_ptr:Pointer to the 6-byte buffer to receive the MAC address (MAC address is a 48-bit value)
 *
 * In lowrisc the starting value is programmed by the boot loader according to DIP switch [15:12]
 */

static void lowrisc_read_mac_address(struct net_local *lp, u8 *address_ptr)
{
  uint32_t macaddr_hi = ntohs(eth_read(lp, MACHI_OFFSET)&MACHI_MACADDR_MASK);
  uint32_t macaddr_lo = ntohl(eth_read(lp, MACLO_OFFSET));
  memcpy (address_ptr+2, &macaddr_lo, sizeof(uint32_t));
  memcpy (address_ptr+0, &macaddr_hi, sizeof(uint16_t));
}

/**
 * lowrisc_set_mac_address - Set the MAC address for this device
 * @dev:	Pointer to the network device instance
 * @addr:	Void pointer to the sockaddr structure
 *
 * This function copies the HW address from the sockaddr strucutre to the
 * net_device structure and updates the address in HW.
 *
 * Return:	Error if the net device is busy or 0 if the addr is set
 *		successfully
 */
static int lowrisc_set_mac_address(struct net_device *ndev, void *address)
{
	struct net_local *lp = netdev_priv(ndev);
	struct sockaddr *addr = address;
	memcpy(ndev->dev_addr, addr->sa_data, ndev->addr_len);
	lowrisc_update_address(lp, ndev->dev_addr);
	return 0;
}

/**
 * lowrisc_tx_timeout - Callback for Tx Timeout
 * @dev:	Pointer to the network device
 *
 * This function is called when Tx time out occurs for Ether100MHz device.
 */
static void lowrisc_tx_timeout(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);

	dev_err(&lp->ndev->dev, "Exceeded transmit timeout of %lu ms\n",
		TX_TIMEOUT * 1000UL / HZ);

	ndev->stats.tx_errors++;

	/* Reset the device */
	spin_lock(&lp->lock);

	/* Shouldn't really be necessary, but shouldn't hurt */
	netif_stop_queue(ndev);

	/* To exclude tx timeout */
	ndev->trans_start = jiffies; /* prevent tx timeout */

	/* We're all ready to go. Start the queue */
	netif_wake_queue(ndev);
	spin_unlock(&lp->lock);
}

/**
 * lowrisc_close - Close the network device
 * @dev:	Pointer to the network device
 *
 * This function stops the Tx queue, disables interrupts and frees the IRQ for
 * the Ether100MHz device.
 * It also disconnects the phy device associated with the Ether100MHz device.
 */
static int lowrisc_close(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);

	netif_stop_queue(ndev);
	eth_disable_irq(lp);
	free_irq(IRQ_SOFTWARE, ndev);
        printk("Close device, free interrupt\n");
        
	if (lp->phy_dev)
		phy_disconnect(lp->phy_dev);
	lp->phy_dev = NULL;

	return 0;
}

/**
 * lowrisc_remove_ndev - Free the network device
 * @ndev:	Pointer to the network device to be freed
 *
 * This function un maps the IO region of the Ether100MHz device and frees the net
 * device.
 */
static void lowrisc_remove_ndev(struct net_device *ndev)
{
	if (ndev) {
		free_netdev(ndev);
	}
}

#ifndef CONFIG_LOWRISC_BITBANG

static int lowrisc_mii_read(struct mii_bus *bus, int phyaddr, int regidx)
{
	struct net_local *lp = (struct net_local *)bus->priv;
	int reg = -EIO;

	spin_lock(&lp->lock);

	reg = lp->mdio_regs_cache[regidx];

	spin_unlock(&lp->lock);
	return reg;
}

static int lowrisc_mii_write(struct mii_bus *bus, int phyaddr, int regidx,
			   u16 val)
{
	struct net_local *lp = (struct net_local *)bus->priv;

	spin_lock(&lp->lock);

	lp->mdio_regs_cache[regidx] = val;
	
	spin_unlock(&lp->lock);

	return 0;
}
#endif

static void lowrisc_phy_adjust_link(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);
	struct phy_device *phy_dev = lp->phy_dev;
	int carrier;

	if (phy_dev->duplex != lp->last_duplex) {
		if (phy_dev->duplex) {
			netif_dbg(lp, link, lp->ndev, "full duplex mode\n");
		} else {
			netif_dbg(lp, link, lp->ndev, "half duplex mode\n");
		}

		lp->last_duplex = phy_dev->duplex;
	}

	carrier = netif_carrier_ok(ndev);
	if (carrier != lp->last_carrier) {
		if (carrier)
			netif_dbg(lp, link, lp->ndev, "carrier OK\n");
		else
			netif_dbg(lp, link, lp->ndev, "no carrier\n");
		lp->last_carrier = carrier;
	}
}

static int lowrisc_mii_probe(struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);
	struct phy_device *phydev = NULL;
	const char *phyname;
	
	BUG_ON(lp->phy_dev);

	/* Device only supports internal PHY at address 1 */
	phydev = mdiobus_get_phy(lp->mii_bus, 1);
	if (!phydev) {
		netdev_err(ndev, "no PHY found at address 1\n");
		return -ENODEV;
	}

	phyname = phydev_name(phydev);
	printk("Probing %s\n", phyname);
	
	phydev = phy_connect(ndev, phyname,
			     lowrisc_phy_adjust_link, PHY_INTERFACE_MODE_MII);

	if (IS_ERR(phydev)) {
		netdev_err(ndev, "Could not attach to PHY\n");
		return PTR_ERR(phydev);
	}

	/* mask with MAC supported features */
	phydev->supported &= (PHY_BASIC_FEATURES | SUPPORTED_Pause |
			      SUPPORTED_Asym_Pause);
	phydev->advertising = phydev->supported;

	phy_attached_info(phydev);

	lp->phy_dev = phydev;
	lp->last_duplex = -1;
	lp->last_carrier = -1;

	return 0;
}

#ifdef CONFIG_LOWRISC_BITBANG

static uint32_t last_gpio;

static void mdio_dir(struct mdiobb_ctrl *ctrl, int dir)
{
  struct net_local *lp = (struct net_local *)ctrl; /* struct mdiobb_ctrl must be first in net_local for bitbang driver to work */
  if (dir)
    last_gpio |= MDIOCTRL_MDIOOEN_MASK;
  else
    last_gpio &= ~MDIOCTRL_MDIOOEN_MASK;
    
  eth_write(lp, MDIOCTRL_OFFSET, last_gpio);
}

static int mdio_get(struct mdiobb_ctrl *ctrl)
{
  struct net_local *lp = (struct net_local *)ctrl; /* struct mdiobb_ctrl must be first in net_local for bitbang driver to work */
  uint32_t rslt = eth_read(lp, MDIOCTRL_OFFSET) & MDIOCTRL_MDIOIN_MASK ? 1:0;
  return rslt;
}

static void mdio_set(struct mdiobb_ctrl *ctrl, int what)
{
  struct net_local *lp = (struct net_local *)ctrl; /* struct mdiobb_ctrl must be first in net_local for bitbang driver to work */
  if (what)
    last_gpio |= MDIOCTRL_MDIOOUT_MASK;
  else
    last_gpio &= ~MDIOCTRL_MDIOOUT_MASK;
    
  eth_write(lp, MDIOCTRL_OFFSET, last_gpio);
}

static void mdc_set(struct mdiobb_ctrl *ctrl, int what)
{
  struct net_local *lp = (struct net_local *)ctrl; /* struct mdiobb_ctrl must be first in net_local for bitbang driver to work */
  if (what)
    last_gpio |= MDIOCTRL_MDIOCLK_MASK;
  else
    last_gpio &= ~MDIOCTRL_MDIOCLK_MASK;
    
  eth_write(lp, MDIOCTRL_OFFSET, last_gpio);
}

/* reset callback */
static int lowrisc_reset(struct mii_bus *bus)
{
  struct net_local *lp = (struct net_local *)bus->priv;
  eth_write(lp, MDIOCTRL_OFFSET, MDIOCTRL_MDIORST_MASK);
  mdelay(1000);
  eth_write(lp, MDIOCTRL_OFFSET, 0);
  mdelay(1000);
  return 0;
}

static struct mdiobb_ops mdio_gpio_ops = {
        .owner = THIS_MODULE,
        .set_mdc = mdc_set,
        .set_mdio_dir = mdio_dir,
        .set_mdio_data = mdio_set,
        .get_mdio_data = mdio_get,
};

#else

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
#endif

static int lowrisc_mii_init(struct net_device *ndev)
{
        struct mii_bus *new_bus;
	struct net_local *lp = netdev_priv(ndev);
	int err = -ENXIO;
	
#ifdef CONFIG_LOWRISC_BITBANG
	lp->ctrl.ops = &mdio_gpio_ops;
	lp->ctrl.reset = lowrisc_reset;
        new_bus = alloc_mdio_bitbang(&(lp->ctrl));
#else
	new_bus = mdiobus_alloc();
	new_bus->read = lowrisc_mii_read;
	new_bus->write = lowrisc_mii_write;
	memcpy(lp->mdio_regs_cache, mdio_regs_init, sizeof(lp->mdio_regs_cache));
#endif	
	if (!new_bus) {
		err = -ENOMEM;
		goto err_out_1;
	}
	snprintf(new_bus->id, MII_BUS_ID_SIZE, "lowrisc-0");
        new_bus->name = "GPIO Bitbanged LowRISC",

        new_bus->phy_mask = ~(1 << 1);
        new_bus->phy_ignore_ta_mask = 0;

	mutex_init(&(new_bus->mdio_lock));
	
	lp->mii_bus = new_bus;
	lp->mii_bus->priv = lp;

	/* Mask all PHYs except ID 1 (internal) */
	lp->mii_bus->phy_mask = ~(1 << 1);

	if (mdiobus_register(lp->mii_bus)) {
		netif_warn(lp, probe, lp->ndev, "Error registering mii bus\n");
		goto err_out_free_bus_2;
	}

	if (lowrisc_mii_probe(ndev) < 0) {
		netif_warn(lp, probe, lp->ndev, "Error probing mii bus\n");
		goto err_out_unregister_bus_3;
	}

	return 0;

err_out_unregister_bus_3:
	mdiobus_unregister(lp->mii_bus);
err_out_free_bus_2:
	mdiobus_free(lp->mii_bus);
err_out_1:
	return err;
}
/**********************/
/* Interrupt Handlers */
/**********************/

/**
 * lowrisc_ether_isr - Interrupt handler for frames received
 * @dev:	Pointer to the network device
 *
 * This function allocates memory for a socket buffer, fills it with data
 * received and hands it over to the TCP/IP stack.
 */

irqreturn_t lowrisc_ether_isr(int irq, void *dev_id)
{
  irqreturn_t rc = IRQ_NONE;
  struct net_device *ndev = dev_id;
  struct net_local *lp = netdev_priv(ndev);
  spin_lock(&(lp->lock));
  /* Check if there is Rx Data available */
  if (eth_read(lp, RSR_OFFSET) & RSR_RECV_DONE_MASK)
    {
      int fcs = eth_read(lp, RFCS_OFFSET);
      int rplr = eth_read(lp, RPLR_OFFSET);
      int len = (rplr & RPLR_LENGTH_MASK) - 4; /* discard FCS bytes */
      rc = IRQ_HANDLED;
      if ((len >= 14) && (fcs == 0xc704dd7b) && (len <= ETH_FRAME_LEN + ETH_FCS_LEN + ALIGNMENT))
	{
	  uint32_t *alloc;
	  int rnd = (((len-1)|3)+1); /* round to a multiple of 4 */
	  struct sk_buff *skb = netdev_alloc_skb(ndev, rnd + ALIGNMENT);
	  if (!skb)
	    {
	      /* Couldn't get memory. */
	      ndev->stats.rx_dropped++;
	      dev_err(&lp->ndev->dev, "Could not allocate receive buffer\n");
	    }
	  else
	    {
	      /*
	       * A new skb should have the data halfword aligned, but this code is
	       * here just in case that isn't true. Calculate how many
	       * bytes we should reserve to get the data to start on a word
	       * boundary */
	      unsigned int i, align = BUFFER_ALIGN(skb->data);
	      if (align)
		skb_reserve(skb, align);
	      skb_reserve(skb, 2);
	      
              alloc = (uint32_t *)(skb->data);
              for (i = 0; i < rnd/4; i++)
                {
                  alloc[i] = eth_read(lp, RXBUFF_OFFSET+(i<<2));
                }
              skb_put(skb, len);	/* Tell the skb how much data we got */
              
              skb->protocol = eth_type_trans(skb, ndev);
              skb_checksum_none_assert(skb);
              
              ndev->stats.rx_packets++;
              ndev->stats.rx_bytes += len;
              
              if (!skb_defer_rx_timestamp(skb))
                netif_rx(skb);	/* Send the packet upstream */
            }
        }
      else
	  ndev->stats.rx_errors++;
      /* acknowledge, even if an error occurs, to reset irq */
      eth_write(lp, RSR_OFFSET, 0);
    }
  spin_unlock(&(lp->lock));
  eth_enable_irq(lp);
  
  return rc;
}

/**
 * lowrisc_open - Open the network device
 * @dev:	Pointer to the network device
 *
 * This function sets the MAC address, requests an IRQ and enables interrupts
 * for the Ether100MHz device and starts the Tx queue.
 * It also connects to the phy device, if MDIO is included in Ether100MHz device.
 */

static int lowrisc_get_regs_len(struct net_device __always_unused *netdev)
{
#define LOWRISC_REGS_LEN 40	/* overestimate */
  return LOWRISC_REGS_LEN * sizeof(u32);
}

s32 lowrisc_read_phy_reg(struct phy_device *phydev, u32 reg_addr, u16 * phy_data)
{
  u16 val = phy_read(phydev, reg_addr);
  *phy_data = val;
  return 0;
}

s32 lowrisc_write_phy_reg(struct phy_device *phydev, u32 reg_addr, u16 data)
{
  return phy_write(phydev, reg_addr, data);
}

static void lowrisc_get_regs(struct net_device *ndev,
			   struct ethtool_regs *regs, void *p)
{
  struct net_local *lp = netdev_priv(ndev);
  struct phy_device *phy = lp->phy_dev;

  u32 *regs_buff = p;
  u16 phy_data;
  int i;

  memset(p, 0, LOWRISC_REGS_LEN * sizeof(u32));

  regs->version = 0;

  for (i = 0; i < LOWRISC_REGS_LEN; i++)
    {
      if (i >= 32)
	regs_buff[i] = eth_read(lp, MACLO_OFFSET+((i-32)<<2));
      else
	{
	lowrisc_read_phy_reg(phy, i, &phy_data);
	regs_buff[i] = phy_data;
	}
    }
}

static const struct ethtool_ops lowrisc_ethtool_ops = {
	.get_regs_len		= lowrisc_get_regs_len,
	.get_regs		= lowrisc_get_regs
};

static int lowrisc_open(struct net_device *ndev)
{
  int retval;
  struct net_local *lp = netdev_priv(ndev);
  ndev->ethtool_ops = &lowrisc_ethtool_ops;

  /* Set the MAC address each time opened */
  lowrisc_update_address(lp, ndev->dev_addr);
  
  if (lp->phy_dev) {
    /* Ether100MHz doesn't support giga-bit speeds */
    lp->phy_dev->supported &= (PHY_BASIC_FEATURES);
    lp->phy_dev->advertising = lp->phy_dev->supported;
    
    phy_start(lp->phy_dev);
  }
  
  /* Grab the IRQ */
  printk("Open device, request interrupt\n");
  retval = request_irq(IRQ_SOFTWARE, lowrisc_ether_isr, IRQF_SHARED, ndev->name, ndev);
  if (retval) {
    dev_err(&lp->ndev->dev, "Could not allocate interrupt %d\n", IRQ_SOFTWARE);
    if (lp->phy_dev)
      phy_disconnect(lp->phy_dev);
    lp->phy_dev = NULL;
    
    return retval;
  }
  
  lowrisc_update_address(lp, ndev->dev_addr);

  /* We're ready to go */
  netif_start_queue(ndev);

  /* first call to handler enables the irq */
  lowrisc_ether_isr(IRQ_SOFTWARE, ndev);
  return 0;
}

/**
 * lowrisc_send - Transmit a frame
 * @orig_skb:	Pointer to the socket buffer to be transmitted
 * @dev:	Pointer to the network device
 *
 * This function checks if the Tx buffer of the Ether100MHz device is free to send
 * data. If so, it fills the Tx buffer with data from socket buffer data,
 * updates the stats and frees the socket buffer.
 * Return:	0, always.
 */
static int lowrisc_send(struct sk_buff *new_skb, struct net_device *ndev)
{
	struct net_local *lp = netdev_priv(ndev);
	unsigned int len = new_skb->len;
        uint32_t *alloc = (uint32_t *)new_skb->data;
        int i, rslt;

	spin_lock(&lp->lock);
        rslt = eth_read(lp, TPLR_OFFSET);
        if (rslt & TPLR_BUSY_MASK)
          printk("TX Busy Status = %x, len = %d, ignoring\n", rslt, len);
        for (i = 0; i < (((len-1)|3)+1)/4; i++)
          {
            eth_write(lp, TXBUFF_OFFSET+(i<<2), alloc[i]);
          }
        eth_write(lp, TPLR_OFFSET, len);

	spin_unlock(&lp->lock);

	skb_tx_timestamp(new_skb);

	ndev->stats.tx_bytes += len;
	ndev->stats.tx_packets++;
	dev_consume_skb_any(new_skb);

	return 0;
}

static int lowrisc_mii_ioctl(struct net_device *netdev, struct ifreq *ifr, int cmd)
{
        struct net_local *lp = netdev_priv(netdev);
	struct phy_device *phy = lp->phy_dev;
        struct mii_ioctl_data *data = if_mii(ifr);
        u16 mii_reg;

        switch (cmd) {
        case SIOCGMIIPHY:
                data->phy_id = 1;
                break;
        case SIOCGMIIREG:
                spin_lock(&lp->lock);
                if (lowrisc_read_phy_reg(phy, data->reg_num & 0x1F,
                                   &data->val_out)) {
                        spin_unlock(&lp->lock);
                        return -EIO;
                }
                spin_unlock(&lp->lock);
                break;
        case SIOCSMIIREG:
                if (data->reg_num & ~(0x1F))
                        return -EFAULT;
                mii_reg = data->val_in;
                spin_lock(&lp->lock);
                if (lowrisc_write_phy_reg(phy, data->reg_num,
                                        mii_reg)) {
                        spin_unlock(&lp->lock);
                        return -EIO;
                }
                spin_unlock(&lp->lock);
                break;
        default:
                return -EOPNOTSUPP;
        }
        return 0;
	}

static struct net_device_ops lowrisc_netdev_ops = {
	.ndo_open		= lowrisc_open,
	.ndo_stop		= lowrisc_close,
	.ndo_start_xmit		= lowrisc_send,
	.ndo_set_mac_address	= lowrisc_set_mac_address,
	.ndo_tx_timeout		= lowrisc_tx_timeout,
	.ndo_do_ioctl           = lowrisc_mii_ioctl,
};

/**
 * lowrisc_of_probe - Probe method for the Ether100MHz device.
 * @ofdev:	Pointer to OF device structure
 * @match:	Pointer to the structure used for matching a device
 *
 * This function probes for the Ether100MHz device in the device tree.
 * It initializes the driver data structure and the hardware, sets the MAC
 * address and registers the network device.
 * It also registers a mii_bus for the Ether100MHz device, if MDIO is included
 * in the device.
 *
 * Return:	0, if the driver is bound to the Ether100MHz device, or
 *		a negative error if there is failure.
 */
static int lowrisc_100MHz_probe(struct platform_device *ofdev)
{
	struct net_device *ndev = NULL;
	struct net_local *lp = NULL;
	struct device *dev = &ofdev->dev;
        struct resource *lowrisc_ethernet = ofdev->resource;
	unsigned char mac_address[7];
	int rc = 0;

        lowrisc_ethernet = platform_get_resource(ofdev, IORESOURCE_MEM, 0);

	/* Create an ethernet device instance */
	ndev = alloc_etherdev(sizeof(struct net_local));
	if (!ndev)
		return -ENOMEM;

	dev_set_drvdata(dev, ndev);
	SET_NETDEV_DEV(ndev, &ofdev->dev);

	lp = netdev_priv(ndev);
	lp->ndev = ndev;
        lp->ioaddr = devm_ioremap_resource(&ofdev->dev, lowrisc_ethernet);

	printk("lowrisc-digilent-ethernet: Lowrisc ethernet platform (%llX-%llX) mapped to %p\n",
               lowrisc_ethernet[0].start, lowrisc_ethernet[0].end, lp->ioaddr);
        
	spin_lock_init(&lp->lock);

        /* get the MAC address set by the boot loader */
        lowrisc_read_mac_address(lp, mac_address);
	memcpy(ndev->dev_addr, mac_address, ETH_ALEN);

	/* Set the MAC address in the Ether100MHz device */
	lowrisc_update_address(lp, ndev->dev_addr);

	lowrisc_mii_init(ndev);

	ndev->netdev_ops = &lowrisc_netdev_ops;
	ndev->flags &= ~IFF_MULTICAST;
	ndev->watchdog_timeo = TX_TIMEOUT;

	/* Finally, register the device */
	rc = register_netdev(ndev);
	if (rc) {
		dev_err(dev,
			"Cannot register network device, aborting\n");
		goto error;
	}

	dev_info(dev, "Lowrisc Ether100MHz registered\n");
	
	return 0;

error:
	lowrisc_remove_ndev(ndev);
	return rc;
}

/* Match table for OF platform binding */
static const struct of_device_id lowrisc_100MHz_of_match[] = {
	{ .compatible = "riscv,lowrisc" },
	{ /* end of list */ },
};
MODULE_DEVICE_TABLE(of, lowrisc_100MHz_of_match);

void lowrisc_100MHz_free(struct platform_device *of_dev)
{
        struct resource *iomem = platform_get_resource(of_dev, IORESOURCE_MEM, 0);
        release_mem_region(iomem->start, resource_size(iomem));
}

int lowrisc_100MHz_unregister(struct platform_device *of_dev)
{
        lowrisc_100MHz_free(of_dev);
        return 0;
}

static struct platform_driver lowrisc_100MHz_driver = {
	.driver = {
		.name = "lowrisc_digilent_ethernet",
		.of_match_table = lowrisc_100MHz_of_match,
	},
	.probe = lowrisc_100MHz_probe,
	.remove = lowrisc_100MHz_unregister,
};

module_platform_driver(lowrisc_100MHz_driver);

struct lan8720_priv {
	int	chip_id;
	int	chip_rev;
	__u32	wolopts;
};

MODULE_AUTHOR("Jonathan Kimmitt");
MODULE_DESCRIPTION("Lowrisc Ethernet 100MHz driver");
MODULE_LICENSE("GPL");
