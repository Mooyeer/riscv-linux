#include <linux/init.h>
#include <linux/console.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/tty_driver.h>
#include <linux/module.h>
#include <linux/interrupt.h>

#include <asm/sbi.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/spinlock.h>
 
/* A timer list */
struct timer_list keyb_timer;
static DEFINE_SPINLOCK(sbi_tty_port_lock);
static DEFINE_SPINLOCK(sbi_timer_lock);
static struct tty_port sbi_tty_port;
static struct tty_driver *sbi_tty_driver;

static irqreturn_t sbi_console_isr(int irq, void *dev_id)
{
	int ch = sbi_console_getchar();
	if (ch < 0)
		return IRQ_NONE;

	spin_lock(&sbi_tty_port_lock);
	tty_insert_flip_char(&sbi_tty_port, ch, TTY_NORMAL);
	tty_flip_buffer_push(&sbi_tty_port);
	spin_unlock(&sbi_tty_port_lock);

	return IRQ_HANDLED;
}

enum edcl_mode {edcl_mode_unknown, edcl_mode_read, edcl_mode_write, edcl_mode_block_read, edcl_max=256};

#pragma pack(4)

static struct etrans {
  enum edcl_mode mode;
  volatile uint32_t *ptr;
  uint32_t val;
} edcl_trans[edcl_max+1];

#pragma pack()

static int edcl_cnt;

/* shared address space pointer (appears at 0x800000 in minion address map */
volatile static struct etrans *shared_base;
volatile uint32_t * const rxfifo_base = (volatile uint32_t*)(4<<20);

int shared_read(volatile struct etrans *addr, int cnt, struct etrans *obuf)
  {
    int i;
    spin_lock(&sbi_timer_lock);
    for (i = 0; i < cnt; i++)
      {
	obuf[i] = addr[i];
#ifdef SDHCI_VERBOSE4
	printk("shared_read(%d, %p) => %p,%x;\n", i, addr+i, obuf[i].ptr, obuf[i].val);
#endif
      }
    spin_unlock(&sbi_timer_lock);
    return 0;
  }

int shared_write(volatile struct etrans *addr, int cnt, struct etrans *ibuf)
  {
    int i;
    spin_lock(&sbi_timer_lock);
    for (i = 0; i < cnt; i++)
      {
	addr[i] = ibuf[i];
#ifdef SDHCI_VERBOSE4
	{
	  int j;
	  printk("shared_write(%d, %p, 0x%x, 0x%p);\n", i, addr+i, cnt, ibuf);
	  for (j = 0; j < sizeof(struct etrans); j++)
	    printk("%x ", ((volatile uint8_t *)(&addr[i]))[j]);
	  printk("\n");
	}
#endif	
      }
    spin_unlock(&sbi_timer_lock);
    return 0;
  }

int queue_flush(void)
{
  int cnt;
  struct etrans tmp;
  spin_lock(&sbi_timer_lock);
  tmp.val = 0xDEADBEEF;
  edcl_trans[edcl_cnt++].mode = edcl_mode_unknown;
#ifdef VERBOSE
  printk("sizeof(struct etrans) = %d\n", sizeof(struct etrans));
  for (int i = 0; i < edcl_cnt; i++)
    {
      switch(edcl_trans[i].mode)
	{
	case edcl_mode_write:
	  printk("queue_mode_write(%p, 0x%x);\n", edcl_trans[i].ptr, edcl_trans[i].val);
	  break;
	case edcl_mode_read:
	  printk("queue_mode_read(%p, 0x%x);\n", edcl_trans[i].ptr, edcl_trans[i].val);
	  break;
	case edcl_mode_unknown:
	  if (i == edcl_cnt-1)
	    {
	    printk("queue_end();\n");
	    break;
	    }
	default:
	  printk("queue_mode %d\n", edcl_trans[i].mode);
	  break;
	}
    }
#endif
  shared_write(shared_base, edcl_cnt, edcl_trans);
  shared_write(shared_base+edcl_max, 1, &tmp);
  do {
#ifdef VERBOSE
    int i = 10000000;
    int tot = 0;
    while (i--) tot += i;
    printk("waiting for minion %x\n", tot);
#endif
    shared_read(shared_base, 1, &tmp);
  } while (tmp.ptr);
  tmp.val = 0;
  shared_write(shared_base+edcl_max, 1, &tmp);
  cnt = edcl_cnt;
  edcl_cnt = 1;
  edcl_trans[0].mode = edcl_mode_read;
  edcl_trans[0].ptr = (volatile uint32_t*)(8<<20);
  spin_unlock(&sbi_timer_lock);
  return cnt;
}

