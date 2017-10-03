//#define SDHCI_VERBOSE3
//#define SDHCI_VERBOSE2
#define SDHCI_VERBOSE
/*
 * drivers/mmc/host/sdhci-of-minion.c
 *
 * LowRISC Minion Secure Digital Host Controller Interface
 *
 * Based on sdhci-of-hlwd.c
 *
 * Nintendo Wii Secure Digital Host Controller Interface.
 * Copyright (C) 2009 The GameCube Linux Team
 * Copyright (C) 2009 Albert Herranz
 *
 * Based on sdhci-of-esdhc.c
 *
 * Copyright (c) 2007 Freescale Semiconductor, Inc.
 * Copyright (c) 2009 MontaVista Software, Inc.
 *
 * Authors: Xiaobo Xie <X.Xie@freescale.com>
 *	    Anton Vorontsov <avorontsov@ru.mvista.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or (at
 * your option) any later version.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/platform_device.h>
#include <asm/config-string.h>

#include "sdhci-pltfm.h"
//#include "sdhci-minion-hash-md5.h"
#include <stdarg.h>

extern int echo;
void write_led(uint32_t data);
void myputhex(unsigned n, unsigned width);
void sd_setting(int setting);
void sd_cmd_start(int sd_cmd);

extern int printf (const char *, ...);
extern void uart_init (void);
extern void uart_send (uint8_t);
extern void uart_send_string (const char *);
extern void uart_send_buf (const char *, const int32_t);
extern uint8_t uart_recv (void);
extern void uart_send (uint8_t data);
extern void uart_init (void);
extern void uart_send_buf (const char *buf, const int32_t len);
extern uint8_t uart_recv (void);
extern void uart_send_string (const char *str);
extern void cpu_perf_set (unsigned int counterId, unsigned int value);
extern void illegal_insn_handler_c (void);
extern void int_time_cmp (void);
extern void int_main (void);
extern void uart_set_cfg (int parity, uint16_t clk_counter);
extern void __libc_init_array (void);
extern char uart_getchar (void);
extern void uart_wait_tx_done (void);
extern void uart_sendchar (const char c);
extern void mystatus (void);
// SDCARD entry point
void spi_init(void);
unsigned int sd_resp(int);
void sd_timeout(int d_timeout);
void sd_blksize(int d_blksize);
void sd_blkcnt(int d_blkcnt);
void rx_write_fifo(unsigned int data);
unsigned int rx_read_fifo(void);
int sd_read_sector(int, void *, int);
void open_handle(void);
void uart_printf(const char *fmt, ...);
void log_printf(const char *fmt, ...);
void uart_write(volatile unsigned int * const sd_ptr, unsigned val);
int cli_readline_into_buffer(const char *const prompt, char *buffer, int timeout);

int edcl_main(void);
  void edcl_loadelf(const char *elf);
  void edcl_close(void);
  int edcl_read(uint64_t addr, int bytes, uint8_t *obuf);
  int edcl_write(uint64_t addr, int bytes, uint8_t *ibuf);

typedef unsigned int uint;

void myputs(const char *str);
void sdhci_write(struct sdhci_host *host, uint32_t val, int reg);
uint32_t sdhci_read(struct sdhci_host *host, int reg);
void sdhci_reset(struct sdhci_host *host, uint8_t mask);
void minion_dispatch(const char *ucmd);
void sd_reset(int sd_rst, int clk_rst, int data_rst, int cmd_rst);
void sd_arg(unsigned arg);
void sd_align(int d_align);
void sd_clk_div(int clk_div);
void sd_cmd(unsigned cmd);
void board_mmc_power_init(void);
int init_sd(void);

/*
 * Host SDMA buffer boundary. Valid values from 4K to 512K in powers of 2.
 */
#define SDHCI_DEFAULT_BOUNDARY_SIZE	(512 * 1024)

/*
 * No command will be sent by driver if card is busy, so driver must wait
 * for card ready state.
 * Every time when card is busy after timeout then (last) timeout value will be
 * increased twice but only if it doesn't exceed global defined maximum.
 * Each function call will use last timeout value.
 */
#define SDHCI_CMD_MAX_TIMEOUT			3200
#define SDHCI_CMD_DEFAULT_TIMEOUT		100
#define SDHCI_READ_STATUS_TIMEOUT		1000

typedef __signed__ char __s8;
typedef unsigned char __u8;
typedef __signed__ short __s16;
typedef unsigned short __u16;
typedef __signed__ int __s32;
typedef unsigned int __u32;
typedef __signed__ long long __s64;
typedef unsigned long long __u64;
typedef __u16 __le16;
typedef __u16 __be16;
typedef __u32 __le32;
typedef __u32 __be32;

