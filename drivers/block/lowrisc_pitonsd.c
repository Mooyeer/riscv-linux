/*
 * LowRISC_piton_sd device driver
 *
 * SD-Card block driver based on modified open piton FPGA driver
 * This driver based on xsysace.c (Copyright 2007 Secret Lab Technologies Ltd.)
 * Changes copyright LowRISC CIC
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 */

/*
 * The LowRISC_piton_sd FPGA uses a hardware state-machine to make an SD-Card
 * appear as a regular block device
 * This driver is a block device driver for the LowRISC_piton_sd.
 *
 * Initialization:
 *    The driver registers itself as a platform_device driver at module
 *    load time.  The platform bus will take care of calling the
 *    pitonsd_probe() method for all LowRISC_piton_sd instances in the system.  Any
 *    number of LowRISC_piton_sd instances are supported.  pitonsd_probe() calls
 *    pitonsd_setup() which initialized all data structures, reads the CF
 *    id structure and registers the device. In practice the platform supports just one instance.
 *
 * Processing:
 *    Just about all of the heavy lifting in this driver is performed by
 *    a Finite State Machine (FSM).  The driver needs to wait on a number
 *    of events; some raised by interrupts, some which need to be polled
 *    for.  Describing all of the behaviour in a FSM seems to be the
 *    easiest way to keep the complexity low and make it easy to
 *    understand what the driver is doing.  If the block ops or the
 *    request function need to interact with the hardware, then they
 *    simply need to flag the request and kick of FSM processing.
 *
 *    The FSM itself is atomic-safe code which can be run from any
 *    context.  The general process flow is:
 *    1. obtain the ace->lock spinlock.
 *    2. loop on pitonsd_fsm_dostate() until the ace->fsm_continue flag is
 *       cleared.
 *    3. release the lock.
 *
 *    Individual states do not sleep in any way.  If a condition needs to
 *    be waited for then the state much clear the fsm_continue flag and
 *    either schedule the FSM to be run again at a later time, or expect
 *    an interrupt to call the FSM when the desired condition is met.
 *
 *    In normal operation, the FSM is processed at interrupt context
 *    either when the driver's tasklet is scheduled, or when an irq is
 *    raised by the hardware.  The tasklet can be scheduled at any time.
 *    The request method in particular schedules the tasklet when a new
 *    request has been indicated by the block layer.  Once started, the
 *    FSM proceeds as far as it can processing the request until it
 *    needs on a hardware event.  At this point, it must yield execution.
 *
 *    A state has two options when yielding execution:
 *    1. pitonsd_fsm_yield()
 *       - Call if need to poll for event.
 *       - clears the fsm_continue flag to exit the processing loop
 *       - reschedules the tasklet to run again as soon as possible
 *    2. pitonsd_fsm_yieldirq()
 *       - Call if an irq is expected from the HW
 *       - clears the fsm_continue flag to exit the processing loop
 *       - does not reschedule the tasklet so the FSM will not be processed
 *         again until an irq is received.
 *    After calling a yield function, the state must return control back
 *    to the FSM main loop.
 *
 *    Additionally, the driver maintains a kernel timer which can process
 *    the FSM.  If the FSM gets stalled, typically due to a missed
 *    interrupt, then the kernel timer will expire and the driver can
 *    continue where it left off.
 *
 * To Do:
 *    - Add FPGA configuration control interface.
 *    - Request major number from lanana
 */

#undef DEBUG

#include <linux/module.h>
#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <linux/mutex.h>
#include <linux/ata.h>
#include <linux/hdreg.h>
#include <linux/platform_device.h>
#if defined(CONFIG_OF)
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#endif

MODULE_AUTHOR("Grant Likely <grant.likely@secretlab.ca>");
MODULE_DESCRIPTION("LowRISC_piton_sd device driver");
MODULE_LICENSE("GPL");

/* LowRISC_piton_sd register definitions */
#define _piton_sd_BUSMODE (0x09)

#define _piton_sd_STATUS (0x02)
#define _piton_sd_STATUS_REQ          (0x00000001)
#define _piton_sd_STATUS_WR           (0x00000002)
#define _piton_sd_STATUS_IRQ_EN       (0x00000004)	/* config controller error */
#define _piton_sd_STATUS_IRQ          (0x00000008)	/* CF controller error */
#define _piton_sd_STATUS_RDYFORCFCMD  (0x00000010)
#define _piton_sd_STATUS_CFGDONE      (0x00000020)
#define _piton_sd_STATUS_SDHC         (0x00000040)
#define _piton_sd_STATUS_CFDETECT     (0x00000080)

#define _piton_sd_ERROR  (0x03)

#define _piton_sd_VERSION (0x16)
#define _piton_sd_VERSION_REVISION_MASK (0x00FF)
#define _piton_sd_VERSION_MINOR_MASK    (0x0F00)
#define _piton_sd_VERSION_MAJOR_MASK    (0xF000)

