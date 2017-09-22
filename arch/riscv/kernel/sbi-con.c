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

static DEFINE_SPINLOCK(xuart_tty_port_lock);
static struct tty_port xuart_tty_port;
static struct tty_driver *xuart_tty_driver;
static volatile uint32_t *uart_base_ptr;

static void xuart_putchar(int data)
{
#if 1
  sbi_console_putchar(data);
#else  
  // wait until THR empty
  while(! (*(uart_base_ptr + UART_LSR) & UART_LSR_TEMT)); /* should that be UART_LSR_THRE ?? */
  *(uart_base_ptr + UART_TX) = data;
#endif  
}

// enable uart read IRQ
void xuart_enable_read_irq(void) {
  *(uart_base_ptr + UART_IER) |= UART_IER_RDI;
}

// disable uart read IRQ
void xuart_disable_read_irq(void) {
  *(uart_base_ptr + UART_IER) &= ~UART_IER_RDI;
}

enum {inc=HZ/10}; // 1Hz

static struct timer_list xuart_timer;

static irqreturn_t xuart_console_isr(int irq, void *dev_id)
{
        if (*(uart_base_ptr + UART_LSR) & UART_LSR_DR)
          {
            xuart_disable_read_irq();
            do {
              int ch = *(uart_base_ptr + UART_RX);            
              //              *(uart_base_ptr + UART_TX) = ch;
              spin_lock(&xuart_tty_port_lock);
              tty_insert_flip_char(&xuart_tty_port, ch, TTY_NORMAL);
              tty_flip_buffer_push(&xuart_tty_port);
              spin_unlock(&xuart_tty_port_lock);
            } while (*(uart_base_ptr + UART_LSR) & UART_LSR_DR);
            xuart_enable_read_irq();
            
            return IRQ_HANDLED;
          }
        else
          {
            return IRQ_NONE;
          }
}

/* Timer callback */
void xuart_timer_callback(unsigned long arg)
{
  void *dev_id = (void *)arg;
  //  printk("xuart_timer_callback();\n");
  //  xuart_console_isr(-1, dev_id);
  mod_timer(&xuart_timer, jiffies + inc); /* restarting timer */
}

static void xuart_init_timer(void *dev)
{
  init_timer(&xuart_timer);
  xuart_timer.function = xuart_timer_callback;
  xuart_timer.data = (unsigned long)dev;
  xuart_timer.expires = jiffies + inc;
  add_timer(&xuart_timer); /* Starting the timer */
  
  printk(KERN_INFO "xuart_timer is started\n");
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
		xuart_putchar(*buf);
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
			xuart_putchar('\r');
		xuart_putchar(*buf);
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

static int __init xuart_console_init(void)
{
	int ret;
	// Find config string driver
	struct device *csdev = bus_find_device_by_name(&platform_bus_type, NULL, "config-string");
	struct platform_device *pcsdev = to_platform_device(csdev);
        u64 ubase = config_string_u64(pcsdev, "uart.addr");
        uart_base_ptr = 0x400 + (volatile uint32_t *)ioremap(ubase, 8192);
        printk("xuart_console address %llx, remapped to %p\n", ubase, uart_base_ptr);
        
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
      
	ret = request_irq(IRQ_SOFTWARE, xuart_console_isr, IRQF_SHARED,
	                  xuart_tty_driver->driver_name, xuart_console_isr);

	if (unlikely(ret))
		goto out_tty_put;

#if 1
	/* Trigger future interrupts */
        xuart_enable_read_irq();
#else
        xuart_disable_read_irq();
#endif        
        xuart_init_timer(NULL);

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

#ifdef CONFIG_EARLY_PRINTK

static struct console early_console_dev __initdata = {
	.name	= "early",
	.write	= xuart_console_write,
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