uint32_t to_cpu(uint32_t arg)
{
  return (__builtin_constant_p((__u32)(( __u32)(__be32)(arg))) ? ((__u32)( (((__u32)((( 
__u32)(__be32)(arg))) & (__u32)0x000000ffUL) << 24) | (((__u32)((( __u32)(__be32)(arg)))
 & (__u32)0x0000ff00UL) << 8) | (((__u32)((( __u32)(__be32)(arg))) & (__u32)0x00ff0000UL
) >> 8) | (((__u32)((( __u32)(__be32)(arg))) & (__u32)0xff000000UL) >> 24) )) : __fswab32((( __u32)(__be32)(arg))));
}

void sd_disable(void) {

}

uint8_t sd_send(uint8_t dat) {
  return 0;
}

void sd_send_multi(const uint8_t* dat, uint8_t n) {
}

void sd_recv_multi(uint8_t* dat, uint8_t n) {
}

void sd_select_slave(uint8_t id) {
}

void sd_deselect_slave(uint8_t id) {
}

/*-----------------------------------------------------------------------*/
/* Wait for card ready                                                   */
/*-----------------------------------------------------------------------*/

int wait_ready (                /* 1:Ready, 0:Timeout */
                uint32_t wt     /* Timeout [ms] */
                                )
{
  return 1;
}



/*-----------------------------------------------------------------------*/
/* Deselect the card                                                     */
/*-----------------------------------------------------------------------*/

void sd_deselect (void)
{
  sd_deselect_slave(0);
}

/*-----------------------------------------------------------------------*/
/* Select the card and wait for ready                                    */
/*-----------------------------------------------------------------------*/

int sd_select (void)   /* 1:Successful, 0:Timeout */
{
  sd_select_slave(0);
  if (wait_ready(500)) return 1;  /* Wait for card ready */

  sd_deselect();
  return 0;   /* Timeout */
}

void myputs(const char *str)
{
  printk("%s", str);
}

void myputn(uint32_t n)
{
  printk("%u", n);
}

void myputhex(uint32_t n, uint32_t width)
{
  printk("%*X", width, n);
}

uint32_t sd_resp(int);
void sd_timeout(int d_timeout);
void sd_blksize(int d_blksize);
void sd_blkcnt(int d_blkcnt);
void tx_write_fifo(uint32_t data);
uint32_t rx_read_fifo(void);

void open_handle(void);
void uart_printf(const char *fmt, ...);
void log_printf(const char *fmt, ...);
void uart_write(volatile uint32_t * const sd_ptr, uint32_t val);
int cli_readline_into_buffer(const char *const prompt, char *buffer, int timeout);

void myputs(const char *str);
void sdhci_write(struct sdhci_host *host, uint32_t val, int reg);
uint32_t sdhci_read(struct sdhci_host *host, int reg);
void sdhci_reset(struct sdhci_host *host, uint8_t mask);

void minion_dispatch(const char *ucmd);

/* HID peripheral address space pointer */
static u64 sd_addr, sdtx_addr, sdrx_addr;
static volatile uint32_t *sd_base, *sdtx_base, *sdrx_base;

void tx_write_fifo(uint32_t data)
{
  sdtx_base[0] = data;
}

void rx_write_fifo(uint32_t data)
{
  sdrx_base[0] = data;
}

uint32_t rx_read_fifo(void)
{
  return sdrx_base[0];
}

void write_led(uint32_t data)
{
  sd_base[15] = data;
}

uint32_t sd_resp(int sel)
{
  uint32_t rslt = sd_base[sel];
  return rslt;
}

void sd_align(int d_align)
{
  sd_base[0] = d_align;
}

void sd_clk_div(int clk_div)
{
  sd_base[1] = clk_div;
}

void sd_arg(uint32_t arg)
{
  sd_base[2] = arg;
}

void sd_cmd(uint32_t cmd)
{
  sd_base[3] = cmd;
}

void sd_setting(int setting)
{
  sd_base[4] = setting;
}

void sd_cmd_start(int sd_cmd)
{
  sd_base[5] = sd_cmd;
}

void sd_reset(int sd_rst, int clk_rst, int data_rst, int cmd_rst)
{
  sd_base[6] = ((sd_rst&1) << 3)|((clk_rst&1) << 2)|((data_rst&1) << 1)|((cmd_rst&1) << 0);
}

void sd_blkcnt(int d_blkcnt)
{
  sd_base[7] = d_blkcnt&0xFFFF;
}

void sd_blksize(int d_blksize)
{
  sd_base[8] = d_blksize&0xFFF;
}

void sd_timeout(int d_timeout)
{
  sd_base[9] = d_timeout;
}

