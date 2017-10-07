/*
 *  LowRISC Secure Digital Host Controller Interface driver
 *
 *  Based on toshsd.c
 *
 *  Copyright (C) 2014 Ondrej Zary
 *  Copyright (C) 2007 Richard Betts, All Rights Reserved.
 *
 *	Based on asic3_mmc.c, copyright (c) 2005 SDG Systems, LLC and,
 *	sdhci.c, copyright (C) 2005-2006 Pierre Ossman
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/pm.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/platform_device.h>
#include <asm/config-string.h>

#include "lowrisc_sd.h"

#define DRIVER_NAME "lowrisc_sd"
void write_led(struct lowrisc_sd_host *host, uint32_t data)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[15] = data;
}

void sd_align(struct lowrisc_sd_host *host, int d_align)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[0] = d_align;
}

void sd_clk_div(struct lowrisc_sd_host *host, int clk_div)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[1] = clk_div;
}

void sd_arg(struct lowrisc_sd_host *host, uint32_t arg)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[2] = arg;
}

void sd_cmd(struct lowrisc_sd_host *host, uint32_t cmd)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[3] = cmd;
}

void sd_setting(struct lowrisc_sd_host *host, int setting)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[4] = setting;
}

void sd_cmd_start(struct lowrisc_sd_host *host, int sd_cmd)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[5] = sd_cmd;
}

void sd_reset(struct lowrisc_sd_host *host, int sd_rst, int clk_rst, int data_rst, int cmd_rst)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[6] = ((sd_rst&1) << 3)|((clk_rst&1) << 2)|((data_rst&1) << 1)|((cmd_rst&1) << 0);
}

void sd_blkcnt(struct lowrisc_sd_host *host, int d_blkcnt)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[7] = d_blkcnt&0xFFFF;
}

void sd_blksize(struct lowrisc_sd_host *host, int d_blksize)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[8] = d_blksize&0xFFF;
}

void sd_timeout(struct lowrisc_sd_host *host, int d_timeout)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[9] = d_timeout;
}

void sd_irq_en(struct lowrisc_sd_host *host, int mask)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[11] = mask;
}

static void lowrisc_sd_init(struct lowrisc_sd_host *host)
{

}

/* Set MMC clock / power */
static void __lowrisc_sd_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct lowrisc_sd_host *host = mmc_priv(mmc);

	switch (ios->power_mode) {
	case MMC_POWER_OFF:
		mdelay(1);
		break;
	case MMC_POWER_UP:
		break;
	case MMC_POWER_ON:
		mdelay(20);
		break;
	}

	switch (ios->bus_width) {
	case MMC_BUS_WIDTH_1:
		break;
	case MMC_BUS_WIDTH_4:
		break;
	}
}

static void lowrisc_sd_set_led(struct lowrisc_sd_host *host, unsigned char state)
{

}

static void lowrisc_sd_finish_request(struct lowrisc_sd_host *host)
{
	struct mmc_request *mrq = host->mrq;

	/* Write something to end the command */
	host->mrq = NULL;
	host->cmd = NULL;
	host->data = NULL;

	lowrisc_sd_set_led(host, 0);
	mmc_request_done(host->mmc, mrq);
}

static irqreturn_t lowrisc_sd_thread_irq(int irq, void *dev_id)
{
	struct lowrisc_sd_host *host = dev_id;
        volatile uint32_t *sd_base = host->ioaddr;
	struct mmc_data *data = host->data;
	struct sg_mapping_iter *sg_miter = &host->sg_miter;
	unsigned short *buf;
	int count;
	unsigned long flags;

	if (!data) {
		dev_warn(&host->pdev->dev, "Spurious Data IRQ\n");
		if (host->cmd) {
			host->cmd->error = -EIO;
			lowrisc_sd_finish_request(host);
		}
		return IRQ_NONE;
	}
	spin_lock_irqsave(&host->lock, flags);

	if (!sg_miter_next(sg_miter))
		goto done;

	buf = sg_miter->addr;

	/* Ensure we dont read more than one block. The chip will interrupt us
	 * When the next block is available.
	 */
	count = sg_miter->length;
	if (count > data->blksz)
		count = data->blksz;

	dev_dbg(&host->pdev->dev, "count: %08x, flags %08x\n", count,
		data->flags);

	/* Transfer the data */
	if (data->flags & MMC_DATA_READ)
          memcpy(buf, (void*)&sd_base[0x2000], count >> 2);
	else
          memcpy((void*)&sd_base[0x2000], buf, count >> 2);

	sg_miter->consumed = count;
	sg_miter_stop(sg_miter);

done:
	spin_unlock_irqrestore(&host->lock, flags);

	return IRQ_HANDLED;
}