#define _piton_sd_CTRL (0x3)
#define _piton_sd_CTRL_REQ            (0x0001)
#define _piton_sd_CTRL_WR             (0x0002)
#define _piton_sd_CTRL_IRQ_EN         (0x0004)
#define _piton_sd_CTRL_CFGRESET       (0x0008)

#define _piton_sd_NUM_MINORS 16
#define _piton_sd_SECTOR_SIZE (512)
#define _piton_sd_FIFO_SIZE (32)
#define _piton_sd_BUF_PER_SECTOR (_piton_sd_SECTOR_SIZE / _piton_sd_FIFO_SIZE)

struct pitonsd_reg_ops;

struct pitonsd_device {
	/* driver state data */
	int id;
	int media_change;
	int users;
	struct list_head list;

	/* finite state machine data */
	struct tasklet_struct fsm_tasklet;
	uint fsm_task;		/* Current activity (_piton_sd_TASK_*) */
	uint fsm_state;		/* Current state (_piton_sd_FSM_STATE_*) */
	uint fsm_continue_flag;	/* cleared to exit FSM mainloop */
	uint fsm_iter_num;
	struct timer_list stall_timer;

	/* Transfer state/result, use for both id and block request */
	struct request *req;	/* request being processed */
	void *data_ptr;		/* pointer to I/O buffer */
	int data_count;		/* number of buffers remaining */
	int data_result;	/* Result of transfer; 0 := success */

	int id_req_count;	/* count of id requests */
	int id_result;
	struct completion id_completion;	/* used when id req finishes */
	int in_irq;

	/* Details of hardware device */
	resource_size_t physaddr;
	void __iomem *baseaddr;
	int irq;
	int lock_count;

	/* Block device data structures */
	spinlock_t lock;
	struct device *dev;
	struct request_queue *queue;
	struct gendisk *gd;

	/* Inserted CF card parameters */
	u16 cf_id[ATA_ID_WORDS];
};

static DEFINE_MUTEX(pitonsd_mutex);
static int pitonsd_major;

/* ---------------------------------------------------------------------
 * Low level register access
 */

static inline u64 pitonsd_in(struct pitonsd_device *ace, int reg)
{
  volatile uint64_t *sd_base = ace->baseaddr;
  return sd_base[reg];
}

static inline void pitonsd_out(struct pitonsd_device *ace, int reg, u64 val)
{
  volatile uint64_t *sd_base = ace->baseaddr;
  sd_base[reg] = val;
  return;
}

/* ---------------------------------------------------------------------
 * Debug support functions
 */

#if defined(DEBUG)
static void pitonsd_dump_mem(void *base, int len)
{
	const char *ptr = base;
	int i, j;

	for (i = 0; i < len; i += 16) {
		printk(KERN_INFO "%.8x:", i);
		for (j = 0; j < 16; j++) {
			if (!(j % 4))
				printk(" ");
			printk("%.2x", ptr[i + j]);
		}
		printk(" ");
		for (j = 0; j < 16; j++)
			printk("%c", isprint(ptr[i + j]) ? ptr[i + j] : '.');
		printk("\n");
	}
}
#else
static inline void pitonsd_dump_mem(void *base, int len)
{
}
#endif

static void pitonsd_dump_regs(struct pitonsd_device *ace)
{
	dev_info(ace->dev,
		 "    ctrl:  %.8x  seccnt/cmd: %.4x      ver:%.4x\n"
		 "    status:%.8x  mpu_lba:%.8x  busmode:%4x\n"
		 "    error: %.8x  cfg_lba:%.8x  fatstat:%.4x\n",
		 pitonsd_in(ace, _piton_sd_CTRL),
		 pitonsd_in(ace, _piton_sd_SECCNTCMD),
		 pitonsd_in(ace, _piton_sd_VERSION),
		 pitonsd_in(ace, _piton_sd_STATUS),
		 pitonsd_in(ace, _piton_sd_MPULBA),
		 pitonsd_in(ace, _piton_sd_BUSMODE),
		 pitonsd_in(ace, _piton_sd_ERROR),
		 pitonsd_in(ace, _piton_sd_CFGLBA), pitonsd_in(ace, _piton_sd_FATSTAT));
}

static void pitonsd_fix_driveid(u16 *id)
{
#if defined(__BIG_ENDIAN)
	int i;

	/* All half words have wrong byte order; swap the bytes */
	for (i = 0; i < ATA_ID_WORDS; i++, id++)
		*id = le16_to_cpu(*id);
#endif
}

/* ---------------------------------------------------------------------
 * Finite State Machine (FSM) implementation
 */

