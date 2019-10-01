/*
 * LowRISC_piton_sd device driver
 *
 * SD-Card block driver based on modified open piton FPGA driver
 * This driver based on xsysace.c (Copyright 2007 Secret Lab Technologies Ltd.)
 * Original copyright Grant Likely <grant.likely@secretlab.ca>
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
 *    1. obtain the sdpiton->lock spinlock.
 *    2. loop on pitonsd_fsm_dostate() until the sdpiton->fsm_continue flag is
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
 *    #define SELF_TEST to compare cached results from first 32MB in polling
 *    mode with interrupt driven results of normal operation (low confidence checkum)
 */

#undef DEBUG
//#define DEBUG
#undef SELF_TEST
//#define SELF_TEST

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

#include "../../block/partitions/check.h"

MODULE_AUTHOR("Jonathan Kimmitt <jrrk2@cam.ac.uk>");
MODULE_DESCRIPTION("LowRISC_piton_sd device driver");
MODULE_LICENSE("GPL");

/* LowRISC_piton_sd register definitions */
#define _piton_sd_ADDR_SD     (0x00)
#define _piton_sd_ADDR_DMA    (0x01)
#define _piton_sd_BLKCNT      (0x02)
#define _piton_sd_CTRL        (0x3)
#define _piton_sd_LED         (0xF)

#define _piton_sd_CTRL_REQ_VALID          (0x00000001)
#define _piton_sd_CTRL_REQ_WRITE          (0x00000002)
#define _piton_sd_CTRL_IRQ_EN             (0x00000004)
#define _piton_sd_CTRL_SD_RST_EN          (0x00000008)

#define _piton_sd_ADDR_SD_F   (0x00)
#define _piton_sd_ADDR_DMA_F  (0x01)
#define _piton_sd_STATUS      (0x02)
#define _piton_sd_ERROR       (0x03)
#define _piton_sd_INIT_STATE  (0x04)
#define _piton_sd_COUNTER     (0x05)
#define _piton_sd_INIT_FSM    (0x06)
#define _piton_sd_TRAN_STATE  (0x07)
#define _piton_sd_TRAN_FSM    (0x08)

#define _piton_sd_VERSION     (0x09)

#define _piton_sd_STATUS_REQ          (0x00000001)
#define _piton_sd_STATUS_WR           (0x00000002)
#define _piton_sd_STATUS_IRQ_EN       (0x00000004)
#define _piton_sd_STATUS_IRQ          (0x00000008)
#define _piton_sd_STATUS_RDYFORCFCMD  (0x00000010)
#define _piton_sd_STATUS_CFGDONE      (0x00000020)
#define _piton_sd_STATUS_SDHC         (0x00000040)
#define _piton_sd_STATUS_CFDETECT     (0x00000080)

#define _piton_sd_VERSION_REVISION_MASK (0x00FF)
#define _piton_sd_VERSION_MINOR_MASK    (0x0F00)
#define _piton_sd_VERSION_MAJOR_MASK    (0xF000)

#define _piton_sd_NUM_MINORS 16

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
        uint data_pos;          /* data (sector) position */
	int data_result;	/* Result of transfer; 0 := success */

	int in_irq;

	/* Details of hardware device */
        resource_size_t physaddr, physaddrend;
        volatile u64 __iomem *baseaddr, *ioptr;
	int irq;

	/* Block device data structures */
	spinlock_t lock;
	struct device *dev;
	struct request_queue *queue;
	struct gendisk *gd;

        struct block_device *bdev;
};

#define SELF_TEST_LIMIT 65536

static DEFINE_MUTEX(pitonsd_mutex);
static int pitonsd_major;

/* ---------------------------------------------------------------------
 * Debug support functions
 */

#ifdef SELF_TEST
static u_int csum1(u_char *p, int nr)
{
	u_int lcrc = 0;

	/*
	 * 16-bit checksum, rotating right before each addition;
	 * overflow is discarded.
	 */
        while (nr--)
          {
            if (lcrc & 1)
              lcrc |= 0x10000;
            lcrc = ((lcrc >> 1) + *p++) & 0xffff;
          }

	return lcrc;
}

