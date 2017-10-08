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
#define VERBOSE 0
#define LOG dev_dbg(&host->pdev->dev,
#define LOGV if (VERBOSE) dev_dbg(&host->pdev->dev,
//#define LOG printk(
//#define LOG myprintf(

int myprintf(const char *fmt, ...)
{
  extern void xuart_putchar(int);
  char buffer[99], *ptr = buffer;
  va_list va;
  int rslt;
  va_start(va, fmt);
  rslt = vsnprintf(buffer, sizeof(buffer), fmt, va);
  va_end(va);
  while (*ptr)
    xuart_putchar(*ptr++);
  return rslt;
}

void sd_align(struct lowrisc_sd_host *host, int d_align)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[align_reg] = d_align;
}

void sd_clk_div(struct lowrisc_sd_host *host, int clk_div)
{
  volatile uint32_t *sd_base = host->ioaddr;
  /* This section is incomplete */
  sd_base[clk_din_reg] = clk_div;
}

void sd_arg(struct lowrisc_sd_host *host, uint32_t arg)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[arg_reg] = arg;
}

void sd_cmd(struct lowrisc_sd_host *host, uint32_t cmd)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[cmd_reg] = cmd;
}

void sd_setting(struct lowrisc_sd_host *host, int setting)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[setting_reg] = setting;
}

void sd_cmd_start(struct lowrisc_sd_host *host, int sd_cmd)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[start_reg] = sd_cmd;
}

void sd_reset(struct lowrisc_sd_host *host, int sd_rst, int clk_rst, int data_rst, int cmd_rst)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[reset_reg] = ((sd_rst&1) << 3)|((clk_rst&1) << 2)|((data_rst&1) << 1)|((cmd_rst&1) << 0);
}

void sd_blkcnt(struct lowrisc_sd_host *host, int d_blkcnt)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[blkcnt_reg] = d_blkcnt&0xFFFF;
}

void sd_blksize(struct lowrisc_sd_host *host, int d_blksize)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[blksiz_reg] = d_blksize&0xFFF;
}

void sd_timeout(struct lowrisc_sd_host *host, int d_timeout)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[timeout_reg] = d_timeout;
}

void sd_irq_en(struct lowrisc_sd_host *host, int mask)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[irq_en_reg] = mask;
  host->int_en = mask;
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
	  host->width_setting = 0;
	  break;
	case MMC_BUS_WIDTH_4:
	  host->width_setting = 0x20;
	  break;
	}
}

static void lowrisc_sd_set_led(struct lowrisc_sd_host *host, unsigned char state)
{
  volatile uint32_t *sd_base = host->ioaddr;
  sd_base[led_reg] = state;
}

static void lowrisc_sd_finish_request(struct lowrisc_sd_host *host)
{
	struct mmc_request *mrq = host->mrq;

	/* Write something to end the command */
	host->mrq = NULL;
	host->cmd = NULL;
	host->data = NULL;

	sd_reset(host, 0,1,0,1);
	sd_cmd_start(host, 0);
	sd_reset(host, 0,1,1,1);
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

	LOGV "lowrisc_sd_thread_irq\n");

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

	LOG "count: %08x, flags %08x\n", count,
		data->flags);
	/* Transfer the data */
	if (data->flags & MMC_DATA_READ)
          memcpy(buf, (void*)&sd_base[data_buffer_offset], count >> 2);
	else
          memcpy((void*)&sd_base[data_buffer_offset], buf, count >> 2);

	sg_miter->consumed = count;
	sg_miter_stop(sg_miter);
done:
	spin_unlock_irqrestore(&host->lock, flags);

	return IRQ_HANDLED;
}