/* FSM tasks; used to direct state transitions */
#define _piton_sd_TASK_IDLE      0
#define _piton_sd_TASK_IDENTIFY  1
#define _piton_sd_TASK_READ      2
#define _piton_sd_TASK_WRITE     3
#define _piton_sd_FSM_NUM_TASKS  4

/* FSM state definitions */
#define _piton_sd_FSM_STATE_IDLE               0
#define _piton_sd_FSM_STATE_REQ_LOCK           1
#define _piton_sd_FSM_STATE_WAIT_LOCK          2
#define _piton_sd_FSM_STATE_WAIT_CFREADY       3
#define _piton_sd_FSM_STATE_IDENTIFY_PREPARE   4
#define _piton_sd_FSM_STATE_IDENTIFY_TRANSFER  5
#define _piton_sd_FSM_STATE_IDENTIFY_COMPLETE  6
#define _piton_sd_FSM_STATE_REQ_PREPARE        7
#define _piton_sd_FSM_STATE_REQ_TRANSFER       8
#define _piton_sd_FSM_STATE_REQ_COMPLETE       9
#define _piton_sd_FSM_STATE_ERROR             10
#define _piton_sd_FSM_NUM_STATES              11

/* Set flag to exit FSM loop and reschedule tasklet */
static inline void pitonsd_fsm_yield(struct pitonsd_device *ace)
{
	pr_debug("pitonsd_fsm_yield()\n");
	tasklet_schedule(&ace->fsm_tasklet);
	ace->fsm_continue_flag = 0;
}

/* Set flag to exit FSM loop and wait for IRQ to reschedule tasklet */
static inline void pitonsd_fsm_yieldirq(struct pitonsd_device *ace)
{
	printk("pitonsd_fsm_yieldirq()\n");

	if (!ace->irq)
		/* No IRQ assigned, so need to poll */
		tasklet_schedule(&ace->fsm_tasklet);
	ace->fsm_continue_flag = 0;
}

/* Get the next read/write request; ending requests that we don't handle */
static struct request *pitonsd_get_next_request(struct request_queue *q)
{
	struct request *req;

	while ((req = blk_peek_request(q)) != NULL) {
		if (!blk_rq_is_passthrough(req))
			break;
		blk_start_request(req);
		__blk_end_request_all(req, BLK_STS_IOERR);
	}
	return req;
}