static void csum2(struct pitonsd_device *sdpiton, void *vp, int pos, int count)
{
  u_char *p = vp;
  while (count-- && pos < SELF_TEST_LIMIT)
    {
      u_int sum = csum1(p, 512);
      if (sum != sdpiton->expected[pos])
        printk("csum(%d,512) = %u (expected %u)\n", pos, sum, sdpiton->expected[pos]);
      ++pos;
      p += 512;
    }
}

static void csum3(struct pitonsd_device *sdpiton, void *vp, int pos, int count)
{
  u_char *p = vp;
  while (count-- && pos < SELF_TEST_LIMIT)
    {
      u_int sum = csum1(p, 512);
      sdpiton->expected[pos++] = sum;
      p += 512;
    }
}

#endif

static void pitonsd_dump_regs(struct pitonsd_device *sdpiton)
{
	printk(
		 "    sd_f:  0x%llx  dma_f: 0x%llx   status: 0x%llx\n"
		 "    resp_vec: 0x%llx  init_state: 0x%llx  counter: 0x%llx\n"
		 "    init_fsm: 0x%llx  tran_state: 0x%llx  tran_fsm: 0x%llx\n",
		 sdpiton->baseaddr[0],
		 sdpiton->baseaddr[1],
		 sdpiton->baseaddr[2],
		 sdpiton->baseaddr[3],
		 sdpiton->baseaddr[4],
		 sdpiton->baseaddr[5],
		 sdpiton->baseaddr[6],
		 sdpiton->baseaddr[7],
                 sdpiton->baseaddr[8]);
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
#define _piton_sd_FSM_STATE_REQ_PREPARE        6
#define _piton_sd_FSM_STATE_REQ_NEXT           7
#define _piton_sd_FSM_STATE_REQ_TRANSFER       8
#define _piton_sd_FSM_STATE_REQ_COMPLETE       9
#define _piton_sd_FSM_STATE_ERROR             10
#define _piton_sd_FSM_NUM_STATES              11

/* Set flag to exit FSM loop and reschedule tasklet */
static inline void pitonsd_fsm_yield(struct pitonsd_device *sdpiton)
{
	pr_debug("pitonsd_fsm_yield()\n");
	tasklet_schedule(&sdpiton->fsm_tasklet);
	sdpiton->fsm_continue_flag = 0;
}

/* Set flag to exit FSM loop and wait for IRQ to reschedule tasklet */
static inline void pitonsd_fsm_yieldirq(struct pitonsd_device *sdpiton)
{
	pr_debug("pitonsd_fsm_yieldirq()\n");

	if (!sdpiton->irq)
		/* No IRQ assigned, so need to poll */
		tasklet_schedule(&sdpiton->fsm_tasklet);
	sdpiton->fsm_continue_flag = 0;
}

/* Get the next read/write request; ending requests that we don't handle */
static struct request *pitonsd_get_next_request(struct request_queue *q)
{
	struct request *req;

        pr_debug("pitonsd_get_next_request()\n");
        
	while ((req = blk_peek_request(q)) != NULL) {
		if (!blk_rq_is_passthrough(req))
			break;
		blk_start_request(req);
		__blk_end_request_all(req, BLK_STS_IOERR);
	}
	return req;
}

static void pitonsd_fsm_dostate(struct pitonsd_device *sdpiton)
{
	u32 status;

	/* Verify that there is actually a SD-Card in the slot. If not, then
	 * bail out back to the idle state and wake up all the waiters */
	status = sdpiton->baseaddr[_piton_sd_STATUS];
	if ((_piton_sd_STATUS_CFDETECT & ~status) == 0) {
		sdpiton->fsm_state = _piton_sd_FSM_STATE_IDLE;
		sdpiton->media_change = 1;
		set_capacity(sdpiton->gd, 0);
		dev_info(sdpiton->dev, "No SD-Card in slot\n");

		/* Drop all in-flight and pending requests */
		if (sdpiton->req) {
			__blk_end_request_all(sdpiton->req, BLK_STS_IOERR);
			sdpiton->req = NULL;
		}
		while ((sdpiton->req = blk_fetch_request(sdpiton->queue)) != NULL)
			__blk_end_request_all(sdpiton->req, BLK_STS_IOERR);

		/* Drop back to IDLE state and notify waiters */
		sdpiton->fsm_state = _piton_sd_FSM_STATE_IDLE;
	}

	switch (sdpiton->fsm_state) {
	case _piton_sd_FSM_STATE_IDLE:
		/* See if there is anything to do */
		if (pitonsd_get_next_request(sdpiton->queue)) {
			sdpiton->fsm_iter_num++;
			sdpiton->fsm_state = _piton_sd_FSM_STATE_REQ_LOCK;
			mod_timer(&sdpiton->stall_timer, jiffies + HZ);
			if (!timer_pending(&sdpiton->stall_timer))
				add_timer(&sdpiton->stall_timer);
			break;
		}
		del_timer(&sdpiton->stall_timer);
		sdpiton->fsm_continue_flag = 0;
		break;

	case _piton_sd_FSM_STATE_REQ_LOCK:
			sdpiton->fsm_state = _piton_sd_FSM_STATE_WAIT_CFREADY;
		break;

	case _piton_sd_FSM_STATE_WAIT_CFREADY:
		status = sdpiton->baseaddr[_piton_sd_STATUS];
		if (!(status & _piton_sd_STATUS_RDYFORCFCMD)) {
			/* SD card isn't ready; it needs to be polled */
			pitonsd_fsm_yield(sdpiton);
			break;
		}

                sdpiton->fsm_state = _piton_sd_FSM_STATE_REQ_PREPARE;
		break;

	case _piton_sd_FSM_STATE_REQ_PREPARE:
		sdpiton->req = pitonsd_get_next_request(sdpiton->queue);
		if (!sdpiton->req) {
			sdpiton->fsm_state = _piton_sd_FSM_STATE_IDLE;
			break;
		}
		blk_start_request(sdpiton->req);

		/* Okay, it's a data request, set it up for transfer */
		dev_dbg(sdpiton->dev,
			"request: sec=0x%llx hcnt=0x%x, ccnt=0x%x, dir=0x%i\n",
			(unsigned long long)blk_rq_pos(sdpiton->req),
			blk_rq_sectors(sdpiton->req), blk_rq_cur_sectors(sdpiton->req),
			rq_data_dir(sdpiton->req));

		sdpiton->fsm_state = _piton_sd_FSM_STATE_REQ_NEXT;
		break;

        case _piton_sd_FSM_STATE_REQ_NEXT:
		sdpiton->data_ptr = bio_data(sdpiton->req->bio);
		sdpiton->data_count = blk_rq_cur_sectors(sdpiton->req);
                sdpiton->data_pos = blk_rq_pos(sdpiton->req);

                if (sdpiton->data_count > 8)
                  printk("Sector count = %d\n", sdpiton->data_count);
		if (rq_data_dir(sdpiton->req)) {
			/* Kick off write request */
			pr_debug("write data\n");
                        memcpy((void*)sdpiton->ioptr, sdpiton->data_ptr, sdpiton->data_count*512);
                        wmb();
			sdpiton->fsm_task = _piton_sd_TASK_WRITE;
                        /* SD sector address */
                        sdpiton->baseaddr[ _piton_sd_ADDR_SD ] = sdpiton->data_pos;
                        /* always start at beginning of DMA buffer */
                        sdpiton->baseaddr[ _piton_sd_ADDR_DMA ] = 0;
                        /* set sector count */
                        sdpiton->baseaddr[ _piton_sd_BLKCNT ] = sdpiton->data_count;
			sdpiton->baseaddr[ _piton_sd_CTRL ] = _piton_sd_CTRL_IRQ_EN|_piton_sd_CTRL_REQ_WRITE|_piton_sd_CTRL_REQ_VALID;
                        sdpiton->baseaddr[ _piton_sd_LED ] = sdpiton->data_pos;
                        wmb();
			sdpiton->baseaddr[ _piton_sd_CTRL ] = _piton_sd_CTRL_IRQ_EN|_piton_sd_CTRL_REQ_WRITE;
                        wmb();
		} else {
			/* Kick off read request */
			pr_debug("read data\n");
			sdpiton->fsm_task = _piton_sd_TASK_READ;
                        /* SD sector address */
                        sdpiton->baseaddr[ _piton_sd_ADDR_SD ] = sdpiton->data_pos;
                        /* always start at beginning of DMA buffer */
                        sdpiton->baseaddr[ _piton_sd_ADDR_DMA ] = 0;
                        /* set sector count */
                        sdpiton->baseaddr[ _piton_sd_BLKCNT ] = sdpiton->data_count;
			sdpiton->baseaddr[ _piton_sd_CTRL ] = _piton_sd_CTRL_REQ_VALID;
                        sdpiton->baseaddr[ _piton_sd_LED ] = sdpiton->data_pos;
                        wmb();
                        sdpiton->baseaddr[ _piton_sd_CTRL ] = _piton_sd_CTRL_IRQ_EN;
                        wmb();
		}

		/* Move to the transfer state.  The pitonsd will raise
		 * an interrupt once there is something to do
		 */
		sdpiton->fsm_state = _piton_sd_FSM_STATE_REQ_TRANSFER;
		if (sdpiton->fsm_task == _piton_sd_TASK_READ)
			pitonsd_fsm_yieldirq(sdpiton);	/* wait for data ready */
		break;

	case _piton_sd_FSM_STATE_REQ_TRANSFER:

                while (_piton_sd_STATUS_RDYFORCFCMD & ~(sdpiton->baseaddr[_piton_sd_STATUS]));

                /* Transfer the next buffer */
                if (sdpiton->fsm_task == _piton_sd_TASK_READ)
                  {
                    u64 dma_nxt = sdpiton->baseaddr[_piton_sd_ADDR_DMA_F];
#ifdef DEBUG
                    pitonsd_dump_regs(sdpiton);
#endif                    
                    pr_debug("Reading buffer from 0x%llx to 0x%llx (DMA=%llx)\n",
                           (u64) sdpiton->ioptr, (u64) sdpiton->data_ptr, (u64) dma_nxt);
                    memcpy(sdpiton->data_ptr, (void*)sdpiton->ioptr, sdpiton->data_count*512);
#ifdef SELF_TEST
                    csum2(sdpiton, sdpiton->data_ptr, sdpiton->data_pos, sdpiton->data_count);
#endif
                  }
                
                /* bio finished; is there another one? */
		if (__blk_end_request_cur(sdpiton->req, BLK_STS_OK)) {
			pr_debug("next block; h=%u c=%u\n",
			       blk_rq_sectors(sdpiton->req),
			       blk_rq_cur_sectors(sdpiton->req));

			sdpiton->fsm_state = _piton_sd_FSM_STATE_REQ_NEXT;
			break;
		}

		sdpiton->fsm_state = _piton_sd_FSM_STATE_REQ_COMPLETE;
		break;

	case _piton_sd_FSM_STATE_REQ_COMPLETE:
		sdpiton->req = NULL;

		/* Finished request; go to idle state */
		sdpiton->fsm_state = _piton_sd_FSM_STATE_IDLE;
		break;

	default:
		sdpiton->fsm_state = _piton_sd_FSM_STATE_IDLE;
		break;
	}
}

static void pitonsd_fsm_tasklet(unsigned long data)
{
	struct pitonsd_device *sdpiton = (void *)data;
	unsigned long flags;

	spin_lock_irqsave(&sdpiton->lock, flags);

	/* Loop over state machine until told to stop */
	sdpiton->fsm_continue_flag = 1;
	while (sdpiton->fsm_continue_flag)
          {
            pr_debug("fsm_state=%i\n", sdpiton->fsm_state);
            pitonsd_fsm_dostate(sdpiton);
          }

        pr_debug("fsm_continue=%i\n", sdpiton->fsm_continue_flag);
	spin_unlock_irqrestore(&sdpiton->lock, flags);
}

static void pitonsd_stall_timer(struct timer_list *t)
{
	struct pitonsd_device *sdpiton = from_timer(sdpiton, t, stall_timer);
	unsigned long flags;

	dev_warn(sdpiton->dev,
		 "kicking stalled fsm; state=%i task=%i iter=%i dc=%i\n",
		 sdpiton->fsm_state, sdpiton->fsm_task, sdpiton->fsm_iter_num,
		 sdpiton->data_count);
	spin_lock_irqsave(&sdpiton->lock, flags);

	/* Rearm the stall timer *before* entering FSM (which may then
	 * delete the timer) */
	mod_timer(&sdpiton->stall_timer, jiffies + HZ);
	/* Loop over state machine until told to stop */
	sdpiton->fsm_continue_flag = 1;
	while (sdpiton->fsm_continue_flag)
		pitonsd_fsm_dostate(sdpiton);

	spin_unlock_irqrestore(&sdpiton->lock, flags);
}

/* ---------------------------------------------------------------------
 * Interrupt handling routines
 */
static int pitonsd_interrupt_checkstate(struct pitonsd_device *sdpiton)
{
        u64 mask = (1 << sdpiton->data_count) - 1;
	u64 creg = sdpiton->baseaddr[ _piton_sd_ERROR ] & mask;
        u64 tstate = sdpiton->baseaddr[ _piton_sd_TRAN_STATE ];
        pr_debug("tran state = %llx\n", tstate);
        
	/* Check for error occurrence */
	if (mask != creg) {
		dev_err(sdpiton->dev, "transfer failure\n");
		pitonsd_dump_regs(sdpiton);
		return -EIO;
	}

	return 0;
}

static irqreturn_t pitonsd_interrupt(int irq, void *dev_id)
{
	struct pitonsd_device *sdpiton = dev_id;

	/* be safe and get the lock */
	spin_lock(&sdpiton->lock);
	sdpiton->in_irq = 1;

	/* clear the interrupt */
	sdpiton->baseaddr[ _piton_sd_CTRL ] = 0;

	/* check for IO failures */
	if (pitonsd_interrupt_checkstate(sdpiton))
		sdpiton->data_result = -EIO;

	if (sdpiton->fsm_task == 0) {
		dev_err(sdpiton->dev,
			"spurious irq; stat=0x%llx ctrl=0x%llx\n",
			sdpiton->baseaddr[ _piton_sd_STATUS ], sdpiton->baseaddr[ _piton_sd_CTRL ]);

		dev_err(sdpiton->dev, "fsm_task=%i fsm_state=%i data_count=%i\n",
			sdpiton->fsm_task, sdpiton->fsm_state, sdpiton->data_count);
	}

	/* Loop over state machine until told to stop */
	sdpiton->fsm_continue_flag = 1;
	while (sdpiton->fsm_continue_flag)
		pitonsd_fsm_dostate(sdpiton);

	/* done with interrupt; drop the lock */
	sdpiton->in_irq = 0;
	spin_unlock(&sdpiton->lock);

	return IRQ_HANDLED;
}

/* ---------------------------------------------------------------------
 * Block ops
 */
static void pitonsd_request(struct request_queue * q)
{
	struct request *req;
	struct pitonsd_device *sdpiton;

	req = pitonsd_get_next_request(q);

	if (req) {
		sdpiton = req->rq_disk->private_data;
		tasklet_schedule(&sdpiton->fsm_tasklet);
	}
}

static int pitonsd_open(struct block_device *bdev, fmode_t mode)
{
        struct gendisk *disk = bdev->bd_disk;
	struct pitonsd_device *sdpiton = disk->private_data;
        unsigned long flags;
	sdpiton->bdev = bdev;

	pr_debug("pitonsd_open() users=%i\n", sdpiton->users + 1);

	mutex_lock(&pitonsd_mutex);
	spin_lock_irqsave(&sdpiton->lock, flags);
	sdpiton->users++;
	spin_unlock_irqrestore(&sdpiton->lock, flags);

	check_disk_change(bdev);
	mutex_unlock(&pitonsd_mutex);

	return 0;
}

static void pitonsd_release(struct gendisk *disk, fmode_t mode)
{
	struct pitonsd_device *sdpiton = disk->private_data;
	unsigned long flags;

	pr_debug("pitonsd_release() users=%i\n", sdpiton->users - 1);

	mutex_lock(&pitonsd_mutex);
	spin_lock_irqsave(&sdpiton->lock, flags);
	sdpiton->users--;
	spin_unlock_irqrestore(&sdpiton->lock, flags);
	mutex_unlock(&pitonsd_mutex);
}

static int pitonsd_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	return -ENOTSUPP;
}

