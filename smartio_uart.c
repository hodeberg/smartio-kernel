#include <linux/tty.h>
#include <linux/module.h>
#include <linux/slab.h>

#define RCV_BUF_SIZE 40
#define STX 2 
#define ETX 
#define SMARTIO_GET_NO_OF_MODULES  1
struct ldisc_data {
  char rcvbuf[RCV_BUF_SIZE];
};


static void write_buf(struct tty_struct *tty)
{
  unsigned char get_no_of_modules[] = {
    STX,
    8 - 2,
    7,
    0, // MODULE 0
    SMARTIO_GET_NO_OF_MODULES,
    0x11,
    0x22,
    ETX
  };
  int chars_written;

  chars_written = tty->driver->ops->write(tty,
					  get_no_of_modules,
					  ARRAY_SIZE(get_no_of_modules));
  pr_warn("smartio_uart: wrote %d chars out of %zu\n", chars_written, ARRAY_SIZE(get_no_of_modules));
  tty_driver_flush_buffer(tty);
}

static int l_open(struct tty_struct *tty)
{
  pr_warn("smartio_uart: line discipline open\n");
  /* Allocate device-specific data */
  tty->disc_data = kzalloc(sizeof(struct ldisc_data), GFP_KERNEL);
  if (!tty->disc_data) {
    pr_err("Failed to allocate smartio uart line disciplin data\n");
    goto err;
  }
  /* Tell driver how much data we can receive */
  tty->receive_room = RCV_BUF_SIZE;

  /* Test writing something */
  write_buf(tty);
  return 0;
 err:
 return -1;
}

static void l_close(struct tty_struct *tty)
{
  kfree(tty->disc_data);
  pr_warn("smartio_uart: line discipline closed\n");
}

static int l_receive_buf2(struct tty_struct *tty, 
			  const unsigned char *buf,
			  char *flags,
			  int count)
{
  int i;

  pr_warn("smartio_uart: received %d chars\n", count);
  if (flags)
    pr_warn("smartio_uart: flags are %X\n", (unsigned int) *flags);
  else
    pr_warn("smartio_uart: flags are not set\n");
  pr_warn("smartio_uart: char dump:\n");
  for (i=0; i<count; i++)
    pr_warn("%x ", buf[i]);
  pr_warn("\n");

  return count;
}

static void l_receive_buf(struct tty_struct *tty, 
			 const unsigned char *buf,
			 char *flags,
			 int count)
{
  int i;

  pr_warn("smartio_uart: received %d chars\n", count);
  if (flags)
    pr_warn("smartio_uart: flags are %X\n", (unsigned int) *flags);
  else
    pr_warn("smartio_uart: flags are not set\n");
  pr_warn("smartio_uart: char dump:\n");
  for (i=0; i<count; i++)
    pr_warn("%x ", buf[i]);
  pr_warn("\n");
}

#define MYNUM 28

static struct tty_ldisc_ops smart_ldisc = {
  .owner = THIS_MODULE,
  .magic = TTY_LDISC_MAGIC,
  .name = "l_smartio",

  .open = l_open,
  .close = l_close,
#if (VERSION>=3) &&  (PATCHLEVEL>=12)
  .receive_buf2 = l_receive_buf2,
#else
.receive_buf = l_receive_buf,
#endif
#if 0
  .flush_buffer = l_flush_buffer,
  .chars_in_buffer = l_chars_in_buffer,
  .read = l_read,
  .write = l_write,
  .poll = l_poll,
  .hangup = l_hangup,


  .fasync = l_fasync,
  .write_wakeup = l_write_wakeup
#endif
};


static int __init my_init(void)
{
  int status;

  status = tty_register_ldisc(MYNUM, &smart_ldisc);
  if (status) 
    pr_err("Failed to register line discipline: %d\n", status);
  else
    pr_warn("Done registering line discipline\n");
  return status;
}
module_init(my_init);

static void __exit my_cleanup(void)
{
  int status;

  status = tty_unregister_ldisc(MYNUM);
  if (status)
    pr_err("Error unregistering line disciple: %d\n", status);
  pr_warn("Unregistered line disciple %d\n", MYNUM);
}
module_exit(my_cleanup);




MODULE_AUTHOR("Hans Odeberg <hans.odeberg@intel.com>");
MODULE_DESCRIPTION("Smartio uart driver");
MODULE_LICENSE("GPL v2");