static void pitonsd_fsm_dostate(struct pitonsd_device *ace)
{
	struct request *req;
	u32 status;
	u16 val;
	int count;

#if defined(DEBUG)
	printk("fsm_state=%i, id_req_count=%i\n",
		ace->fsm_state, ace->id_req_count);
#endif

	/* Verify that there is actually a CF in the slot. If not, then
	 * bail out back to the idle state and wake up all the waiters */
	status = pitonsd_in(ace, _piton_sd_STATUS);
	if ((status & _piton_sd_STATUS_CFDETECT) == 0) {
		ace->fsm_state = _piton_sd_FSM_STATE_IDLE;
		ace->media_change = 1;
		set_capacity(ace->gd, 0);
		dev_info(ace->dev, "No CF in slot\n");

		/* Drop all in-flight and pending requests */
		if (ace->req) {
			__blk_end_request_all(ace->req, BLK_STS_IOERR);
			ace->req = NULL;
		}
		while ((req = blk_fetch_request(ace->queue)) != NULL)
			__blk_end_request_all(req, BLK_STS_IOERR);

		/* Drop back to IDLE state and notify waiters */
		ace->fsm_state = _piton_sd_FSM_STATE_IDLE;
		ace->id_result = -EIO;
		while (ace->id_req_count) {
			complete(&ace->id_completion);
			ace->id_req_count--;
		}
	}

	switch (ace->fsm_state) {
	case _piton_sd_FSM_STATE_IDLE:
		/* See if there is anything to do */
		if (ace->id_req_count || pitonsd_get_next_request(ace->queue)) {
			ace->fsm_iter_num++;
			ace->fsm_state = _piton_sd_FSM_STATE_REQ_LOCK;
			mod_timer(&ace->stall_timer, jiffies + HZ);
			if (!timer_pending(&ace->stall_timer))
				add_timer(&ace->stall_timer);
			break;
		}
		del_timer(&ace->stall_timer);
		ace->fsm_continue_flag = 0;
		break;

	case _piton_sd_FSM_STATE_REQ_LOCK:
		if (pitonsd_in(ace, _piton_sd_STATUS) & _piton_sd_STATUS_MPULOCK) {
			/* Already have the lock, jump to next state */
			ace->fsm_state = _piton_sd_FSM_STATE_WAIT_CFREADY;
			break;
		}

		/* Request the lock */
		val = pitonsd_in(ace, _piton_sd_CTRL);
		pitonsd_out(ace, _piton_sd_CTRL, val | _piton_sd_CTRL_LOCKREQ);
		ace->fsm_state = _piton_sd_FSM_STATE_WAIT_LOCK;
		break;

	case _piton_sd_FSM_STATE_WAIT_LOCK:
		if (pitonsd_in(ace, _piton_sd_STATUS) & _piton_sd_STATUS_MPULOCK) {
			/* got the lock; move to next state */
			ace->fsm_state = _piton_sd_FSM_STATE_WAIT_CFREADY;
			break;
		}

		/* wait a bit for the lock */
		pitonsd_fsm_yield(ace);
		break;

	case _piton_sd_FSM_STATE_WAIT_CFREADY:
		status = pitonsd_in(ace, _piton_sd_STATUS);
		if (!(status & _piton_sd_STATUS_RDYFORCFCMD) ||
		    (status & _piton_sd_STATUS_CFBSY)) {
			/* CF card isn't ready; it needs to be polled */
			pitonsd_fsm_yield(ace);
			break;
		}

		/* Device is ready for command; determine what to do next */
		if (ace->id_req_count)
			ace->fsm_state = _piton_sd_FSM_STATE_IDENTIFY_PREPARE;
		else
			ace->fsm_state = _piton_sd_FSM_STATE_REQ_PREPARE;
		break;

	case _piton_sd_FSM_STATE_IDENTIFY_PREPARE:
		/* Send identify command */
		ace->fsm_task = _piton_sd_TASK_IDENTIFY;
		ace->data_ptr = ace->cf_id;
		ace->data_count = _piton_sd_BUF_PER_SECTOR;
		pitonsd_out(ace, _piton_sd_SECCNTCMD, _piton_sd_SECCNTCMD_IDENTIFY);

		/* As per datasheet, put config controller in reset */
		val = pitonsd_in(ace, _piton_sd_CTRL);
		pitonsd_out(ace, _piton_sd_CTRL, val | _piton_sd_CTRL_CFGRESET);

		/* irq handler takes over from this point; wait for the
		 * transfer to complete */
		ace->fsm_state = _piton_sd_FSM_STATE_IDENTIFY_TRANSFER;
		pitonsd_fsm_yieldirq(ace);
		break;

	case _piton_sd_FSM_STATE_IDENTIFY_TRANSFER:
		/* Check that the sysace is ready to receive data */
		status = pitonsd_in(ace, _piton_sd_STATUS);
		if (status & _piton_sd_STATUS_CFBSY) {
			printk("CFBSY set; t=%i iter=%i dc=%i\n",
				ace->fsm_task, ace->fsm_iter_num,
				ace->data_count);
			pitonsd_fsm_yield(ace);
			break;
		}
		if (!(status & _piton_sd_STATUS_DATABUFRDY)) {
			pitonsd_fsm_yield(ace);
			break;
		}

		/* Transfer the next buffer */
	        pitonsd_in(ace, ace->data_count--);

		/* If there are still buffers to be transfers; jump out here */
		if (ace->data_count != 0) {
			pitonsd_fsm_yieldirq(ace);
			break;
		}

		/* transfer finished; kick state machine */
		printk("identify finished\n");
		ace->fsm_state = _piton_sd_FSM_STATE_IDENTIFY_COMPLETE;
		break;

	case _piton_sd_FSM_STATE_IDENTIFY_COMPLETE:
		pitonsd_fix_driveid(ace->cf_id);
		pitonsd_dump_mem(ace->cf_id, 512);	/* Debug: Dump out disk ID */

		if (ace->data_result) {
			/* Error occurred, disable the disk */
			ace->media_change = 1;
			set_capacity(ace->gd, 0);
			dev_err(ace->dev, "error fetching CF id (%i)\n",
				ace->data_result);
		} else {
			ace->media_change = 0;

			/* Record disk parameters */
			set_capacity(ace->gd,
				ata_id_u32(ace->cf_id, ATA_ID_LBA_CAPACITY));
			dev_info(ace->dev, "capacity: %i sectors\n",
				ata_id_u32(ace->cf_id, ATA_ID_LBA_CAPACITY));
		}

		/* We're done, drop to IDLE state and notify waiters */
		ace->fsm_state = _piton_sd_FSM_STATE_IDLE;
		ace->id_result = ace->data_result;
		while (ace->id_req_count) {
			complete(&ace->id_completion);
			ace->id_req_count--;
		}
		break;

	case _piton_sd_FSM_STATE_REQ_PREPARE:
		req = pitonsd_get_next_request(ace->queue);
		if (!req) {
			ace->fsm_state = _piton_sd_FSM_STATE_IDLE;
			break;
		}
		blk_start_request(req);

		/* Okay, it's a data request, set it up for transfer */
		dev_dbg(ace->dev,
			"request: sec=%llx hcnt=%x, ccnt=%x, dir=%i\n",
			(unsigned long long)blk_rq_pos(req),
			blk_rq_sectors(req), blk_rq_cur_sectors(req),
			rq_data_dir(req));

		ace->req = req;
		ace->data_ptr = bio_data(req->bio);
		ace->data_count = blk_rq_cur_sectors(req) * _piton_sd_BUF_PER_SECTOR;
		pitonsd_out(ace, _piton_sd_MPULBA, blk_rq_pos(req) & 0x0FFFFFFF);

		count = blk_rq_sectors(req);
		if (rq_data_dir(req)) {
			/* Kick off write request */
			printk("write data\n");
			ace->fsm_task = _piton_sd_TASK_WRITE;
			pitonsd_out(ace, _piton_sd_SECCNTCMD,
				count | _piton_sd_SECCNTCMD_WRITE_DATA);
		} else {
			/* Kick off read request */
			printk("read data\n");
			ace->fsm_task = _piton_sd_TASK_READ;
			pitonsd_out(ace, _piton_sd_SECCNTCMD,
				count | _piton_sd_SECCNTCMD_READ_DATA);
		}

		/* As per datasheet, put config controller in reset */
		val = pitonsd_in(ace, _piton_sd_CTRL);
		pitonsd_out(ace, _piton_sd_CTRL, val | _piton_sd_CTRL_CFGRESET);

		/* Move to the transfer state.  The systemace will raise
		 * an interrupt once there is something to do
		 */
		ace->fsm_state = _piton_sd_FSM_STATE_REQ_TRANSFER;
		if (ace->fsm_task == _piton_sd_TASK_READ)
			pitonsd_fsm_yieldirq(ace);	/* wait for data ready */
		break;

	case _piton_sd_FSM_STATE_REQ_TRANSFER:
		/* Check that the sysace is ready to receive data */
		status = pitonsd_in(ace, _piton_sd_STATUS);
		if (status & _piton_sd_STATUS_CFBSY) {
			dev_dbg(ace->dev,
				"CFBSY set; t=%i iter=%i c=%i dc=%i irq=%i\n",
				ace->fsm_task, ace->fsm_iter_num,
				blk_rq_cur_sectors(ace->req) * 16,
				ace->data_count, ace->in_irq);
			pitonsd_fsm_yield(ace);	/* need to poll CFBSY bit */
			break;
		}
		if (!(status & _piton_sd_STATUS_DATABUFRDY)) {
			dev_dbg(ace->dev,
				"DATABUF not set; t=%i iter=%i c=%i dc=%i irq=%i\n",
				ace->fsm_task, ace->fsm_iter_num,
				blk_rq_cur_sectors(ace->req) * 16,
				ace->data_count, ace->in_irq);
			pitonsd_fsm_yieldirq(ace);
			break;
		}

		/* Transfer the next buffer */
		if (ace->fsm_task == _piton_sd_TASK_WRITE)
                  pitonsd_out(ace, ace->data_count--, 0);
		else
                  pitonsd_in(ace, ace->data_count--);

		/* If there are still buffers to be transfers; jump out here */
		if (ace->data_count != 0) {
			pitonsd_fsm_yieldirq(ace);
			break;
		}

		/* bio finished; is there another one? */
		if (__blk_end_request_cur(ace->req, BLK_STS_OK)) {
			/* printk("next block; h=%u c=%u\n",
			 *      blk_rq_sectors(ace->req),
			 *      blk_rq_cur_sectors(ace->req));
			 */
			ace->data_ptr = bio_data(ace->req->bio);
			ace->data_count = blk_rq_cur_sectors(ace->req) * 16;
			pitonsd_fsm_yieldirq(ace);
			break;
		}

		ace->fsm_state = _piton_sd_FSM_STATE_REQ_COMPLETE;
		break;

	case _piton_sd_FSM_STATE_REQ_COMPLETE:
		ace->req = NULL;

		/* Finished request; go to idle state */
		ace->fsm_state = _piton_sd_FSM_STATE_IDLE;
		break;

	default:
		ace->fsm_state = _piton_sd_FSM_STATE_IDLE;
		break;
	}
}