static void pitonsd_unlock_native_capacity(struct gendisk *disk)
{
        int p;
	struct pitonsd_device *sdpiton = disk->private_data;
	size_t inferred_capacity = 1;
        struct parsed_partitions *mbr = check_partition(sdpiton->bdev->bd_disk, sdpiton->bdev);
        printk("native partitions %d", mbr->limit);
        for (p = 1; p < mbr->limit; p++) {
                sector_t size, from;

                size = mbr->parts[p].size;
                if (!size)
                        continue;

                from = mbr->parts[p].from;
                printk("%s: p%d start %llu size %llu",
                       disk->disk_name, p,
                       (unsigned long long) from,
                       (unsigned long long) size);
                if (from + size > inferred_capacity) {
                  inferred_capacity = from + size;
                }
        }
        printk("Inferred capacity %lu", inferred_capacity);
        set_capacity(disk, inferred_capacity); // should come from hardware
}

static const struct block_device_operations pitonsd_fops = {
	.owner = THIS_MODULE,
	.open = pitonsd_open,
	.release = pitonsd_release,
	.getgeo = pitonsd_getgeo,
        .unlock_native_capacity = pitonsd_unlock_native_capacity,
};

/* --------------------------------------------------------------------
 * LowRISC_piton_sd device setup/teardown code
 */
