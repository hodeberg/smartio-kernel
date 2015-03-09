#include <linux/module.h>
#include "smartio.h"


static const struct smartio_device_id my_idtable[] = {
  { "adc" },
  { "dac" },
  { NULL }
};


MODULE_DEVICE_TABLE(smartio, my_idtable);



static struct smartio_function_driver my_driver = {
  .driver = {
    .name = "smartio_adc",
  },
  .id_table = my_idtable,
};

static int __init my_init(void)
{
  int status;

  status = smartio_add_driver(&my_driver);
  pr_warn("Done registering smartio adc driver\n");
  return status;
}
module_init(my_init);

static void __exit my_cleanup(void)
{
  smartio_del_driver(&my_driver);
  pr_warn("Removed smartio adc driver\n");
}
module_exit(my_cleanup);



MODULE_AUTHOR("Hans Odeberg <hans.odeberg@intel.com>");
MODULE_DESCRIPTION("Sample smartio adc driver");
MODULE_LICENSE("GPL v2");
