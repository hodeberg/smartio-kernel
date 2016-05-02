#include <linux/tty.h>
#include <linux/module.h>

int l_open(struct tty_struct *p)
{
  pr_warn("open called\n");
  return 0;
}

void l_close(struct tty_struct *p)
{
  pr_warn("close called\n");
}


#define MYNUM 28

static struct tty_ldisc_ops smart_ldisc = {
  .owner = THIS_MODULE,
  .magic = TTY_LDISC_MAGIC,
  .name = "l_smartio",

  .open = l_open,
  .close = l_close,
#if 0
  .flush_buffer = l_flush_buffer,
  .chars_in_buffer = l_chars_in_buffer,
  .read = l_read,
  .write = l_write,
  .poll = l_poll,
  .hangup = l_hangup,

  .receive_buf2 = l_receive_buf2,
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