static int pitonsd_setup(struct pitonsd_device *sdpiton)
{
	u16 version;
	int rc;
        
	printk("pitonsd_setup(sdpiton=0x%llx)\n", (u64)sdpiton);

	spin_lock_init(&sdpiton->lock);

	/*
	 * Map the device
	 */
	sdpiton->baseaddr = ioremap(sdpiton->physaddr, sdpiton->physaddrend - sdpiton->physaddr);
        sdpiton->ioptr = sdpiton->baseaddr+0x1000;
	if (!sdpiton->baseaddr)
		goto err_ioremap;

	printk("physaddr=0x%llx, vstart=%llx, vsiz=%llx, irq=%i\n",
		(unsigned long long)sdpiton->physaddr,
		(unsigned long long)sdpiton->baseaddr,
                (unsigned long long)(sdpiton->physaddrend - sdpiton->physaddr),
                sdpiton->irq);

#ifdef SELF_TEST
        {
          int i;
          u8 *buff = kzalloc(4096, GFP_KERNEL);
          for (i = 0; i < SELF_TEST_LIMIT; i+=8)
            {
              if (i % 1000 == 0) printk("self test iteration %d\n", i);
              sdpiton_self_test(sdpiton, buff, i, 8);
            }
        }
#endif

	/*
	 * Initialize the state machine tasklet and stall timer
	 */
	tasklet_init(&sdpiton->fsm_tasklet, pitonsd_fsm_tasklet, (unsigned long)sdpiton);
	timer_setup(&sdpiton->stall_timer, pitonsd_stall_timer, 0);

	/*
	 * Initialize the request queue
	 */
	sdpiton->queue = blk_init_queue(pitonsd_request, &sdpiton->lock);
	if (sdpiton->queue == NULL)
		goto err_blk_initq;
	blk_queue_logical_block_size(sdpiton->queue, 512);
	blk_queue_bounce_limit(sdpiton->queue, BLK_BOUNCE_HIGH);

	/*
	 * Allocate and initialize GD structure
	 */
	sdpiton->gd = alloc_disk(_piton_sd_NUM_MINORS);
	if (!sdpiton->gd)
		goto err_alloc_disk;

	sdpiton->gd->major = pitonsd_major;
	sdpiton->gd->first_minor = sdpiton->id * _piton_sd_NUM_MINORS;
	sdpiton->gd->fops = &pitonsd_fops;
	sdpiton->gd->queue = sdpiton->queue;
	sdpiton->gd->private_data = sdpiton;
	snprintf(sdpiton->gd->disk_name, 32, "rd%c", sdpiton->id + 'a');
        set_capacity(sdpiton->gd, 255*1023*31); // dummy (255 heads, 1023 cyl, 31 sectors)
        /* Tell the block layer that this is not a rotational device */
        blk_queue_flag_set(QUEUE_FLAG_NONROT, sdpiton->queue);
        blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, sdpiton->queue);
        
	/* Make sure version register is sane */
	version = sdpiton->baseaddr[ _piton_sd_VERSION ];
	if ((version == 0) || (version == 0xFFFF))
		goto err_read;

	/* Now we can hook up the irq handler */
	if (sdpiton->irq) {
		rc = request_irq(sdpiton->irq, pitonsd_interrupt, 0, "pitonsd", sdpiton);
		if (rc) {
			/* Failure - fall back to polled mode */
			dev_err(sdpiton->dev, "request_irq failed\n");
			sdpiton->irq = 0;
		}
	}

	/* Print the identification */
	dev_info(sdpiton->dev, "LowRISC_piton_sd revision %i.%i.%i\n",
		 (version >> 12) & 0xf, (version >> 8) & 0x0f, version & 0xff);
	printk("physaddr 0x%llx, mapped to 0x%llx, irq=%i\n",
               (u64) sdpiton->physaddr, (u64) sdpiton->baseaddr, sdpiton->irq);

	sdpiton->media_change = 1;

	/* Make the sysace device 'live' */
	add_disk(sdpiton->gd);

	return 0;

