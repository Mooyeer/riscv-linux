// See LICENSE for license details.

#ifndef ETH_HEADER_H
#define ETH_HEADER_H

/* Register offsets for the EmacLite Core */

#define TXBUFF_OFFSET       0x1000          /* Transmit Buffer */

#define MACLO_OFFSET        0x0800          /* MAC address low 32-bits */
#define MACHI_OFFSET        0x0804          /* MAC address high 16-bits and MAC ctrl */
#define TPLR_OFFSET         0x0808          /* Tx packet length */
#define FRMERR_OFFSET       0x080C          /* Tx frame error register */
#define MDIOCTRL_OFFSET     0x0810          /* MDIO Control Register */
#define FCSERR_OFFSET       0x0814          /* Rx reset and FCS error register */
#define BUF_OFFSET          0x0818          /* Rx index register */
#define RPLR_OFFSET         0x0820          /* Rx packet length registers */

#define RXBUFF_OFFSET       0x4000          /* Receive Buffer */
#define MDIORD_RDDATA_MASK    0x0000FFFF    /* Data to be Read */

/* MAC Ctrl Register (MACHI) Bit Masks */
#define MACHI_MACADDR_MASK    0x0000FFFF     /* MAC high 16-bits mask */
#define MACHI_COOKED_MASK     0x00010000     /* obsolete flag */
#define MACHI_LOOPBACK_MASK   0x00020000     /* Rx loopback packets */
#define MACHI_IRQ_EN          0x00400000     /* Rx packet interrupt enable */

/* MDIO Control Register Bit Masks */
#define MDIOCTRL_MDIOCLK_MASK 0x00000001    /* MDIO Clock Mask */
#define MDIOCTRL_MDIOOUT_MASK 0x00000002    /* MDIO Output Mask */
#define MDIOCTRL_MDIOOEN_MASK 0x00000004    /* MDIO Output Enable Mask */
#define MDIOCTRL_MDIORST_MASK 0x00000008    /* MDIO Input Mask */
#define MDIOCTRL_MDIOIN_MASK  0x00000008    /* MDIO Input Mask */

/* Transmit Status Register (TPLR) Bit Masks */
#define TPLR_FRAME_ADDR_MASK  0x0FFF0000     /* Tx frame address */
#define TPLR_PACKET_LEN_MASK  0x00000FFF     /* Tx packet length */
#define TPLR_BUSY_MASK        0x80000000     /* Tx busy mask */

/* Receive Index Register (BUF) */
#define BUF_FIRST_MASK        0x0000000F      /* Rx buffer first mask */
#define BUF_NEXT_MASK         0x000000F0      /* Rx buffer next mask */
#define BUF_LAST_MASK         0x00000F00      /* Rx buffer last mask */
#define BUF_IRQ_MASK          0x00001000      /* Rx irq bit */
#define BUF_NEXT_SHIFT        0x00000004      /* Rx buffer next shift */

/* General Ethernet Definitions */
#define HEADER_OFFSET               12      /* Offset to length field */
#define HEADER_SHIFT                16      /* Shift value for length */
#define ARP_PACKET_SIZE             28      /* Max ARP packet size */
#define HEADER_IP_LENGTH_OFFSET     16      /* IP Length Offset */

#endif