static void lowrisc_sd_cmd_irq(struct lowrisc_sd_host *host)
{
	struct mmc_command *cmd = host->cmd;
	u8 *buf;
	u16 data;

	if (!host->cmd) {
		dev_warn(&host->pdev->dev, "Spurious CMD irq\n");
		return;
	}
	buf = (u8 *)cmd->resp;
	host->cmd = NULL;

	if (cmd->flags & MMC_RSP_PRESENT && cmd->flags & MMC_RSP_136) {
		/* R2 */
	} else if (cmd->flags & MMC_RSP_PRESENT) {
		/* R1, R1B, R3, R6, R7 */
	}

	dev_dbg(&host->pdev->dev, "Command IRQ complete %d %d %x\n",
		cmd->opcode, cmd->error, cmd->flags);

	/* If there is data to handle we will
	 * finish the request in the mmc_data_end_irq handler.*/
	if (host->data)
		return;

	lowrisc_sd_finish_request(host);
}

static void lowrisc_sd_data_end_irq(struct lowrisc_sd_host *host)
{
	struct mmc_data *data = host->data;

	host->data = NULL;

	if (!data) {
		dev_warn(&host->pdev->dev, "Spurious data end IRQ\n");
		return;
	}

	if (data->error == 0)
		data->bytes_xfered = data->blocks * data->blksz;
	else
		data->bytes_xfered = 0;

	dev_dbg(&host->pdev->dev, "Completed data request xfr=%d\n",
		data->bytes_xfered);

        //	iowrite16(0, host->ioaddr + SD_STOPINTERNAL);

	lowrisc_sd_finish_request(host);
}

static irqreturn_t lowrisc_sd_irq(int irq, void *dev_id)
{
	struct lowrisc_sd_host *host = dev_id;
        volatile uint32_t *sd_base = host->ioaddr;
	u32 int_reg, int_mask, int_status;
	int error = 0, ret = IRQ_HANDLED;

	spin_lock(&host->lock);
	int_status = sd_base[14];
	int_mask = sd_base[27];
	int_reg = int_status & ~int_mask;

	dev_dbg(&host->pdev->dev, "IRQ status:%x mask:%x\n",
		int_status, int_mask);

	/* nothing to do: it's not our IRQ */
	if (!int_reg) {
		ret = IRQ_NONE;
		goto irq_end;
	}

	if (sd_base[4] >= sd_base[25]) {
		error = -ETIMEDOUT;
		dev_dbg(&host->pdev->dev, "Timeout\n");
	} else if (int_reg & 0) {
		error = -EILSEQ;
		dev_err(&host->pdev->dev, "BadCRC\n");
        }
        
	if (error) {
		if (host->cmd)
			host->cmd->error = error;

		if (error == -ETIMEDOUT) {
                  sd_cmd_start(host, 0);
                  sd_setting(host, 0);
		} else {
			lowrisc_sd_init(host);
			__lowrisc_sd_set_ios(host->mmc, &host->mmc->ios);
			goto irq_end;
		}
	}

	/* Card insert/remove. The mmc controlling code is stateless. */
	if (int_reg & (SD_CARD_CARD_INSERTED_0 | SD_CARD_CARD_REMOVED_0)) {
          sd_irq_en(host, int_status &
                    ~(SD_CARD_CARD_REMOVED_0 | SD_CARD_CARD_INSERTED_0));

		if (int_reg & SD_CARD_CARD_INSERTED_0)
			lowrisc_sd_init(host);

		mmc_detect_change(host->mmc, 1);
	}

	/* Command completion */
	if (int_reg & SD_CARD_RESP_END) {

		lowrisc_sd_cmd_irq(host);
	}

	/* Data transfer completion */
	if (int_reg & SD_CARD_RW_END) {

		lowrisc_sd_data_end_irq(host);
	}
irq_end:
	spin_unlock(&host->lock);
	return ret;
}