void queue_write(volatile uint32_t *const sd_ptr, uint32_t val, int flush)
 {
   struct etrans tmp;
   spin_lock(&sbi_timer_lock);
#if 0
   flush = 1;
#endif   
   tmp.mode = edcl_mode_write;
   tmp.ptr = sd_ptr;
   tmp.val = val;
   edcl_trans[edcl_cnt++] = tmp;
   if (flush || (edcl_cnt==edcl_max-1))
     {
       queue_flush();
     }
#ifdef VERBOSE  
   printk("queue_write(%p, 0x%x);\n", tmp.ptr, tmp.val);
#endif
   spin_unlock(&sbi_timer_lock);
 }

uint32_t queue_read(volatile uint32_t * const sd_ptr)
 {
   int cnt;
   struct etrans tmp;
   spin_lock(&sbi_timer_lock);
   tmp.mode = edcl_mode_read;
   tmp.ptr = sd_ptr;
   tmp.val = 0xDEADBEEF;
   edcl_trans[edcl_cnt++] = tmp;
   cnt = queue_flush();
   shared_read(shared_base+(cnt-2), 1, &tmp);
#ifdef VERBOSE
   printk("queue_read(%p, %p, 0x%x);\n", sd_ptr, tmp.ptr, tmp.val);
#endif   
   spin_unlock(&sbi_timer_lock);
   return tmp.val;
 }

void queue_read_array(volatile uint32_t * const sd_ptr, uint32_t cnt, uint32_t iobuf[])
 {
   int i, n, cnt2;
   struct etrans tmp;
   spin_lock(&sbi_timer_lock);
   if (edcl_cnt+cnt >= edcl_max)
     {
     queue_flush();
     }
   for (i = 0; i < cnt; i++)
     {
       tmp.mode = edcl_mode_read;
       tmp.ptr = sd_ptr+i;
       tmp.val = 0xDEADBEEF;
       edcl_trans[edcl_cnt++] = tmp;
     }
   cnt2 = queue_flush();
   n = cnt2-1-cnt;
   shared_read(shared_base+n, cnt, edcl_trans+n);
   for (i = n; i < n+cnt; i++) iobuf[i-n] = edcl_trans[i].val;
   spin_unlock(&sbi_timer_lock);
 }

uint32_t queue_block_read2(int i)
{
  uint32_t rslt;
  spin_lock(&sbi_timer_lock);
  rslt = __be32_to_cpu(((volatile uint32_t *)(shared_base+1))[i]);
  spin_unlock(&sbi_timer_lock);
  return rslt;
}

int queue_block_read1(void)
{
   struct etrans tmp;
   spin_lock(&sbi_timer_lock);
   queue_flush();
   tmp.mode = edcl_mode_block_read;
   tmp.ptr = rxfifo_base;
   tmp.val = 1;
   shared_write(shared_base, 1, &tmp);
   tmp.val = 0xDEADBEEF;
   shared_write(shared_base+edcl_max, 1, &tmp);
   do {
    shared_read(shared_base, 1, &tmp);
  } while (tmp.ptr);
#ifdef SDHCI_VERBOSE3
   printk("queue_block_read1 completed\n");
#endif
   spin_unlock(&sbi_timer_lock);
   return tmp.mode;
}

void rx_write_fifo(uint32_t data)
{
  queue_write(rxfifo_base, data, 0);
}

uint32_t rx_read_fifo(void)
{
  return queue_read(rxfifo_base);
}

void write_led(uint32_t data)
{
  volatile uint32_t * const led_base = (volatile uint32_t*)(7<<20);
  queue_write(led_base, data, 1);
}
 
/* Timer callback */
void timer_callback(unsigned long arg)
{
  uint32_t key;
  volatile uint32_t * const keyb_base = (volatile uint32_t*)(9<<20);
  key = queue_read(keyb_base);
  while ((1<<28) & ~key) /* FIFO not empty */
    {
      int ch;
      queue_write(keyb_base+1, 0, 0);
      ch = queue_read(keyb_base+1) >> 8; /* strip off the scan code (default ascii code is UK) */
      spin_lock(&sbi_tty_port_lock);
      tty_insert_flip_char(&sbi_tty_port, ch, TTY_NORMAL);
      tty_flip_buffer_push(&sbi_tty_port);
      spin_unlock(&sbi_tty_port_lock);
      key = queue_read(keyb_base);
    }
  mod_timer(&keyb_timer, jiffies + 6); /* restarting timer */
}
 
/* Init the timer */
static void keyb_init_timer(void)
{
    init_timer(&keyb_timer);
    keyb_timer.function = timer_callback;
    keyb_timer.data = 0;
    keyb_timer.expires = jiffies + 6;
    add_timer(&keyb_timer); /* Starting the timer */
 
    printk(KERN_INFO "keyb_timer is started\n");
}