static void lowrisc_sd_cmd_irq(struct lowrisc_sd_host *host)
{
	struct mmc_command *cmd = host->cmd;
        volatile uint32_t *sd_base = host->ioaddr;
	u32 data;

	LOGV "lowrisc_sd_cmd_irq\n");
	
	if (!host->cmd) {
		dev_warn(&host->pdev->dev, "Spurious CMD irq\n");
		return;
	}
	host->cmd = NULL;

        LOGV "lowrisc_sd_cmd_irq IRQ line %d\n", __LINE__);
	if (cmd->flags & MMC_RSP_PRESENT && cmd->flags & MMC_RSP_136) {
	  int i;
	  LOGV "lowrisc_sd_cmd_irq IRQ line %d\n", __LINE__);
		/* R2 */
	  for (i = 0;i < 4;i++)
	    {
	    cmd->resp[i] = sd_base[resp0 + (3-i)] << 8;
	    if (i != 3)
	      cmd->resp[i] |= sd_base[resp0 + (2-i)] >> 24;
	    } 
	} else if (cmd->flags & MMC_RSP_PRESENT) {
	  LOGV "lowrisc_sd_cmd_irq IRQ line %d\n", __LINE__);
		/* R1, R1B, R3, R6, R7 */
	  cmd->resp[0] = sd_base[resp0];
	}

	LOG "Command IRQ complete %d %d %x\n", cmd->opcode, cmd->error, cmd->flags);

	/* If there is data to handle we will
	 * finish the request in the mmc_data_end_irq handler.*/
	if (host->data)
	  {
	    host->int_en |= SD_CARD_RW_END;
	  }
	else
	  lowrisc_sd_finish_request(host);
}

static void lowrisc_sd_data_end_irq(struct lowrisc_sd_host *host)
{
	struct mmc_data *data = host->data;
        volatile uint32_t *sd_base = host->ioaddr;
	unsigned long flags;
	size_t blksize, len, chunk;
	u32 uninitialized_var(scratch);
	u8 *buf;
	int i = 0;
	
	LOG "lowrisc_sd_data_end_irq\n");

	host->data = NULL;

	if (!data) {
		dev_warn(&host->pdev->dev, "Spurious data end IRQ\n");
		return;
	}
        
	blksize = data->blksz;
	chunk = 0;

	local_irq_save(flags);

	while (blksize) {
	  int idx = 0;
	  BUG_ON(!sg_miter_next(&host->sg_miter));
	  
	  len = min(host->sg_miter.length, blksize);
	  
	  blksize -= len;
	  host->sg_miter.consumed = len;
	  
	  buf = host->sg_miter.addr;
	  
	  while (len) {
	    if (chunk == 0) {
	      scratch = __be32_to_cpu(sd_base[0x2000 + i++]);
	      chunk = 4;
	    }
	    
	    buf[idx] = scratch & 0xFF;	    
	    idx++;
	    scratch >>= 8;
	    chunk--;
	    len--;
	  }
	}
	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);

	if (data->error == 0)
		data->bytes_xfered = data->blocks * data->blksz;
	else
		data->bytes_xfered = 0;

	LOG "Completed data request xfr=%d\n",
		data->bytes_xfered);

        //	iowrite16(0, host->ioaddr + SD_STOPINTERNAL);

	lowrisc_sd_finish_request(host);
}