static void lowrisc_sd_start_cmd(struct lowrisc_sd_host *host, struct mmc_command *cmd)
{
	struct mmc_data *data = host->data;
	int c = cmd->opcode;

	dev_dbg(&host->pdev->dev, "Command opcode: %d\n", cmd->opcode);

	if (cmd->opcode == MMC_STOP_TRANSMISSION) {
          sd_cmd(host, SD_STOPINT_ISSUE_CMD12);

		cmd->resp[0] = cmd->opcode;
		cmd->resp[1] = 0;
		cmd->resp[2] = 0;
		cmd->resp[3] = 0;

		lowrisc_sd_finish_request(host);
		return;
	}

	switch (mmc_resp_type(cmd)) {
	case MMC_RSP_NONE:
		c |= SD_CMD_RESP_TYPE_NONE;
		break;

	case MMC_RSP_R1:
		c |= SD_CMD_RESP_TYPE_EXT_R1;
		break;
	case MMC_RSP_R1B:
		c |= SD_CMD_RESP_TYPE_EXT_R1B;
		break;
	case MMC_RSP_R2:
		c |= SD_CMD_RESP_TYPE_EXT_R2;
		break;
	case MMC_RSP_R3:
		c |= SD_CMD_RESP_TYPE_EXT_R3;
		break;

	default:
		dev_err(&host->pdev->dev, "Unknown response type %d\n",
			mmc_resp_type(cmd));
		break;
	}

	host->cmd = cmd;

	if (cmd->opcode == MMC_APP_CMD)
		c |= SD_CMD_TYPE_ACMD;

	if (cmd->opcode == MMC_GO_IDLE_STATE)
		c |= (3 << 8);  /* removed from ipaq-asic3.h for some reason */

	if (data) {
		c |= SD_CMD_DATA_PRESENT;

		if (data->blocks > 1) {
                  sd_cmd(host, SD_STOPINT_AUTO_ISSUE_CMD12);
			c |= SD_CMD_MULTI_BLOCK;
		}

		if (data->flags & MMC_DATA_READ)
			c |= SD_CMD_TRANSFER_READ;

		/* MMC_DATA_WRITE does not require a bit to be set */
	}

	/* Send the command */
	sd_arg(host, cmd->arg);
	sd_cmd(host, c);
}

static void lowrisc_sd_start_data(struct lowrisc_sd_host *host, struct mmc_data *data)
{
	unsigned int flags = SG_MITER_ATOMIC;

	dev_dbg(&host->pdev->dev, "setup data transfer: blocksize %08x  nr_blocks %d, offset: %08x\n",
		data->blksz, data->blocks, data->sg->offset);

	host->data = data;

	if (data->flags & MMC_DATA_READ)
		flags |= SG_MITER_TO_SG;
	else
		flags |= SG_MITER_FROM_SG;

	sg_miter_start(&host->sg_miter, data->sg, data->sg_len, flags);

	/* Set transfer length and blocksize */
	sd_blkcnt(host, data->blocks);
	sd_blksize(host, data->blksz);
}

/* Process requests from the MMC layer */
static void lowrisc_sd_request(struct mmc_host *mmc, struct mmc_request *mrq)
{
	struct lowrisc_sd_host *host = mmc_priv(mmc);
        volatile uint32_t *sd_base = host->ioaddr;
	unsigned long flags;

	/* abort if card not present */
	if (sd_base[12] & SD_CARD_PRESENT_0) {
		mrq->cmd->error = -ENOMEDIUM;
		mmc_request_done(mmc, mrq);
		return;
	}

	spin_lock_irqsave(&host->lock, flags);

	WARN_ON(host->mrq != NULL);

	host->mrq = mrq;

	if (mrq->data)
		lowrisc_sd_start_data(host, mrq->data);

	lowrisc_sd_set_led(host, 1);

	lowrisc_sd_start_cmd(host, mrq->cmd);

	spin_unlock_irqrestore(&host->lock, flags);
}