err_read:
	put_disk(sdpiton->gd);
err_alloc_disk:
	blk_cleanup_queue(sdpiton->queue);
err_blk_initq:
	iounmap(sdpiton->baseaddr);
err_ioremap:
	dev_info(sdpiton->dev, "pitonsd: error initializing device at 0x%llx\n",
		 (unsigned long long) sdpiton->physaddr);
	return -ENOMEM;
}

static void pitonsd_teardown(struct pitonsd_device *sdpiton)
{
	if (sdpiton->gd) {
		del_gendisk(sdpiton->gd);
		put_disk(sdpiton->gd);
	}

	if (sdpiton->queue)
		blk_cleanup_queue(sdpiton->queue);

	tasklet_kill(&sdpiton->fsm_tasklet);

	if (sdpiton->irq)
		free_irq(sdpiton->irq, sdpiton);

	iounmap(sdpiton->baseaddr);
}

static int pitonsd_alloc(struct device *dev, int id, resource_size_t physaddr, resource_size_t physaddrend, int irq)
{
	struct pitonsd_device *sdpiton;
	int rc;
	dev_dbg(dev, "pitonsd_alloc(%llx)\n", (u64)dev);

	if (!physaddr) {
		rc = -ENODEV;
		goto err_noreg;
	}

	/* Allocate and initialize the ace device structure */
	sdpiton = kzalloc(sizeof(struct pitonsd_device), GFP_KERNEL);
	if (!sdpiton) {
		rc = -ENOMEM;
		goto err_alloc;
	}
#ifdef SELF_TEST
        sdpiton->expected = kzalloc(SELF_TEST_LIMIT*sizeof(uint16_t), GFP_KERNEL);
	if (!sdpiton) {
		rc = -ENOMEM;
		goto err_alloc;
	}
#endif
	sdpiton->dev = dev;
	sdpiton->id = id;
	sdpiton->physaddr = physaddr;
	sdpiton->physaddrend = physaddrend;
	sdpiton->irq = irq;

	/* Call the setup code */
	rc = pitonsd_setup(sdpiton);
	if (rc)
		goto err_setup;

	dev_set_drvdata(dev, sdpiton);
        pr_debug("pitonsd_alloc returned success\n");
	return 0;

err_setup:
	dev_set_drvdata(dev, NULL);
	kfree(sdpiton);
err_alloc:
err_noreg:
	dev_err(dev, "could not initialize device, err=%i\n", rc);
	return rc;
}