static void pitonsd_fsm_tasklet(unsigned long data)
{
	struct pitonsd_device *ace = (void *)data;
	unsigned long flags;

	spin_lock_irqsave(&ace->lock, flags);

	/* Loop over state machine until told to stop */
	ace->fsm_continue_flag = 1;
	while (ace->fsm_continue_flag)
		pitonsd_fsm_dostate(ace);

	spin_unlock_irqrestore(&ace->lock, flags);
}

static void pitonsd_stall_timer(struct timer_list *t)
{
	struct pitonsd_device *ace = from_timer(ace, t, stall_timer);
	unsigned long flags;

	dev_warn(ace->dev,
		 "kicking stalled fsm; state=%i task=%i iter=%i dc=%i\n",
		 ace->fsm_state, ace->fsm_task, ace->fsm_iter_num,
		 ace->data_count);
	spin_lock_irqsave(&ace->lock, flags);

	/* Rearm the stall timer *before* entering FSM (which may then
	 * delete the timer) */
	mod_timer(&ace->stall_timer, jiffies + HZ);

	/* Loop over state machine until told to stop */
	ace->fsm_continue_flag = 1;
	while (ace->fsm_continue_flag)
		pitonsd_fsm_dostate(ace);

	spin_unlock_irqrestore(&ace->lock, flags);
}

