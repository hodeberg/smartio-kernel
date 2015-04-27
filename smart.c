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
  //  pr_warn("HAOD: tx header is %x\n", tx->transport_header);
  rx->transport_header = tx->transport_header;
  //pr_warn("HAOD: rx intial header is %x\n", rx->transport_header);
  smartio_set_msg_type(rx, SMARTIO_RESPONSE);
  //pr_warn("HAOD: rx header after set msg is %x\n", rx->transport_header);
  smartio_set_direction(rx, SMARTIO_FROM_NODE);
  //pr_warn("HAOD: rx header after set direction is %x\n", rx->transport_header);
}
	
static void smartio_write_16bit(struct smartio_comm_buf* buf, int ofs, int val)
{
  buf->data[ofs] = val >> 8;
  buf->data[ofs+1] = val;
}

static int smartio_read_16bit(struct smartio_comm_buf* buf, int ofs)
{
  return  (buf->data[ofs] << 8) | buf->data[ofs+1];
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



struct attr_info {
  uint8_t directions;
  uint8_t arraySize;
  enum smartio_io_types type;
  char *name;
  union {
    int val;
    char* str;
  };
};

struct attr_info adc_attrs[] = {
  { IO_IS_INPUT, 0, IO_STRING, "offset", {.str = "42"} },
  { IO_IS_OUTPUT, 0, IO_TEMP_C, "temp", {.val = 1000} }
};

struct attr_info dac_attrs[] = {
  { IO_IS_INPUT, 0, IO_STRING, "gain", {.str = "large"} },
  { IO_IS_INPUT | IO_IS_OUTPUT, 0, IO_TEMP_C, "temp", {.val =2000} },
  { IO_IS_INPUT | IO_IS_DEVICE, 0, IO_TEMP_K, "dev_temp", {.val = 500} }
};

struct attr_info node_attrs[] = {
  { IO_IS_INPUT | IO_IS_OUTPUT, 3, IO_INT32, "en", {.val = 1} }
};


struct module_info {
  char* name;
  int no_of_attrs;
  struct attr_info  *attrs;
};

struct module_info modules[] = {
  { "smartio-i2c-hod", ARRAY_SIZE(node_attrs), node_attrs },
  { "adc", ARRAY_SIZE(adc_attrs), adc_attrs },
  { "dac", ARRAY_SIZE(dac_attrs), dac_attrs }
};

struct reply {
  int len;
  char data[40];
};

struct reply deferred_reply;


static int communicate(struct smartio_node* this, 
		     struct smartio_comm_buf* tx,
		     struct smartio_comm_buf* rx)
{
  int ix = tx->data[0];
  int attr_ix;
  struct attr_info *attr = NULL;
  char* module_name = modules[ix].name;
  dev_warn(&this->dev, "HAOD: calling communicate() function\n");
  switch (tx->data[1]) {
  case SMARTIO_GET_NO_OF_MODULES:
    if (ix != 0) {
      dev_err(&this->dev, "Illegal module index %d\n", ix);
      return -1;
    }
    fill_reply_buffer(rx, tx);
    rx->data_len = 3 + strlen(module_name) + 1;
    rx->data[0] = 0;
    smartio_write_16bit(rx, 1, 3);
    strcpy(rx->data + 3, module_name);
    break;
  case SMARTIO_GET_NO_OF_ATTRIBUTES:
    if ((ix < 0) || (ix >= ARRAY_SIZE(modules))) {
      dev_err(&this->dev, "Illegal module index %d\n", ix);
      rx->data[0] = SMARTIO_ILLEGAL_MODULE_INDEX;
      return -1;
    }
    else
      rx->data[0] = 0;
    fill_reply_buffer(rx, tx);
    rx->data_len = 3 + strlen(modules[ix].name) + 1;
    smartio_write_16bit(rx, 1, modules[ix].no_of_attrs);
    strcpy(rx->data + 3, module_name);
    dev_warn(&this->dev, "Communicate: module ix =  %d\n", ix);
    dev_warn(&this->dev, "Communicate: no of attrs =  %d\n", modules[ix].no_of_attrs);
    dev_warn(&this->dev, "Communicate: name =  %s\n", modules[ix].name);
    break;
  case SMARTIO_GET_ATTRIBUTE_DEFINITION:
    if ((ix < 0) || (ix >= ARRAY_SIZE(modules))) {
      dev_err(&this->dev, "Illegal module index %d\n", ix);
      rx->data[0] = SMARTIO_ILLEGAL_MODULE_INDEX;
      return -1;
    }
    else {
      attr_ix = smartio_read_16bit(tx, 2);

      if (!((attr_ix >= 0) && (attr_ix < modules[ix].no_of_attrs))) {
	rx->data[0] = SMARTIO_ILLEGAL_ATTRIBUTE_INDEX;
	return -1;
      }
      else
	rx->data[0] = 0;
    }
    fill_reply_buffer(rx, tx);
    attr = &modules[ix].attrs[attr_ix];
    rx->data[1] = attr->directions;
    rx->data[2] = attr->arraySize;
    rx->data[3] = attr->type;
    strcpy(rx->data + 4, attr->name);
    rx->data_len = 4 + strlen(attr->name) + 1;
    break;
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
  smartio_unregister_node(&client->dev);
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
