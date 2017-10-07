/*
 *  LowRISC PCI Secure Digital Host Controller Interface driver
 *
 *  Based on toshsd.h
 *
 *  Copyright (C) 2014 Ondrej Zary
 *  Copyright (C) 2007 Richard Betts, All Rights Reserved.
 *
 *      Based on asic3_mmc.c Copyright (c) 2005 SDG Systems, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#define SD_CMD			0x00	/* also for SDIO */
#define SD_ARG0			0x04	/* also for SDIO */
#define SD_ARG1			0x06	/* also for SDIO */
#define SD_STOPINTERNAL		0x08
#define SD_BLOCKCOUNT		0x0a	/* also for SDIO */
#define SD_RESPONSE0		0x0c	/* also for SDIO */
#define SD_RESPONSE1		0x0e	/* also for SDIO */
#define SD_RESPONSE2		0x10	/* also for SDIO */
#define SD_RESPONSE3		0x12	/* also for SDIO */
#define SD_RESPONSE4		0x14	/* also for SDIO */
#define SD_RESPONSE5		0x16	/* also for SDIO */
#define SD_RESPONSE6		0x18	/* also for SDIO */
#define SD_RESPONSE7		0x1a	/* also for SDIO */
#define SD_CARDSTATUS		0x1c	/* also for SDIO */
#define SD_BUFFERCTRL		0x1e	/* also for SDIO */
#define SD_INTMASKCARD		0x20	/* also for SDIO */
#define SD_INTMASKBUFFER	0x22	/* also for SDIO */
#define SD_CARDCLOCKCTRL	0x24
#define SD_CARDXFERDATALEN	0x26	/* also for SDIO */
#define SD_CARDOPTIONSETUP	0x28	/* also for SDIO */
#define SD_ERRORSTATUS0		0x2c	/* also for SDIO */
#define SD_ERRORSTATUS1		0x2e	/* also for SDIO */
#define SD_DATAPORT		0x30	/* also for SDIO */
#define SD_TRANSACTIONCTRL	0x34	/* also for SDIO */
#define SD_SOFTWARERESET	0xe0	/* also for SDIO */

#define SD_CMD_TYPE_CMD			(0 << 6)
#define SD_CMD_TYPE_ACMD		(1 << 6)
#define SD_CMD_TYPE_AUTHEN		(2 << 6)
#define SD_CMD_RESP_TYPE_NONE		(3 << 8)
#define SD_CMD_RESP_TYPE_EXT_R1		(4 << 8)
#define SD_CMD_RESP_TYPE_EXT_R1B	(5 << 8)
#define SD_CMD_RESP_TYPE_EXT_R2		(6 << 8)
#define SD_CMD_RESP_TYPE_EXT_R3		(7 << 8)
#define SD_CMD_RESP_TYPE_EXT_R6		(4 << 8)
#define SD_CMD_RESP_TYPE_EXT_R7		(4 << 8)
#define SD_CMD_DATA_PRESENT		BIT(11)
#define SD_CMD_TRANSFER_READ		BIT(12)
#define SD_CMD_MULTI_BLOCK		BIT(13)
#define SD_CMD_SECURITY_CMD		BIT(14)
#define SD_STOPINT_ISSUE_CMD12		BIT(0)
#define SD_STOPINT_AUTO_ISSUE_CMD12	BIT(8)
#define SD_CARD_PRESENT_0	BIT(5)
#define SD_CARD_WRITE_PROTECT	BIT(7)

enum {SD_CARD_RESP_END=1,SD_CARD_RW_END=2, SD_CARD_CARD_REMOVED_0=4, SD_CARD_CARD_INSERTED_0=8};

struct lowrisc_sd_host {
  	struct platform_device *pdev;
	struct mmc_host *mmc;

	spinlock_t lock;

	struct mmc_request *mrq;/* Current request */
	struct mmc_command *cmd;/* Current command */
	struct mmc_data *data;	/* Current data request */

	struct sg_mapping_iter sg_miter; /* for PIO */

	void __iomem *ioaddr; /* mapped address */
        int irq;
};
