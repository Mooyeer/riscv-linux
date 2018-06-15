#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/console.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/of_platform.h>
#include <linux/init.h>
#include <linux/console.h>
#include <linux/serial_reg.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/tty_driver.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <asm/sbi.h>
#include <linux/module.h>
#include <linux/timer.h>
#include <linux/spinlock.h>

#define DRIVER_NAME "lowrisc-uart"

struct lowrisc_uart_con {
  struct platform_device *pdev;
  struct resource *keyb, *vid, *uart;
  spinlock_t lock;
  volatile uint32_t *keyb_base;
  volatile uint32_t *vid_base;
  volatile uint32_t *uart_base;
  void __iomem *ioaddr; /* mapped address */
  int irq;
  int int_en;
};

static DEFINE_SPINLOCK(xuart_tty_port_lock);
static struct tty_port xuart_tty_port;
static volatile uint32_t xuart_ref_cnt;

void xuart_putchar(int data)
{
  sbi_console_putchar(data&0x7f);
}

static void minion_console_putchar(unsigned char ch)
{
  xuart_putchar(ch);
}

static int xuart_tty_open(struct tty_struct *tty, struct file *filp)
{
  xuart_ref_cnt++;
  return 0;
}

static void xuart_tty_close(struct tty_struct *tty, struct file *filp)
{
  if (xuart_ref_cnt)
      xuart_ref_cnt--;
}

static void xuart_console_poll(struct lowrisc_uart_con *con)
{
  int ch = sbi_console_getchar();
  if (ch && xuart_ref_cnt)
    {
      //      sbi_console_putchar(ch);
      spin_lock(&xuart_tty_port_lock);
      tty_insert_flip_char(&xuart_tty_port, ch, TTY_NORMAL);
      tty_flip_buffer_push(&xuart_tty_port);
      spin_unlock(&xuart_tty_port_lock);      
    }
}

static irqreturn_t lowrisc_uart_irq(int irq, void *dev_id)
{
  struct lowrisc_uart_con *con = (struct lowrisc_uart_con *)dev_id;
  irqreturn_t ret;
  xuart_console_poll(con);
  ret = IRQ_HANDLED;
  return ret;
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
	.close		= xuart_tty_close,
	.write		= xuart_tty_write,
	.write_room	= xuart_tty_write_room,
};

static int lowrisc_uart_remove(struct platform_device *pdev)
{
  return 0;
}

static const struct of_device_id lowrisc_uart_of_match[] = {
  { .compatible = DRIVER_NAME },
  { }
};

MODULE_DEVICE_TABLE(of, lowrisc_uart_of_match);

static int lowrisc_uart_probe(struct platform_device *pdev)
{
  int ret = 0;
  struct resource *iomem;
  struct lowrisc_uart_con *con;
  printk("SBI console probe beginning\n");
  iomem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
  con = kzalloc(sizeof(struct lowrisc_uart_con), GFP_KERNEL);
  if (!con) {
    return -ENOMEM;
  }

  con->pdev = pdev;
  if (!request_mem_region(iomem->start, resource_size(iomem), DRIVER_NAME)) {
    dev_err(&pdev->dev, "cannot request region\n");
    return -EBUSY;
  }

  con->ioaddr = ioremap(iomem->start, resource_size(iomem));
  if (!con->ioaddr) {
    return -ENOMEM;
  }
  
  printk(DRIVER_NAME " : Lowrisc uart platform driver (%llX-%llX) mapped to %lx\n",
               iomem[0].start,
               iomem[0].end,
               (size_t)(con->ioaddr));
        
  con->irq = platform_get_irq(pdev, 0);
  printk("Requesting interrupt %d\n", con->irq);

  ret = request_irq(con->irq, lowrisc_uart_irq,
				   IRQF_SHARED, DRIVER_NAME, con);
  if (ret) return ret;
  
  printk("SBI console probed and mapped\n");
  return 0;
}

static struct platform_driver lowrisc_uart_driver = {
  .driver = {
    .name = DRIVER_NAME,
    .of_match_table = lowrisc_uart_of_match,
  },
  .probe = lowrisc_uart_probe,
  .remove = lowrisc_uart_remove,
};

module_platform_driver(lowrisc_uart_driver);

MODULE_DESCRIPTION("RISC-V SBI console driver");
MODULE_LICENSE("GPL");