/* ---------------------------------------------------------------------
 * Interrupt handling routines
 */
static int pitonsd_interrupt_checkstate(struct pitonsd_device *ace)
{
	u32 sreg = pitonsd_in(ace, _piton_sd_STATUS);
	u16 creg = pitonsd_in(ace, _piton_sd_CTRL);

	/* Check for error occurrence */
	if ((sreg & (_piton_sd_STATUS_CFGERROR | _piton_sd_STATUS_CFCERROR)) &&
	    (creg & _piton_sd_CTRL_ERRORIRQ)) {
		dev_err(ace->dev, "transfer failure\n");
		pitonsd_dump_regs(ace);
		return -EIO;
	}

	return 0;
}

static irqreturn_t pitonsd_interrupt(int irq, void *dev_id)
{
	u16 creg;
	struct pitonsd_device *ace = dev_id;

	/* be safe and get the lock */
	spin_lock(&ace->lock);
	ace->in_irq = 1;

	/* clear the interrupt */
	creg = pitonsd_in(ace, _piton_sd_CTRL);
	pitonsd_out(ace, _piton_sd_CTRL, creg | _piton_sd_CTRL_RESETIRQ);
	pitonsd_out(ace, _piton_sd_CTRL, creg);

	/* check for IO failures */
	if (pitonsd_interrupt_checkstate(ace))
		ace->data_result = -EIO;

	if (ace->fsm_task == 0) {
		dev_err(ace->dev,
			"spurious irq; stat=%.8x ctrl=%.8x cmd=%.4x\n",
			pitonsd_in(ace, _piton_sd_STATUS), pitonsd_in(ace, _piton_sd_CTRL),
			pitonsd_in(ace, _piton_sd_SECCNTCMD));
		dev_err(ace->dev, "fsm_task=%i fsm_state=%i data_count=%i\n",
			ace->fsm_task, ace->fsm_state, ace->data_count);
	}

	/* Loop over state machine until told to stop */
	ace->fsm_continue_flag = 1;
	while (ace->fsm_continue_flag)
		pitonsd_fsm_dostate(ace);

	/* done with interrupt; drop the lock */
	ace->in_irq = 0;
	spin_unlock(&ace->lock);

	return IRQ_HANDLED;
}

/* ---------------------------------------------------------------------
 * Block ops
 */
static void pitonsd_request(struct request_queue * q)
{
	struct request *req;
	struct pitonsd_device *ace;

	req = pitonsd_get_next_request(q);

	if (req) {
		ace = req->rq_disk->private_data;
		tasklet_schedule(&ace->fsm_tasklet);
	}
}

static unsigned int pitonsd_check_events(struct gendisk *gd, unsigned int clearing)
{
	struct pitonsd_device *ace = gd->private_data;
	printk("pitonsd_check_events(): %i\n", ace->media_change);

	return ace->media_change ? DISK_EVENT_MEDIA_CHANGE : 0;
}

static int pitonsd_revalidate_disk(struct gendisk *gd)
{
	struct pitonsd_device *ace = gd->private_data;
	unsigned long flags;

	printk("pitonsd_revalidate_disk()\n");

	if (ace->media_change) {
		printk("requesting cf id and scheduling tasklet\n");

		spin_lock_irqsave(&ace->lock, flags);
		ace->id_req_count++;
		spin_unlock_irqrestore(&ace->lock, flags);

		tasklet_schedule(&ace->fsm_tasklet);
		wait_for_completion(&ace->id_completion);
	}

	printk("revalidate complete\n");
	return ace->id_result;
}

static int pitonsd_open(struct block_device *bdev, fmode_t mode)
{
	struct pitonsd_device *ace = bdev->bd_disk->private_data;
	unsigned long flags;

	printk("pitonsd_open() users=%i\n", ace->users + 1);

	mutex_lock(&pitonsd_mutex);
	spin_lock_irqsave(&ace->lock, flags);
	ace->users++;
	spin_unlock_irqrestore(&ace->lock, flags);

	check_disk_change(bdev);
	mutex_unlock(&pitonsd_mutex);

	return 0;
}