static int sdhci_host_control;
static int sdhci_power_control;
static int sdhci_block_gap;
static int sdhci_wake_up;
static int sdhci_timeout_control;
static int sdhci_software_reset;
static int sdhci_clock_div;
static int sdhci_int_status;
static int sdhci_int_enable;
static int sdhci_signal_enable;
static int sdhci_present_state;
static int sdhci_max_current;
static int sdhci_set_acmd12_error;
static int sdhci_acmd12_err;
static int sdhci_set_int;
static int sdhci_slot_int_status;
static int sdhci_host_version;
static int sdhci_transfer_mode;
static int sdhci_dma_address;
static int sdhci_block_count;
static int sdhci_block_size;
static int sdhci_command;
static int sdhci_argument;
static int sdhci_host_control2;

#define get_card_status(verbose) _get_card_status(__LINE__, verbose)

uint32_t card_status[32];

static void _get_card_status(int line, int verbose)
{
  memcpy(card_status, (const void *)sd_base, sizeof(card_status)); 
#ifdef SDHCI_VERBOSE3
  {
  int i;
  static uint32_t old_card_status[32];
  for (i = 0; i < 26; i++) if (verbose || (card_status[i] != old_card_status[i]))
      {
	printk("line(%d), card_status[%d]=%.8X\n", line, i, card_status[i]);
	old_card_status[i] = card_status[i];
      }
  }
#endif      
}

static void minion_sdhci_do_reset(struct sdhci_host *host, u8 mask)
{
	host->ops->reset(host, mask);
	//	host->mmc->caps2 |= MMC_CAP2_NO_SDIO;

	if (mask & SDHCI_RESET_ALL) {
		if (host->flags & (SDHCI_USE_SDMA | SDHCI_USE_ADMA)) {
			if (host->ops->enable_dma)
				host->ops->enable_dma(host);
		}

		/* Resetting the controller clears many */
		host->preset_enabled = false;
	}
}

static void minion_sdhci_finish_data(struct sdhci_host *host)
{
	struct mmc_data *data;

	BUG_ON(!host->data);

	data = host->data;
	host->data = NULL;

	/*
	 * The specification states that the block count register must
	 * be updated, but it does not specify at what point in the
	 * data flow. That makes the register entirely useless to read
	 * back so we have to assume that nothing made it to the card
	 * in the event of an error.
	 */
	if (data->error)
		data->bytes_xfered = 0;
	else
		data->bytes_xfered = data->blksz * data->blocks;

	/*
	 * Need to send CMD12 if -
	 * a) open-ended multiblock transfer (no CMD23)
	 * b) error in multiblock transfer
	 */
	if (data->stop &&
	    (data->error ||
	     !host->mrq->sbc)) {

		/*
		 * The controller needs a reset of internal state machines
		 * upon error conditions.
		 */
		if (data->error) {
			minion_sdhci_do_reset(host, SDHCI_RESET_CMD);
			minion_sdhci_do_reset(host, SDHCI_RESET_DATA);
		}

		sdhci_send_command(host, data->stop);
	} else
		tasklet_schedule(&host->finish_tasklet);
}

static void minion_sdhci_read_block_pio(struct sdhci_host *host)
{
	unsigned long flags;
	size_t blksize, len, chunk;
	u32 uninitialized_var(scratch);
	u8 *buf;
	int i = 0;
#ifdef SDHCI_MD5
	md5_ctx_t context;
#endif
#ifdef SDHCI_VERBOSE3
	printk("PIO reading\n");
#endif
	blksize = host->data->blksz;
	chunk = 0;

	local_irq_save(flags);

#ifdef SDHCI_MD5
	md5_begin(&context);
#endif	
	while (blksize) {
	  int idx = 0;
	  BUG_ON(!sg_miter_next(&host->sg_miter));
	  
	  len = min(host->sg_miter.length, blksize);
	  
	  blksize -= len;
	  host->sg_miter.consumed = len;
	  
	  buf = host->sg_miter.addr;
	  
	  while (len) {
	    if (chunk == 0) {
	      rx_write_fifo(0);
	      scratch =  __be32_to_cpu(rx_read_fifo());
	      i++;
	      chunk = 4;
	    }
	    
	    buf[idx] = scratch & 0xFF;	    
	    idx++;
	    scratch >>= 8;
	    chunk--;
	    len--;
	  }

#ifdef SDHCI_MD5
	  md5_hash(&context, buf, idx);
#endif
	}
	
#ifdef SDHCI_MD5
	md5_end(&context);
	printk("arg=%X, md5 = %s\n", sdhci_argument, hash_bin_to_hex(&context));
#endif
	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);
}

static void minion_sdhci_write_block_pio(struct sdhci_host *host)
{
	unsigned long flags;
	size_t blksize, len, chunk;
	u32 scratch;
	u8 *buf;

#ifdef SDHCI_VERBOSE3
	printk("PIO writing\n");
#endif

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
			  tx_write_fifo(__cpu_to_be32(scratch));
				chunk = 0;
				scratch = 0;
			}
		}
	}

	sg_miter_stop(&host->sg_miter);

	local_irq_restore(flags);
}