static void pitonsd_free(struct device *dev)
{
	struct pitonsd_device *sdpiton = dev_get_drvdata(dev);
	dev_dbg(dev, "pitonsd_free(%llx)\n", (u64)dev);

	if (sdpiton) {
		pitonsd_teardown(sdpiton);
		dev_set_drvdata(dev, NULL);
		kfree(sdpiton);
	}
}

/* ---------------------------------------------------------------------
 * Platform Bus Support
 */

static int pitonsd_probe(struct platform_device *dev)
{
	resource_size_t physaddr = 0;
	resource_size_t physaddrend = 0;
	u32 id = dev->id;
	int irq = 0;
	int i;

	printk("pitonsd_probe(%llx)\n", (u64)dev);

	/* device id and bus width */
	if (of_property_read_u32(dev->dev.of_node, "port-number", &id))
		id = 0;

	for (i = 0; i < dev->num_resources; i++) {
		if (dev->resource[i].flags & IORESOURCE_MEM)
                  {
                    physaddr = dev->resource[i].start;
                    physaddrend = dev->resource[i].end;
                  }
		if (dev->resource[i].flags & IORESOURCE_IRQ)
			irq = dev->resource[i].start;
	}

	/* Call the bus-independent setup code */
	return pitonsd_alloc(&dev->dev, id, physaddr, physaddrend, irq);
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