volatile uint32_t * const video_base = (volatile uint32_t*)(10<<20);

static char old_video[4096];

static void video_write(int addr, unsigned char ch)
{
  old_video[addr&4095] = ch;
  queue_write(video_base+addr, ch, 0);
}

static void minion_console_putchar(unsigned char ch)
{
  static int addr_int = 0;
  switch(ch)
    {
    case 8: case 127: if (addr_int & 127) --addr_int; break;
    case 13: addr_int = addr_int & -128; break;
    case 10: addr_int = (addr_int|127)+1; break;
    default: video_write(addr_int++, ch);
    }
  if (addr_int >= 4096-128)
    {
      // this is where we scroll
      memcpy(old_video, old_video+128, 4096-128);
      memset(old_video+4096-128, ' ', 128);
      for (addr_int = 0; addr_int < 4096; addr_int++)
	queue_write(video_base+addr_int, old_video[addr_int], 0);
      addr_int = 4096-256;
    }
  sbi_console_putchar(ch);
}

static int sbi_tty_open(struct tty_struct *tty, struct file *filp)
{
	return 0;
}

static int sbi_tty_write(struct tty_struct *tty,
	const unsigned char *buf, int count)
{
	const unsigned char *end;

	for (end = buf + count; buf < end; buf++) {
		minion_console_putchar(*buf);
	}
	return count;
}

static int sbi_tty_write_room(struct tty_struct *tty)
{
	return 1024; /* arbitrary */
}

static const struct tty_operations sbi_tty_ops = {
	.open		= sbi_tty_open,
	.write		= sbi_tty_write,
	.write_room	= sbi_tty_write_room,
};


static void sbi_console_write(struct console *co, const char *buf, unsigned n)
{
	for ( ; n > 0; n--, buf++) {
		if (*buf == '\n')
			minion_console_putchar('\r');
		minion_console_putchar(*buf);
	}
}

static struct tty_driver *sbi_console_device(struct console *co, int *index)
{
	*index = co->index;
	return sbi_tty_driver;
}

static int sbi_console_setup(struct console *co, char *options)
{
	return co->index != 0 ? -ENODEV : 0;
}

static struct console sbi_console = {
	.name	= "sbi_console",
	.write	= sbi_console_write,
	.device	= sbi_console_device,
	.setup	= sbi_console_setup,
	.flags	= CON_PRINTBUFFER,
	.index	= -1
};

static int __init sbi_console_init(void)
{
	int ret;
	shared_base = (volatile struct etrans *)ioremap(0x40010000, 0x2000);

	register_console(&sbi_console);

	sbi_tty_driver = tty_alloc_driver(1,
		TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (unlikely(IS_ERR(sbi_tty_driver)))
		return PTR_ERR(sbi_tty_driver);

	sbi_tty_driver->driver_name = "sbi";
	sbi_tty_driver->name = "ttySBI";
	sbi_tty_driver->major = TTY_MAJOR;
	sbi_tty_driver->minor_start = 0;
	sbi_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	sbi_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	sbi_tty_driver->init_termios = tty_std_termios;
	tty_set_operations(sbi_tty_driver, &sbi_tty_ops);

	tty_port_init(&sbi_tty_port);
	tty_port_link_device(&sbi_tty_port, sbi_tty_driver, 0);

	ret = tty_register_driver(sbi_tty_driver);
	if (unlikely(ret))
		goto out_tty_put;

	ret = request_irq(IRQ_SOFTWARE, sbi_console_isr, IRQF_SHARED,
	                  sbi_tty_driver->driver_name, sbi_console_isr);
	if (unlikely(ret))
		goto out_tty_put;

	/* Poll the console once, which will trigger future interrupts */
	sbi_console_isr(0, NULL);

	/* Init the spinlock */
	spin_lock_init(&sbi_timer_lock);
 
	/* Init the timer */
	keyb_init_timer();
	return ret;

out_tty_put:
	put_tty_driver(sbi_tty_driver);
	return ret;
}

static void __exit sbi_console_exit(void)
{
	tty_unregister_driver(sbi_tty_driver);
	put_tty_driver(sbi_tty_driver);
}

module_init(sbi_console_init);
module_exit(sbi_console_exit);

MODULE_DESCRIPTION("RISC-V SBI console driver");
MODULE_LICENSE("GPL");

#ifdef CONFIG_EARLY_PRINTK

static struct console early_console_dev __initdata = {
	.name	= "early",
	.write	= sbi_console_write,
	.flags	= CON_PRINTBUFFER | CON_BOOT,
	.index	= -1
};

static int __init setup_early_printk(char *str)
{
	if (early_console == NULL) {
		early_console = &early_console_dev;
		register_console(early_console);
	}
	return 0;
}

early_param("earlyprintk", setup_early_printk);

#endif