static void minion_sdhci_finish_command(struct sdhci_host *host)
{
	int i;

	BUG_ON(host->cmd == NULL);

	if (host->cmd->flags & MMC_RSP_PRESENT) {
		if (host->cmd->flags & MMC_RSP_136) {
			/* CRC is stripped so we need to do some shifting. */
			for (i = 0;i < 4;i++) {
				host->cmd->resp[i] = sdhci_readl(host,
					SDHCI_RESPONSE + (3-i)*4) << 8;
				if (i != 3)
					host->cmd->resp[i] |=
						sdhci_readb(host,
						SDHCI_RESPONSE + (3-i)*4-1);
			}
		} else {
			host->cmd->resp[0] = sdhci_readl(host, SDHCI_RESPONSE);
		}
	}

	/* Finished CMD23, now send actual command. */
	if (host->cmd == host->mrq->sbc) {
		host->cmd = NULL;
		sdhci_send_command(host, host->mrq->cmd);
	} else {

		/* Processed actual command. */
		if (host->data && host->data_early)
			minion_sdhci_finish_data(host);

		if (!host->cmd->data)
			tasklet_schedule(&host->finish_tasklet);

		host->cmd = NULL;
	}
}

void sd_transaction_finish(struct sdhci_host *host, int cmd_flags)
{
  int setting = 0;
  static int bad, good;
  uint32_t timeout, stat, wait, timedout = 0;
  switch(sdhci_command & SDHCI_CMD_RESP_MASK)
      {
      case SDHCI_CMD_RESP_NONE: setting = 0; break;
      case SDHCI_CMD_RESP_SHORT: setting = 1; break;
      case SDHCI_CMD_RESP_SHORT_BUSY: setting = 1; break;
      case SDHCI_CMD_RESP_LONG: setting = 3; break;
      }
  if (sdhci_host_control & SDHCI_CTRL_4BITBUS) setting |= 0x20;
  if (sdhci_command & SDHCI_CMD_DATA)
      {
	timeout = sdhci_timeout_control;
	setting |= (sdhci_transfer_mode & SDHCI_TRNS_READ ? 0x10 : 0x8) | 0x4;
      }
  else
    timeout = 15;
      sd_reset(0,1,0,1);
      sd_align(0);
      sd_arg(sdhci_argument);
      sd_cmd(cmd_flags >> 8);
      sd_setting(setting);
      sd_cmd_start(0);
      sd_reset(0,1,1,1);
      sd_blkcnt(sdhci_block_count);
      sd_blksize(sdhci_block_size&0xFFF);
      sd_timeout(timeout);
      //      printk("Timeout control = %d\n", sdhci_timeout_control);
      get_card_status(0);
      /* drain rx fifo, if needed */
      while (1 & ~sd_base[5])
	{
	  rx_write_fifo(0);
	}
      if ((sdhci_command & SDHCI_CMD_DATA) && !(sdhci_transfer_mode & SDHCI_TRNS_READ))
        {
          minion_sdhci_write_block_pio(host);
        }
      sd_cmd_start(1);
      get_card_status(1);
      timeout = 0;
      do
	{
	  get_card_status(0);
	  stat = card_status[5];
	  wait = stat & 0x100;
	}
      while ((wait != 0x100) && (card_status[4] < card_status[25]) && (timeout++ < 1000000));
    #ifdef SDHCI_VERBOSE2
      {
      int i;
      printk("%.4X:%.8X->", card_status[7], card_status[6]);
      for (i = 4; i--; )
	{
	  printk("%.8X,", card_status[i]);
	}
      printk("%.8X,%.8X\n", card_status[5], card_status[4]);
      }
    #endif
      memcpy(card_status, (const void *)sd_base, sizeof(card_status)); 
      if (sdhci_command & SDHCI_CMD_DATA)
	{
	  do
	    {
	       get_card_status(0);
	       stat = card_status[5];
	       wait = stat & 0x400;
	     }
	  while ((wait != 0x400) && (card_status[8] < card_status[25]));
	  if ((card_status[8] < card_status[25]) && card_status[9])
	    {
	      if (sdhci_transfer_mode & SDHCI_TRNS_READ)
		{
		  while (host->blocks--)
		    {
		      minion_sdhci_read_block_pio(host);
		    }
		}
	    }
	  else
	    {
	      timedout = 1;
	    }
	}
  if (card_status[4] < card_status[25])
    minion_sdhci_finish_command(host);
  else
    {
    host->cmd->error = -ETIMEDOUT;
    tasklet_schedule(&host->finish_tasklet);
    }
  if (sdhci_command & SDHCI_CMD_DATA)
    {
      if (timedout)
	{
	  ++bad;
	    host->data->error = -ETIMEDOUT;
	    printk("bad = %d/%d (%d%%)\n", bad, good, bad*100/good);
	}
      else
	++good;
      minion_sdhci_finish_data(host);
    }
#ifdef SDHCI_VERBOSE3
  printk("sd_transaction_finish stopping\n");
#endif
  sd_cmd_start(0);
  sd_setting(0);
#ifdef SDHCI_VERBOSE3
  printk("sd_transaction_finish ended\n");
#endif
}

