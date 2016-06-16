#include <linux/module.h>
#include <linux/i2c-smbus.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>

static struct i2c_client *alert_client;
const int gpioNum = 13;

static irqreturn_t my_handler(int irq, void *dev)
{
  pr_warn("Interrupt handler for SMBUS alert\n");

  return IRQ_HANDLED;
}



static int __init my_init(void)
{
  int status = 0;
  struct i2c_smbus_alert_setup setup = {
    .alert_edge_triggered = 1,
  };
  struct i2c_adapter *adapter = i2c_get_adapter(6);


  if (!adapter) {
    pr_err("Failed to find i2c adapter #%d\n", 6);
    status = -1;
    goto fail;
  }

  if (!gpio_is_valid(gpioNum)) {
     pr_err("GPIO number %d is not valid\n", gpioNum);
     goto fail;
  }
  status = gpio_request_one(gpioNum, GPIOF_DIR_IN, "smbus_alert");
  if (status < 0) {
     pr_err("GPIO allocation failed, result %d\n", status);
     goto fail;
  }
  setup.irq = gpio_to_irq(gpioNum);
  if (setup.irq < 0) {
     pr_err("Failed to get gpio interrupt, err is %d\n", setup.irq);
     goto release_gpio;
  }

#if 1
  alert_client = i2c_setup_smbus_alert(adapter, &setup);
  if (!alert_client) {
     pr_err("Failed to register smbus alert device.\n");
     goto release_gpio;
  }

  i2c_put_adapter(adapter);
#else
  status = request_threaded_irq(setup.irq, NULL, my_handler, 
				IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
				"smartio_smbus", &gpioNum);
  if (status < 0) {
     pr_err("Failed to install interrupt handlert, err is %d\n", status);
     goto release_gpio;
  }
#endif

  pr_warn("Done setting up smbus alert\n");
  return status;

 release_gpio:
  gpio_free(gpioNum);
 release_adapter:
  i2c_put_adapter(adapter);
 fail:
  pr_err("Failed setting up smbus alert\n");
  return status;
}
module_init(my_init);

static void __exit my_cleanup(void)
{
#if 0
  i2c_unregister_device(alert_client);
  gpiod_put(gpio);
#endif
  gpio_free(gpioNum);
  pr_warn("Removed smbus alert\n");
}
module_exit(my_cleanup);




MODULE_AUTHOR("Hans Odeberg <hans.odeberg@intel.com>");
MODULE_DESCRIPTION("Registers SMBus alert pin");
MODULE_LICENSE("GPL v2");
