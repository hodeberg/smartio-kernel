#include <linux/module.h>
#include <linux/i2c.h>
#include "smartio.h"


static struct i2c_device_id my_idtable[] = {
  { "smart1", 10 },
  { "smart2", 15 },
  {}
};

struct smart_adapter {
  struct device *dev;
  // More fields later...
};

MODULE_DEVICE_TABLE(i2c, my_idtable);


static void fill_reply_buffer(struct smartio_comm_buf* rx,
			      const struct smartio_comm_buf* tx)
{
  rx->data_len = 0;
  pr_warn("HAOD: tx header is %x\n", tx->transport_header);
  rx->transport_header = tx->transport_header;
  pr_warn("HAOD: rx intial header is %x\n", rx->transport_header);
  smartio_set_msg_type(rx, SMARTIO_RESPONSE);
  pr_warn("HAOD: rx header after set msg is %x\n", rx->transport_header);
  smartio_set_direction(rx, SMARTIO_FROM_NODE);
  pr_warn("HAOD: rx header after set direction is %x\n", rx->transport_header);
}
	
static void smartio_write_16bit(struct smartio_comm_buf* buf, int ofs, int val)
{
  buf->data[ofs] = val >> 8;
  buf->data[ofs+1] = val;
}
		     

/* This is a dummy communicate(), which does not touch any hardware.
   When asked about its # of modules, it immediately replies that
   it has two modules (plus module 0, so three modules).
   For all other questions, the reply will be deferred to a later
   poll request.
   Here is the list of replies:
   Req: GET_NO_OF_MODULES
   Resp: 3
   SMARTIO_GET_NO_OF_ATTRIBUTES(0): 2
   SMARTIO_GET_NO_OF_ATTRIBUTES(1): 3
   SMARTIO_GET_NO_OF_ATTRIBUTES(1): 4
   SMARTIO_GET_ATTRIBUTE_DEFINITION(0,0): TBD
  SMARTIO_GET_ATTR_VALUE,
  SMARTIO_SET_ATTR_VALUE,
  SMARTIO_GET_STRING,

 */
static int communicate(struct smartio_node* this, 
		     struct smartio_comm_buf* tx,
		     struct smartio_comm_buf* rx)
{
  dev_warn(&this->dev, "HAOD: calling communicate() function\n");
  switch (tx->data[1]) {
  case SMARTIO_GET_NO_OF_MODULES:
    fill_reply_buffer(rx, tx);
    rx->data_len = 3;
    rx->data[0] = 0;
    smartio_write_16bit(rx, 1, 3);
    break;

  case SMARTIO_GET_NO_OF_ATTRIBUTES:
  case SMARTIO_GET_ATTRIBUTE_DEFINITION:
  case SMARTIO_GET_ATTR_VALUE:
  case SMARTIO_SET_ATTR_VALUE:
  case SMARTIO_GET_STRING:
    dev_err(&this->dev, "HAOD: communicate(): command %d not implemented\n", (int) tx->data[1]);
    break;
  }
  return 0;
}

static int my_probe(struct i2c_client* client,
		    const struct i2c_device_id *id)
{
  int status;

  pr_warn("Probing smart i2c driver\n");
  pr_warn("Probing smart i2c driver again\n");

  //  status = devm_smartio_register_node(&client->dev);
  status = dev_smartio_register_node(&client->dev, "smartio-i2c", communicate);
  
  pr_warn("Probe status was %d\n", status);
  return status; 
}

static int my_remove(struct i2c_client* client)
{
  pr_warn("Removing smart i2c driver\n");
  smartio_unregister_node(dev_get_drvdata(&client->dev));
  return 0;
}


static struct i2c_driver my_driver = {
  .driver = {
    .name = "smart",
  },
  .id_table = my_idtable,
  .probe = my_probe,
  .remove = my_remove
};

static int __init my_init(void)
{
  int status;

  status = i2c_add_driver(&my_driver);
  pr_warn("Done registering smart i2c driver\n");
  return status;
}
module_init(my_init);

static void __exit my_cleanup(void)
{
  i2c_del_driver(&my_driver);
  pr_warn("Removed smart i2c driver\n");
}
module_exit(my_cleanup);




MODULE_AUTHOR("Hans Odeberg <hans.odeberg@intel.com>");
MODULE_DESCRIPTION("Sample IO driver");
MODULE_LICENSE("GPL v2");