static void pitonsd_release(struct gendisk *disk, fmode_t mode)
{
	struct pitonsd_device *ace = disk->private_data;
	unsigned long flags;
	u16 val;

	printk("pitonsd_release() users=%i\n", ace->users - 1);

	mutex_lock(&pitonsd_mutex);
	spin_lock_irqsave(&ace->lock, flags);
	ace->users--;
	if (ace->users == 0) {
		val = pitonsd_in(ace, _piton_sd_CTRL);
		pitonsd_out(ace, _piton_sd_CTRL, val & ~_piton_sd_CTRL_LOCKREQ);
	}
	spin_unlock_irqrestore(&ace->lock, flags);
	mutex_unlock(&pitonsd_mutex);
}

static int pitonsd_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	struct pitonsd_device *ace = bdev->bd_disk->private_data;
	u16 *cf_id = ace->cf_id;

	printk("pitonsd_getgeo()\n");

	geo->heads	= cf_id[ATA_ID_HEADS];
	geo->sectors	= cf_id[ATA_ID_SECTORS];
	geo->cylinders	= cf_id[ATA_ID_CYLS];

	return 0;
}

static const struct block_device_operations pitonsd_fops = {
	.owner = THIS_MODULE,
	.open = pitonsd_open,
	.release = pitonsd_release,
	.check_events = pitonsd_check_events,
	.revalidate_disk = pitonsd_revalidate_disk,
	.getgeo = pitonsd_getgeo,
};

/* --------------------------------------------------------------------
 * LowRISC_piton_sd device setup/teardown code
 */
static int pitonsd_setup(struct pitonsd_device *ace)
{
	u16 version;
	u16 val;
	int rc;

	printk("pitonsd_setup(ace=0x%p)\n", ace);
	printk("physaddr=0x%llx irq=%i\n",
		(unsigned long long)ace->physaddr, ace->irq);

	spin_lock_init(&ace->lock);
	init_completion(&ace->id_completion);

	/*
	 * Map the device
	 */
	ace->baseaddr = ioremap(ace->physaddr, 0x80);
	if (!ace->baseaddr)
		goto err_ioremap;

	/*
	 * Initialize the state machine tasklet and stall timer
	 */
	tasklet_init(&ace->fsm_tasklet, pitonsd_fsm_tasklet, (unsigned long)ace);
	timer_setup(&ace->stall_timer, pitonsd_stall_timer, 0);

	/*
	 * Initialize the request queue
	 */
	ace->queue = blk_init_queue(pitonsd_request, &ace->lock);
	if (ace->queue == NULL)
		goto err_blk_initq;
	blk_queue_logical_block_size(ace->queue, 512);
	blk_queue_bounce_limit(ace->queue, BLK_BOUNCE_HIGH);

	/*
	 * Allocate and initialize GD structure
	 */
	ace->gd = alloc_disk(_piton_sd_NUM_MINORS);
	if (!ace->gd)
		goto err_alloc_disk;

	ace->gd->major = pitonsd_major;
	ace->gd->first_minor = ace->id * _piton_sd_NUM_MINORS;
	ace->gd->fops = &pitonsd_fops;
	ace->gd->queue = ace->queue;
	ace->gd->private_data = ace;
	snprintf(ace->gd->disk_name, 32, "xs%c", ace->id + 'a');

	/* Make sure version register is sane */
	version = pitonsd_in(ace, _piton_sd_VERSION);
	if ((version == 0) || (version == 0xFFFF))
		goto err_read;

	/* Put sysace in a sane state by clearing most control reg bits */
	pitonsd_out(ace, _piton_sd_CTRL, _piton_sd_CTRL_FORCECFGMODE |
		_piton_sd_CTRL_DATABUFRDYIRQ | _piton_sd_CTRL_ERRORIRQ);

	/* Now we can hook up the irq handler */
	if (ace->irq) {
		rc = request_irq(ace->irq, pitonsd_interrupt, 0, "systemace", ace);
		if (rc) {
			/* Failure - fall back to polled mode */
			dev_err(ace->dev, "request_irq failed\n");
			ace->irq = 0;
		}
	}

	/* Enable interrupts */
	val = pitonsd_in(ace, _piton_sd_CTRL);
	val |= _piton_sd_CTRL_DATABUFRDYIRQ | _piton_sd_CTRL_ERRORIRQ;
	pitonsd_out(ace, _piton_sd_CTRL, val);

	/* Print the identification */
	dev_info(ace->dev, "LowRISC_piton_sd revision %i.%i.%i\n",
		 (version >> 12) & 0xf, (version >> 8) & 0x0f, version & 0xff);
	printk("physaddr 0x%llx, mapped to 0x%p, irq=%i\n",
		(unsigned long long) ace->physaddr, ace->baseaddr, ace->irq);

	ace->media_change = 1;
	pitonsd_revalidate_disk(ace->gd);

	/* Make the sysace device 'live' */
	add_disk(ace->gd);

	return 0;

err_read:
	put_disk(ace->gd);
err_alloc_disk:
	blk_cleanup_queue(ace->queue);
err_blk_initq:
	iounmap(ace->baseaddr);
err_ioremap:
	dev_info(ace->dev, "pitonsd: error initializing device at 0x%llx\n",
		 (unsigned long long) ace->physaddr);
	return -ENOMEM;
}