static irqreturn_t lowrisc_sd_irq(int irq, void *dev_id)
{
	struct lowrisc_sd_host *host = dev_id;
        volatile uint32_t *sd_base = host->ioaddr;
	u32 int_reg, int_status;
	int error = 0, ret = IRQ_HANDLED;

	spin_lock(&host->lock);
	int_status = sd_base[irq_stat_resp];
	int_reg = int_status & host->int_en;

	/* nothing to do: it's not our IRQ */
	if (!int_reg) {
		ret = IRQ_NONE;
		goto irq_end;
	}

	LOGV "lowrisc_sd IRQ status:%x enabled:%x\n", int_status, host->int_en);

	if (sd_base[wait_resp] >= sd_base[timeout_resp]) {
		error = -ETIMEDOUT;
		LOG "Timeout %d clocks\n", sd_base[timeout_resp]);
	} else if (int_reg & 0) {
		error = -EILSEQ;
		dev_err(&host->pdev->dev, "BadCRC\n");
        }
        
        LOGV "lowrisc_sd IRQ line %d\n", __LINE__);

	if (error) {
	  LOGV "lowrisc_sd IRQ line %d\n", __LINE__);
		if (host->cmd)
			host->cmd->error = error;

		if (error == -ETIMEDOUT) {
		  LOGV "lowrisc_sd IRQ line %d\n", __LINE__);
                  sd_cmd_start(host, 0);
                  sd_setting(host, 0);
		} else {
		    LOGV "lowrisc_sd IRQ line %d\n", __LINE__);
			lowrisc_sd_init(host);
			__lowrisc_sd_set_ios(host->mmc, &host->mmc->ios);
			goto irq_end;
		}
	}

        LOGV "lowrisc_sd IRQ line %d\n", __LINE__);

        /* Card insert/remove. The mmc controlling code is stateless. */
	if (int_reg & SD_CARD_CARD_REMOVED_0)
	  {
	    int mask = (host->int_en & ~SD_CARD_CARD_REMOVED_0) | SD_CARD_CARD_INSERTED_0;
	    sd_irq_en(host, mask);
	    LOG "Card removed, mask changed to %d\n", mask);
	    mmc_detect_change(host->mmc, 1);
	  }
	
        LOGV "lowrisc_sd IRQ line %d\n", __LINE__);
	if (int_reg & SD_CARD_CARD_INSERTED_0)
	  {
	    int mask = (host->int_en & ~SD_CARD_CARD_INSERTED_0) | SD_CARD_CARD_REMOVED_0 ;
	    sd_irq_en(host, mask);
	    LOG "Card inserted, mask changed to %d\n", mask);
	    lowrisc_sd_init(host);
	    mmc_detect_change(host->mmc, 1);
	  }

        LOGV "lowrisc_sd IRQ line %d\n", __LINE__);
	/* Command completion */
	if (int_reg & SD_CARD_RESP_END) {
	  LOGV "lowrisc_sd IRQ line %d\n", __LINE__);

		lowrisc_sd_cmd_irq(host);
		host->int_en &= ~SD_CARD_RESP_END;
	}

        LOGV "lowrisc_sd IRQ line %d\n", __LINE__);
	/* Data transfer completion */
	if (int_reg & SD_CARD_RW_END) {
	  LOGV "lowrisc_sd IRQ line %d\n", __LINE__);

		lowrisc_sd_data_end_irq(host);
		host->int_en &= ~SD_CARD_RW_END;
	}
irq_end:
        sd_irq_en(host, host->int_en);
	spin_unlock(&host->lock);
	return ret;
}

static void minion_sdhci_write_block_pio(struct lowrisc_sd_host *host)
{
  volatile uint32_t *sd_base = host->ioaddr;
  unsigned long flags;
  size_t blksize, len, chunk;
  u32 scratch, i = 0;
  u8 *buf;

	LOGV "PIO writing\n");
        
	blksize = host->data->blksz;
	chunk = 0;
	scratch = 0;

	local_irq_save(flags);

	while (blksize) {
		BUG_ON(!sg_miter_next(&host->sg_miter));

		len = min(host->sg_miter.length, blksize);

		blksize -= len;
		host->sg_miter.consumed = len;

		buf = host->sg_miter.addr;

		while (len) {
			scratch |= (u32)*buf << (chunk * 8);

			buf++;
			chunk++;
			len--;

			if ((chunk == 4) || ((len == 0) && (blksize == 0))) {
			  sd_base[0x2000 + i++] = __cpu_to_be32(scratch);
				chunk = 0;
				scratch = 0;
			}
		}
	}

	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);
}

static void lowrisc_sd_start_cmd(struct lowrisc_sd_host *host, struct mmc_command *cmd)
{
  int setting = 0;
  int timeout = 1000000;
  struct mmc_data *data = host->data;
  volatile uint32_t *sd_base = host->ioaddr;
  spin_lock(&host->lock);

  LOGV "Command opcode: %d\n", cmd->opcode);
/*
  if (cmd->opcode == MMC_STOP_TRANSMISSION) {
    sd_cmd(host, SD_STOPINT_ISSUE_CMD12);

    cmd->resp[0] = cmd->opcode;
    cmd->resp[1] = 0;
    cmd->resp[2] = 0;
    cmd->resp[3] = 0;
    
    lowrisc_sd_finish_request(host);
    return;
  }
*/
  if (!(cmd->flags & MMC_RSP_PRESENT))
    setting = 0;
  else if (cmd->flags & MMC_RSP_136)
    setting = 3;
  else if (cmd->flags & MMC_RSP_BUSY)
    setting = 1;
  else
    setting = 1;
  setting |= host->width_setting;
  
  host->cmd = cmd;
  
  if (cmd->opcode == MMC_APP_CMD)
    {
      /* placeholder */
    }
  
  if (cmd->opcode == MMC_GO_IDLE_STATE)
    {
      /* placeholder */
    }