void sdhci_minion_hw_reset(struct sdhci_host *host)
{
#ifdef SDHCI_VERBOSE3
  printk("sdhci_minion_hw_reset();\n");
#endif
  write_led(0x55);
}

#ifdef SDHCI_VERBOSE

const char *sdhci_kind(int reg)
{  
  switch (reg)
    {
    case SDHCI_DMA_ADDRESS      : return "SDHCI_DMA_ADDRESS";
    case SDHCI_ARGUMENT	        : return "SDHCI_ARGUMENT";
    case SDHCI_BLOCK_COUNT	: return "SDHCI_BLOCK_COUNT";
    case SDHCI_BLOCK_GAP_CONTROL: return "SDHCI_BLOCK_GAP_CONTROL";
    case SDHCI_BLOCK_SIZE	: return "SDHCI_BLOCK_SIZE";
    case SDHCI_BUFFER           : return "SDHCI_BUFFER ";
    case SDHCI_CAPABILITIES     : return "SDHCI_CAPABILITIES";
    case SDHCI_CLOCK_CONTROL	: return "SDHCI_CLOCK_CONTROL";
    case SDHCI_COMMAND	        : return "SDHCI_COMMAND";
    case SDHCI_HOST_CONTROL	: return "SDHCI_HOST_CONTROL";
    case SDHCI_HOST_VERSION	: return "SDHCI_HOST_VERSION";
    case SDHCI_INT_ENABLE	: return "SDHCI_INT_ENABLE";
    case SDHCI_INT_STATUS	: return "SDHCI_INT_STATUS";
    case SDHCI_MAX_CURRENT	: return "SDHCI_MAX_CURRENT";
    case SDHCI_POWER_CONTROL	: return "SDHCI_POWER_CONTROL";
    case SDHCI_PRESENT_STATE	: return "SDHCI_PRESENT_STATE";
    case SDHCI_RESPONSE+12      : return "SDHCI_RESPONSE+12 ";
    case SDHCI_RESPONSE+4       : return "SDHCI_RESPONSE+4";
    case SDHCI_RESPONSE+8       : return "SDHCI_RESPONSE+8";
    case SDHCI_RESPONSE         : return "SDHCI_RESPONSE";
    case SDHCI_SET_ACMD12_ERROR	: return "SDHCI_SET_ACMD12_ERROR";
    case SDHCI_SET_INT_ERROR	: return "SDHCI_SET_INT_ERROR";
    case SDHCI_SIGNAL_ENABLE	: return "SDHCI_SIGNAL_ENABLE";
    case SDHCI_SLOT_INT_STATUS	: return "SDHCI_SLOT_INT_STATUS";
    case SDHCI_SOFTWARE_RESET	: return "SDHCI_SOFTWARE_RESET";
    case SDHCI_TIMEOUT_CONTROL	: return "SDHCI_TIMEOUT_CONTROL";
    case SDHCI_TRANSFER_MODE	: return "SDHCI_TRANSFER_MODE";
    case SDHCI_WAKE_UP_CONTROL	: return "SDHCI_WAKE_UP_CONTROL";
    case SDHCI_ACMD12_ERR       : return "SDHCI_ACMD12_ERR";
    case SDHCI_HOST_CONTROL2    : return "SDHCI_HOST_CONTROL2";
    case SDHCI_CAPABILITIES_1   : return "SDHCI_CAPABILITIES_1";
    default                     : return "unknown";
    }
}
#endif  