static void pitonsd_teardown(struct pitonsd_device *ace)
{
	if (ace->gd) {
		del_gendisk(ace->gd);
		put_disk(ace->gd);
	}

	if (ace->queue)
		blk_cleanup_queue(ace->queue);

	tasklet_kill(&ace->fsm_tasklet);

	if (ace->irq)
		free_irq(ace->irq, ace);

	iounmap(ace->baseaddr);
}

static int pitonsd_alloc(struct device *dev, int id, resource_size_t physaddr, int irq)
{
	struct pitonsd_device *ace;
	int rc;
	dev_dbg(dev, "pitonsd_alloc(%p)\n", dev);

	if (!physaddr) {
		rc = -ENODEV;
		goto err_noreg;
	}

	/* Allocate and initialize the ace device structure */
	ace = kzalloc(sizeof(struct pitonsd_device), GFP_KERNEL);
	if (!ace) {
		rc = -ENOMEM;
		goto err_alloc;
	}

	ace->dev = dev;
	ace->id = id;
	ace->physaddr = physaddr;
	ace->irq = irq;

	/* Call the setup code */
	rc = pitonsd_setup(ace);
	if (rc)
		goto err_setup;

	dev_set_drvdata(dev, ace);
	return 0;

err_setup:
	dev_set_drvdata(dev, NULL);
	kfree(ace);
err_alloc:
err_noreg:
	dev_err(dev, "could not initialize device, err=%i\n", rc);
	return rc;
}

static void pitonsd_free(struct device *dev)
{
	struct pitonsd_device *ace = dev_get_drvdata(dev);
	dev_dbg(dev, "pitonsd_free(%p)\n", dev);

	if (ace) {
		pitonsd_teardown(ace);
		dev_set_drvdata(dev, NULL);
		kfree(ace);
	}
}

/* ---------------------------------------------------------------------
 * Platform Bus Support
 */

static int pitonsd_probe(struct platform_device *dev)
{
	resource_size_t physaddr = 0;
	u32 id = dev->id;
	int irq = 0;
	int i;

	printk("pitonsd_probe(%p)\n", dev);

	/* device id and bus width */
	if (of_property_read_u32(dev->dev.of_node, "port-number", &id))
		id = 0;

	for (i = 0; i < dev->num_resources; i++) {
		if (dev->resource[i].flags & IORESOURCE_MEM)
			physaddr = dev->resource[i].start;
		if (dev->resource[i].flags & IORESOURCE_IRQ)
			irq = dev->resource[i].start;
	}

	/* Call the bus-independent setup code */
	return pitonsd_alloc(&dev->dev, id, physaddr, irq);
}

/*
 * Platform bus remove() method
 */
static int pitonsd_remove(struct platform_device *dev)
{
	pitonsd_free(&dev->dev);
	return 0;
}

#if defined(CONFIG_OF)
/* Match table for of_platform binding */
static const struct of_device_id pitonsd_of_match[] = {
	{ .compatible = "pitonsd" },
	{},
};
MODULE_DEVICE_TABLE(of, pitonsd_of_match);
#else /* CONFIG_OF */
#define pitonsd_of_match NULL
#endif /* CONFIG_OF */

static struct platform_driver pitonsd_platform_driver = {
	.probe = pitonsd_probe,
	.remove = pitonsd_remove,
	.driver = {
		.name = "pitonsd",
		.of_match_table = pitonsd_of_match,
	},
};

/* ---------------------------------------------------------------------
 * Module init/exit routines
 */
static int __init pitonsd_init(void)
{
	int rc;

	pitonsd_major = register_blkdev(pitonsd_major, "pitonsd");
	if (pitonsd_major <= 0) {
		rc = -ENOMEM;
		goto err_blk;
	}

	rc = platform_driver_register(&pitonsd_platform_driver);
	if (rc)
		goto err_plat;

	pr_info("LowRISC_piton_sd device driver, major=%i\n", pitonsd_major);
	return 0;

err_plat:
	unregister_blkdev(pitonsd_major, "pitonsd");
err_blk:
	printk(KERN_ERR "pitonsd: registration failed; err=%i\n", rc);
	return rc;
}
module_init(pitonsd_init);

static void __exit pitonsd_exit(void)
{
	pr_debug("Unregistering LowRISC_piton_sd driver\n");
	platform_driver_unregister(&pitonsd_platform_driver);
	unregister_blkdev(pitonsd_major, "pitonsd");
}
module_exit(pitonsd_exit);