static void lowrisc_sd_set_ios(struct mmc_host *mmc, struct mmc_ios *ios)
{
	struct lowrisc_sd_host *host = mmc_priv(mmc);
	unsigned long flags;

	spin_lock_irqsave(&host->lock, flags);
	__lowrisc_sd_set_ios(mmc, ios);
	spin_unlock_irqrestore(&host->lock, flags);
}

static int lowrisc_sd_get_ro(struct mmc_host *mmc)
{
	struct lowrisc_sd_host *host = mmc_priv(mmc);

	/* active low */
	return 1;
}

static int lowrisc_sd_get_cd(struct mmc_host *mmc)
{
	struct lowrisc_sd_host *host = mmc_priv(mmc);
        volatile uint32_t *sd_base = host->ioaddr;

	return !sd_base[12];
}

static struct mmc_host_ops lowrisc_sd_ops = {
	.request = lowrisc_sd_request,
	.set_ios = lowrisc_sd_set_ios,
	.get_ro = lowrisc_sd_get_ro,
	.get_cd = lowrisc_sd_get_cd,
};


static void lowrisc_sd_powerdown(struct lowrisc_sd_host *host)
{
  /* mask all interrupts */
  sd_irq_en(host, 0);
  /* disable card clock */
}

static int lowrisc_sd_probe(struct platform_device *pdev)
{
	int ret;
	struct lowrisc_sd_host *host;
	struct mmc_host *mmc;
        struct resource *iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
        
	mmc = mmc_alloc_host(sizeof(struct lowrisc_sd_host), &pdev->dev);
	if (!mmc) {
		ret = -ENOMEM;
		goto release;
	}

	host = mmc_priv(mmc);
	host->mmc = mmc;

	host->pdev = pdev;

	if (!request_mem_region(iomem->start, resource_size(iomem),
		mmc_hostname(host->mmc))) {
		dev_err(&pdev->dev, "cannot request region\n");
		ret = -EBUSY;
		goto release;
	}

	host->ioaddr = ioremap(iomem->start, resource_size(iomem));
	if (!host->ioaddr) {
		ret = -ENOMEM;
		goto release;
	}
        host->irq = platform_get_irq(pdev, 0);
        
	/* Set MMC host parameters */
	mmc->ops = &lowrisc_sd_ops;
	mmc->caps = MMC_CAP_4_BIT_DATA;
	mmc->ocr_avail = MMC_VDD_32_33;

	mmc->f_min = 5000000;
	mmc->f_max = 5000000;

	spin_lock_init(&host->lock);

	lowrisc_sd_init(host);

	ret = request_threaded_irq(host->irq, lowrisc_sd_irq, lowrisc_sd_thread_irq,
				   IRQF_SHARED, DRIVER_NAME, host);
	if (ret)
		goto unmap;

	mmc_add_host(mmc);

	dev_dbg(&pdev->dev, "MMIO %p, IRQ %d\n", host->ioaddr, host->irq);

	return 0;

unmap:
release:
	mmc_free_host(mmc);
	return ret;
}

static int lowrisc_sd_remove(struct platform_device *pdev)
{
	struct lowrisc_sd_host *host = platform_get_drvdata(pdev);

	mmc_remove_host(host->mmc);
	lowrisc_sd_powerdown(host);
	free_irq(host->irq, host);
	mmc_free_host(host->mmc);
        return 0;
}

static const struct of_device_id lowrisc_sd_of_match[] = {
	{ .compatible = "riscv,minion" },
	{ }
};

MODULE_DEVICE_TABLE(of, lowrisc_sd_of_match);

static struct platform_driver lowrisc_sd_driver = {
	.driver = {
		.name = "sdhci-minion",
		.of_match_table = lowrisc_sd_of_match,
	},
	.probe = lowrisc_sd_probe,
	.remove = lowrisc_sd_remove,
};

module_platform_driver(lowrisc_sd_driver);

MODULE_AUTHOR("Ondrej Zary, Richard Betts, Jonathan Kimmitt");
MODULE_DESCRIPTION("LowRISC Secure Digital Host Controller Interface driver");
MODULE_LICENSE("GPL");