void sdhci_write(struct sdhci_host *host, uint32_t val, int reg)
{
#ifdef SDHCI_VERBOSE2
  if (reg != SDHCI_HOST_CONTROL) printk("sdhci_write(&host, 0x%x, %s);\n", val, sdhci_kind(reg));
#endif
  switch (reg)
    {
    case SDHCI_DMA_ADDRESS      :
      printk("DMA_address = %X\n", val);
      sdhci_dma_address = val;
      break;
    case SDHCI_BLOCK_COUNT	:
      sdhci_block_count = val;
      break;
    case SDHCI_BLOCK_SIZE	        :
      sdhci_block_size = val;
      break;
    case SDHCI_HOST_CONTROL	:
      if ((val^sdhci_host_control) & SDHCI_CTRL_4BITBUS)
	{
	  if (val & SDHCI_CTRL_4BITBUS)
	    printk("4-bit bus enabled\n");
	  else
	    printk("4-bit bus disabled\n");
	}
      if (sdhci_host_control != val)
	write_led(sdhci_host_control);
      sdhci_host_control = val;
      break;
    case SDHCI_ARGUMENT	        :
      sdhci_argument = val;
      break;
    case SDHCI_TRANSFER_MODE	:
      sdhci_transfer_mode = val;
      break;
    case SDHCI_POWER_CONTROL	:
      if (val & SDHCI_POWER_ON)
	{
	  sdhci_minion_hw_reset(host);
	  sd_reset(0,1,0,0);
	  get_card_status(0);
	  sd_align(0);
	  sd_reset(0,1,1,1);
	  switch (val & ~SDHCI_POWER_ON)
	    {
	    case SDHCI_POWER_180: printk("SD Power = 1.8V\n"); break;
	    case SDHCI_POWER_300: printk("SD Power = 3.0V\n"); break;
	    case SDHCI_POWER_330: printk("SD Power = 3.3V\n"); break;
	    }
	}
      else
	{
	printk("SD Power off\n"); 
	sd_reset(1,0,0,0);
	}
      sdhci_power_control = val;
      break;
    case SDHCI_COMMAND	        :
      sdhci_command = val;
      sd_transaction_finish(host, sdhci_command);
      break;
    case SDHCI_BLOCK_GAP_CONTROL	: sdhci_block_gap = val; break;
    case SDHCI_WAKE_UP_CONTROL	: sdhci_wake_up = val; break;
    case SDHCI_TIMEOUT_CONTROL	:
      sdhci_timeout_control = 2500000 / sdhci_clock_div;
      if (sdhci_timeout_control < val) sdhci_timeout_control = val;
      break;
    case SDHCI_SOFTWARE_RESET	:
      sdhci_software_reset = val;
      sdhci_transfer_mode = 0;
      if (val & SDHCI_RESET_ALL) sdhci_minion_hw_reset(host);
      get_card_status(0);      
      break;
    case SDHCI_CLOCK_CONTROL	:
      sdhci_clock_div = val >> SDHCI_DIVIDER_SHIFT;
      if (sdhci_clock_div)
	{
	  //	  printk("Trying clock div = %d\n", sdhci_clock_div);
	  if (sdhci_clock_div < 2) sdhci_clock_div = 2;
	  if (sdhci_clock_div > 512) sdhci_clock_div = 512;
#ifdef VERBOSE
	  printk("Actual clock divider = %d\n", sdhci_clock_div);
#endif
	  sd_clk_div(sdhci_clock_div/2 - 1);
	  sdhci_timeout_control = 2500000 / sdhci_clock_div;
	  get_card_status(0);
	}
      if (val & SDHCI_CLOCK_CARD_EN)
	{
	  sd_reset(0,1,1,1);
#ifdef VERBOSE
	  printk("Card clock enabled\n");
#endif
	  get_card_status(0);
	}
      else
	{
	  sd_reset(0,0,1,1);
#ifdef VERBOSE
	  printk("Card clock disabled\n");
#endif
	  get_card_status(0);
	}
      break;
    case SDHCI_INT_STATUS	:
      sdhci_int_status = val;
      break;
    case SDHCI_INT_ENABLE	: sdhci_int_enable = val; break;
    case SDHCI_SIGNAL_ENABLE	: sdhci_signal_enable = val; break;
    case SDHCI_PRESENT_STATE	: sdhci_present_state = val; break;
    case SDHCI_MAX_CURRENT	: sdhci_max_current = val; break;
    case SDHCI_BUFFER           : printk("spurious tx_write_fifo\n"); break;
    case SDHCI_SET_ACMD12_ERROR	: sdhci_set_acmd12_error = val; break;
    case SDHCI_SET_INT_ERROR	: sdhci_set_int = val; break;
    case SDHCI_HOST_VERSION	: sdhci_host_version = val; break;
    case SDHCI_HOST_CONTROL2    : sdhci_host_control2 = val; break;
    case SDHCI_ACMD12_ERR       : sdhci_acmd12_err = val; break;
    case SDHCI_SLOT_INT_STATUS  : sdhci_slot_int_status = val; break;
    default: printk("unknown(0x%X)", reg);
    }
}