LOGV "testing data flags\n");
  if (data) {
    setting |= 0x4;
    sd_blkcnt(host, data->blocks);
    sd_blksize(host, data->blksz&0xFFF);
#if 0    
    if (data->blocks > 1) {
      sd_cmd(host, SD_STOPINT_AUTO_ISSUE_CMD12);
      c |= SD_CMD_MULTI_BLOCK;
    }
#endif
    if (data->flags & MMC_DATA_READ)
      setting |= 0x10;
    else
      {
      setting |= 0x8;
      //      minion_sdhci_write_block_pio(host);
      }
    /* MMC_DATA_WRITE does not require a bit to be set */
  }

LOGV "writing registers\n");
  /* Send the command */
  sd_reset(host, 0,1,0,1);
  sd_align(host, 0);
  sd_arg(host, cmd->arg);
  sd_cmd(host, cmd->opcode);
  sd_setting(host, setting);
  sd_cmd_start(host, 0);
  sd_reset(host, 0,1,1,1);
  sd_timeout(host, timeout);
  /* start the transaction */ 
  sd_cmd_start(host, 1);
 LOGV "enabling interrupt\n");
  sd_irq_en(host, sd_base[irq_en_resp] | SD_CARD_RESP_END);
spin_unlock(&host->lock);
 LOGV "leaving lowrisc_sd_start_cmd\n");
}

static void lowrisc_sd_start_data(struct lowrisc_sd_host *host, struct mmc_data *data)
{
	unsigned int flags = SG_MITER_ATOMIC;

	LOG "setup data transfer: blocksize %08x  nr_blocks %d, offset: %08x\n",
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
	if (sd_base[detect_resp]) {
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
        volatile uint32_t *sd_base = host->ioaddr;
	return sd_base[detect_resp];
}

static int lowrisc_sd_get_cd(struct mmc_host *mmc)
{
	struct lowrisc_sd_host *host = mmc_priv(mmc);
        volatile uint32_t *sd_base = host->ioaddr;

	return !sd_base[detect_resp];
}

static int lowrisc_sd_card_busy(struct mmc_host *mmc)
{
	struct lowrisc_sd_host *host = mmc_priv(mmc);
        volatile uint32_t *sd_base = host->ioaddr;
	u32 present_state;
	return sd_base[resp0] >> 31;
}

static struct mmc_host_ops lowrisc_sd_ops = {
	.request = lowrisc_sd_request,
	.set_ios = lowrisc_sd_set_ios,
	.get_ro = lowrisc_sd_get_ro,
	.get_cd = lowrisc_sd_get_cd,
	.card_busy = lowrisc_sd_card_busy,
};


static void lowrisc_sd_powerdown(struct lowrisc_sd_host *host)
{
  volatile uint32_t *sd_base = host->ioaddr;
  /* mask all interrupts */
  sd_base[irq_en_reg] = 0;
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
	mmc->max_blk_count = 1;
	
	spin_lock_init(&host->lock);

	lowrisc_sd_init(host);

	ret = request_threaded_irq(host->irq, lowrisc_sd_irq, lowrisc_sd_thread_irq,
				   IRQF_SHARED, DRIVER_NAME, host);
	if (ret)
		goto unmap;

	mmc_add_host(mmc);

	dev_dbg(&pdev->dev, "lowrisc-sd driver loaded, mapped to address %p, IRQ %d\n", host->ioaddr, host->irq);
	sd_irq_en(host, SD_CARD_CARD_INSERTED_0 | SD_CARD_CARD_REMOVED_0); /* get an interrupt either way */
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
	{ .compatible = "riscv,lowrisc" },
	{ }
};

MODULE_DEVICE_TABLE(of, lowrisc_sd_of_match);

static struct platform_driver lowrisc_sd_driver = {
	.driver = {
		.name = "lowrisc-sd",
		.of_match_table = lowrisc_sd_of_match,
	},
	.probe = lowrisc_sd_probe,
	.remove = lowrisc_sd_remove,
};

module_platform_driver(lowrisc_sd_driver);

MODULE_AUTHOR("Ondrej Zary, Richard Betts, Jonathan Kimmitt");
MODULE_DESCRIPTION("LowRISC Secure Digital Host Controller Interface driver");
MODULE_LICENSE("GPL");
