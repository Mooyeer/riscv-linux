#include <linux/init.h>
#include <linux/console.h>
#include <linux/serial_reg.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/tty_driver.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <asm/config-string.h>
#include <asm/sbi.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/spinlock.h>

struct timer_list keyb_timer;
static DEFINE_SPINLOCK(xuart_tty_port_lock);
static DEFINE_SPINLOCK(sbi_timer_lock);
static struct tty_port xuart_tty_port;
static struct tty_driver *xuart_tty_driver;
static volatile uint32_t *keyb_base, *vid_base;

/* Timer callback */
void timer_callback(unsigned long arg)
{
  uint32_t key = *keyb_base;
  while ((1<<16) & ~key) /* FIFO not empty */
    {
      int ch;
      *keyb_base = 0;
      ch = *keyb_base >> 8; /* strip off the scan code (default ascii code is UK) */
      spin_lock(&xuart_tty_port_lock);
      tty_insert_flip_char(&xuart_tty_port, ch, TTY_NORMAL);
      tty_flip_buffer_push(&xuart_tty_port);
      spin_unlock(&xuart_tty_port_lock);
      key = *keyb_base;
    }
  //  printk(KERN_INFO "keyb_timer callback\n");
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

static void minion_console_putchar(unsigned char ch)
{
  static int addr_int = 0;
  switch(ch)
    {
    case 8: case 127: if (addr_int & 127) --addr_int; break;
    case 13: addr_int = addr_int & -128; break;
    case 10: addr_int = (addr_int|127)+1; break;
    default: vid_base[addr_int++] = ch;
    }
  if (addr_int >= 4096-128)
    {
      // this is where we scroll
      for (addr_int = 0; addr_int < 4096; addr_int++)
        if (addr_int < 4096-128)
          vid_base[addr_int] = vid_base[addr_int+128];
        else
          vid_base[addr_int] = ' ';
      addr_int = 4096-256;
    }
}

static int xuart_tty_open(struct tty_struct *tty, struct file *filp)
{
	return 0;
}

static int xuart_tty_write(struct tty_struct *tty,
	const unsigned char *buf, int count)
{
	const unsigned char *end;

	for (end = buf + count; buf < end; buf++) {
		minion_console_putchar(*buf);
	}
	return count;
}

static int xuart_tty_write_room(struct tty_struct *tty)
{
	return 1024; /* arbitrary */
}

static const struct tty_operations xuart_tty_ops = {
	.open		= xuart_tty_open,
	.write		= xuart_tty_write,
	.write_room	= xuart_tty_write_room,
};

static void xuart_console_write(struct console *co, const char *buf, unsigned n)
{
	for ( ; n > 0; n--, buf++) {
		if (*buf == '\n')
			minion_console_putchar('\r');
		minion_console_putchar(*buf);
	}
}

static struct tty_driver *xuart_console_device(struct console *co, int *index)
{
	*index = co->index;
	return xuart_tty_driver;
}

static int xuart_console_setup(struct console *co, char *options)
{
	return co->index != 0 ? -ENODEV : 0;
}

static struct console xuart_console = {
	.name	= "xuart_console",
	.write	= xuart_console_write,
	.device	= xuart_console_device,
	.setup	= xuart_console_setup,
	.flags	= CON_PRINTBUFFER,
	.index	= -1
};

static struct resource lowrisc_hid[] = {
	[0] = {
		.start = 0,
		.end   = 0xFFF,
		.flags = IORESOURCE_MEM,
	},
	[1] = {
		.start = 0x8000,
		.end   = 0xFFFF,
		.flags = IORESOURCE_MEM,
	},
};

static int __init xuart_console_init(void)
{
	int ret;
	// Find config string driver
	struct device *csdev = bus_find_device_by_name(&platform_bus_type, NULL, "config-string");
	struct platform_device *pcsdev = to_platform_device(csdev);
	u64 hid_addr = config_string_u64(pcsdev, "hid.addr");
	lowrisc_hid[0].start += hid_addr;
	lowrisc_hid[0].end += hid_addr;
	lowrisc_hid[1].start += hid_addr;
	lowrisc_hid[1].end += hid_addr;
	keyb_base = (volatile uint32_t *)ioremap(lowrisc_hid[0].start, resource_size(lowrisc_hid+0));
	vid_base = (volatile uint32_t *)ioremap(lowrisc_hid[1].start, resource_size(lowrisc_hid+1));
	
	printk("hid_keyboard address %llx, remapped to %p\n", lowrisc_hid[0].start, keyb_base);
	printk("hid_display address %llx, remapped to %p\n", lowrisc_hid[1].start, vid_base);
	
	register_console(&xuart_console);

	xuart_tty_driver = tty_alloc_driver(1,
		TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (unlikely(IS_ERR(xuart_tty_driver)))
		return PTR_ERR(xuart_tty_driver);

	xuart_tty_driver->driver_name = "sbi";
	xuart_tty_driver->name = "ttySBI";
	xuart_tty_driver->major = TTY_MAJOR;
	xuart_tty_driver->minor_start = 0;
	xuart_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	xuart_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	xuart_tty_driver->init_termios = tty_std_termios;
	tty_set_operations(xuart_tty_driver, &xuart_tty_ops);

	tty_port_init(&xuart_tty_port);
	tty_port_link_device(&xuart_tty_port, xuart_tty_driver, 0);

	ret = tty_register_driver(xuart_tty_driver);
	if (unlikely(ret))
		goto out_tty_put;
      
	/* Init the spinlock */
	spin_lock_init(&sbi_timer_lock);
 
	/* Init the timer */
	keyb_init_timer();
	return ret;

out_tty_put:
	put_tty_driver(xuart_tty_driver);
	return ret;
}

static void __exit xuart_console_exit(void)
{
	tty_unregister_driver(xuart_tty_driver);
	put_tty_driver(xuart_tty_driver);
}

module_init(xuart_console_init);
module_exit(xuart_console_exit);

MODULE_DESCRIPTION("RISC-V SBI console driver");
MODULE_LICENSE("GPL");