uint32_t sdhci_read(struct sdhci_host *host, int reg)
{
  uint32_t rslt = 0;
  switch (reg)
    {
    case SDHCI_DMA_ADDRESS       : rslt = sdhci_dma_address; break;
    case SDHCI_BLOCK_COUNT	 : rslt = sdhci_block_count; break;
    case SDHCI_BLOCK_SIZE	 : rslt = sdhci_block_size; break;
    case SDHCI_HOST_CONTROL      : rslt = sdhci_host_control; break;
    case SDHCI_ARGUMENT          : rslt = sdhci_argument; break;
    case SDHCI_TRANSFER_MODE	 : rslt = sdhci_transfer_mode; break;
    case SDHCI_COMMAND	         : rslt = sdhci_command; break;
    case SDHCI_RESPONSE          : rslt = card_status[0]; break;
    case SDHCI_RESPONSE+4        : rslt = card_status[1]; break;
    case SDHCI_RESPONSE+8        : rslt = card_status[2]; break;
    case SDHCI_RESPONSE+12       : rslt = card_status[3]; break;
    case SDHCI_INT_STATUS	 : rslt = sdhci_int_status; break;
    case SDHCI_INT_ENABLE	 : rslt = sdhci_int_enable; break;
    case SDHCI_PRESENT_STATE	 : 
      sdhci_present_state = card_status[12] ? 0 : SDHCI_CARD_PRESENT;
      rslt = sdhci_present_state; break;
    case SDHCI_HOST_VERSION	 : rslt = SDHCI_SPEC_300; break;
    case SDHCI_CAPABILITIES      :
      rslt = SDHCI_CAN_VDD_330|(12 << SDHCI_CLOCK_BASE_SHIFT)|SDHCI_CAN_DO_HISPD;
      break;
    case SDHCI_SOFTWARE_RESET    : rslt = 0; break;
    case SDHCI_BLOCK_GAP_CONTROL : rslt = sdhci_block_gap; break;
    case SDHCI_CLOCK_CONTROL     : rslt = (sdhci_clock_div << SDHCI_DIVIDER_SHIFT)|SDHCI_CLOCK_INT_STABLE; break;
    case SDHCI_BUFFER            : printk("spurious rx_read_fifo\n"); rslt = 0; break;
    case SDHCI_MAX_CURRENT       : rslt = 0; break;
    case SDHCI_POWER_CONTROL	 : rslt = sdhci_power_control; break;
    case SDHCI_WAKE_UP_CONTROL	 : rslt = sdhci_wake_up; break;
    case SDHCI_TIMEOUT_CONTROL	 : rslt = sdhci_timeout_control; break;
    case SDHCI_SIGNAL_ENABLE	 : rslt = sdhci_signal_enable; break;
    case SDHCI_SET_ACMD12_ERROR	 : rslt = sdhci_set_acmd12_error; break;
    case SDHCI_HOST_CONTROL2     : rslt = sdhci_host_control2; break;
    case SDHCI_ACMD12_ERR        : rslt = sdhci_acmd12_err; break;
    case SDHCI_CAPABILITIES_1    : rslt = MMC_CAP2_NO_SDIO; break;
    case SDHCI_SLOT_INT_STATUS   : rslt = SDHCI_INT_RESPONSE; break;
    default: printk("unknown(0x%X)", reg);
    }
#ifdef SDHCI_VERBOSE2
  if ((reg != SDHCI_PRESENT_STATE) && (reg != SDHCI_HOST_CONTROL))
    printk("sdhci_read(&host, %s) => %X;\n", sdhci_kind(reg), rslt);
#endif  
  return rslt;
}

#ifdef CONFIG_MMC_SDHCI_IO_ACCESSORS
static inline u32 sdhci_minion_readl(struct sdhci_host *host, int reg)
{
#ifdef VERBOSE  
  printk("sdhci_minion_readl(%X)\n", reg);
#endif
  return sdhci_read(host, reg);
}

static inline u16 sdhci_minion_readw(struct sdhci_host *host, int reg)
{
#ifdef VERBOSE  
  printk("sdhci_minion_readw(%X)\n", reg);
#endif
  return sdhci_read(host, reg);
}

static inline u8 sdhci_minion_readb(struct sdhci_host *host, int reg)
{
#ifdef VERBOSE  
  printk("sdhci_minion_readb(%X)\n", reg);
#endif
  if (reg >= SDHCI_RESPONSE && reg <= SDHCI_RESPONSE+15)
    return card_status[(reg-SDHCI_RESPONSE) >> 2] >> (reg&3)*8;
  else
    return sdhci_read(host, reg);
}

static inline void sdhci_minion_writel(struct sdhci_host *host,  u32 val, int reg)
{
#ifdef VERBOSE  
  printk("sdhci_minion_writel(%X)\n", reg);
#endif
  sdhci_write(host, val, reg);
}

static void sdhci_minion_writew(struct sdhci_host *host, u16 val, int reg)
{
#ifdef VERBOSE  
  printk("sdhci_minion_writew(%X)\n", reg);
#endif
  sdhci_write(host, val, reg);
}

static void sdhci_minion_writeb(struct sdhci_host *host, u8 val, int reg)
{
#ifdef VERBOSE  
  printk("sdhci_minion_writeb(%X)\n", reg);
#endif
  sdhci_write(host, val, reg);
}
#else
#error "CONFIG_MMC_SDHCI_IO_ACCESSORS must be enabled in Kconfig"
#endif

unsigned int sdhci_minion_get_max_clock(struct sdhci_host *host)
 {
   return 200000000U;
 }

unsigned int sdhci_minion_get_min_clock(struct sdhci_host *host)
 {
   return 400000U;
 }
 
static unsigned int sdhci_minion_get_ro(struct sdhci_host *host)
{
        /*
         * The SDHCI_WRITE_PROTECT bit is unstable on current hardware so we
         * can't depend on its value in any way.
         */
  if (1) return 0;
        return SDHCI_WRITE_PROTECT;
 }
 
static const struct sdhci_ops sdhci_minion_ops = {
#ifdef CONFIG_MMC_SDHCI_IO_ACCESSORS
	.read_l = sdhci_minion_readl,
	.read_w = sdhci_minion_readw,
	.read_b = sdhci_minion_readb,
	.write_l = sdhci_minion_writel,
	.write_w = sdhci_minion_writew,
	.write_b = sdhci_minion_writeb,
#endif
	.get_max_clock = sdhci_minion_get_max_clock,
	.get_min_clock = sdhci_minion_get_min_clock,
	.set_clock = sdhci_set_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset = sdhci_reset,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
	.get_ro = sdhci_minion_get_ro
};

static const struct sdhci_pltfm_data sdhci_minion_pdata = {
	.quirks = SDHCI_QUIRK_BROKEN_DMA |
		  SDHCI_QUIRK_BROKEN_ADMA |
		  SDHCI_QUIRK_DATA_TIMEOUT_USES_SDCLK | 
	          SDHCI_QUIRK_BROKEN_TIMEOUT_VAL |
	          SDHCI_QUIRK_NO_MULTIBLOCK,
	.quirks2 = SDHCI_QUIRK2_PRESET_VALUE_BROKEN,
	.ops = &sdhci_minion_ops,
};

static int sdhci_minion_probe(struct platform_device *pdev)
{
  return sdhci_pltfm_register(pdev, &sdhci_minion_pdata, 0);
}

static const struct of_device_id sdhci_minion_of_match[] = {
	{ .compatible = "riscv,minion" },
	{ }
};
MODULE_DEVICE_TABLE(of, sdhci_minion_of_match);

static struct platform_driver sdhci_minion_driver = {
	.driver = {
		.name = "sdhci-minion",
		.of_match_table = sdhci_minion_of_match,
		.pm = SDHCI_PLTFM_PMOPS,
	},
	.probe = sdhci_minion_probe,
	.remove = sdhci_pltfm_unregister,
};

module_platform_driver(sdhci_minion_driver);

static struct resource lowrisc_sd[] = {
	[0] = {
		.start = 0,
		.end   = 0xFFF,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = 0,
		.end   = 0xFFF,
		.flags = IORESOURCE_MEM,
	},
	[2] = {
		.start = 0,
		.end   = 0xFFF,
		.flags = IORESOURCE_MEM,
	},
};

static struct platform_device minion_mmc_device = {
		.name = "sdhci-minion",
		.id = -1,
		.num_resources = ARRAY_SIZE(lowrisc_sd),
		.resource = lowrisc_sd,
	};

static int __init sdhci_minion_drv_init(void)
{
	// Find config string driver
	struct device *csdev = bus_find_device_by_name(&platform_bus_type, NULL, "config-string");
	struct platform_device *pcsdev = to_platform_device(csdev);
	u64 hid_addr = config_string_u64(pcsdev, "hid.addr");
	sd_addr = hid_addr + 0x00010000;
	sdtx_addr = hid_addr + 0x00014000;
	sdrx_addr = hid_addr + 0x00018000;
	lowrisc_sd[0].start += sd_addr;
	lowrisc_sd[0].end += sd_addr;
	lowrisc_sd[1].start += sdtx_addr;
	lowrisc_sd[1].end += sdtx_addr;
	lowrisc_sd[2].start += sdrx_addr;
	lowrisc_sd[2].end += sdrx_addr;
	sd_base = (volatile uint32_t *)ioremap(sd_addr, 0x1000);
	sdtx_base = (volatile uint32_t *)ioremap(sdtx_addr, 0x1000);
	sdrx_base = (volatile uint32_t *)ioremap(sdrx_addr, 0x1000);
	printk("sdhci-minion: address %llx, remapped to %p\n", sd_addr, sd_base);
        platform_device_register(&minion_mmc_device);
	return 0;
}

module_init(sdhci_minion_drv_init);

static void __exit sdhci_minion_drv_exit(void)
{

}

module_exit(sdhci_minion_drv_exit);

MODULE_DESCRIPTION("LowRISC Minion SDHCI OF driver");
MODULE_AUTHOR("The LowRISC Linux Team, Jonathan Kimmitt");
MODULE_LICENSE("GPL v2");
