#include <linux/module.h>
#include <linux/i2c.h>
#include "smartio.h"
#include "smartio_inline.h"


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
  rx->data_len = 1;
  rx->data[0] = 0;
  //  pr_warn("HAOD: tx header is %x\n", tx->transport_header);
  rx->transport_header = tx->transport_header;
  //pr_warn("HAOD: rx intial header is %x\n", rx->transport_header);
  smartio_set_msg_type(rx, SMARTIO_RESPONSE);
  //pr_warn("HAOD: rx header after set msg is %x\n", rx->transport_header);
  smartio_set_direction(rx, SMARTIO_FROM_NODE);
  //pr_warn("HAOD: rx header after set direction is %x\n", rx->transport_header);
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

#include "convert.h"


struct attr_info {
  uint8_t directions;
  uint8_t arraySize;
  enum smartio_io_types type;
  char *name;
  union val data;
};


char str1[30] = "42";
char str2[30] = "large";

struct attr_info adc_attrs[] = {
  { IO_IS_INPUT, 0, IO_ASCII_STRING, "offset", .data.str = str1 },
  { IO_IS_OUTPUT, 0, IO_PRESSURE_KPA, "pressure", .data.intval = 1000 }
};

struct attr_info adc2_attrs[] = {
  { IO_IS_INPUT, 0, IO_ASCII_STRING, "offset", .data.str = str1 },
  { IO_IS_OUTPUT, 0, IO_PRESSURE_KPA, "pressure", .data.intval = 1000 },
  { IO_IS_INPUT | IO_IS_DEVICE, 0, IO_ENERGY_WH, "dev_energy", .data.intval = 500 }
};

struct attr_info dac_attrs[] = {
  { IO_IS_INPUT, 0, IO_ASCII_STRING, "gain", .data.str = str2 },
  { IO_IS_INPUT | IO_IS_OUTPUT, 0, IO_ENERGY_WH, "energy", .data.intval =2000 },
  { IO_IS_OUTPUT | IO_IS_DEVICE, 0, IO_ENERGY_WH, "dev_energy", .data.intval = 500 }
};

struct attr_info node_attrs[] = {
  { IO_IS_INPUT | IO_IS_OUTPUT, 3, IO_ASCII_CHAR, "en", .data.intval = 1 }
};


struct module_info {
  char* name;
  int no_of_attrs;
  struct attr_info  *attrs;
};

struct module_info modules[] = {
  { "smartio-i2c-hod", ARRAY_SIZE(node_attrs), node_attrs },
  { "adc", ARRAY_SIZE(adc_attrs), adc_attrs },
  { "adc", ARRAY_SIZE(adc2_attrs), adc2_attrs },
  { "dac", ARRAY_SIZE(dac_attrs), dac_attrs },
  { "dac", ARRAY_SIZE(dac_attrs), dac_attrs },
  { "dac", ARRAY_SIZE(dac_attrs), dac_attrs }
};

struct reply {
  int len;
  char data[40];
};

struct reply deferred_reply;


static void write_val_to_attr(const void* indata, struct attr_info* attr)
{
  pr_info("attr name: %s, attr type: %d\n", attr->name, attr->type);
  if (attr->type == IO_ASCII_STRING) {
    pr_info("storing string: %s\n", (char *) indata);
    strcpy(attr->data.str, indata);
  }
  else
    attr->data.intval = smartio_buf2value(attr->type, indata);
}


static int communicate(struct smartio_node* this, 
		     struct smartio_comm_buf* tx,
		     struct smartio_comm_buf* rx)
{
  int ix = tx->data[0];
  int attr_ix;
  int array_ix;
  int len;
  struct attr_info *attr = NULL;
  char* module_name = modules[ix].name;

  dev_warn(&this->dev, "HAOD: calling communicate() function at %p\n", &communicate);
  fill_reply_buffer(rx, tx);

  switch (tx->data[1]) {
  case SMARTIO_GET_NO_OF_MODULES:
    if (ix != 0) {
      dev_err(&this->dev, "Illegal module index %d\n", ix);
      rx->data[0] = SMARTIO_ILLEGAL_MODULE_INDEX;
      return -1;
    }
    rx->data_len = 3 + strlen(module_name) + 1;
    smartio_write_16bit(rx, 1, ARRAY_SIZE(modules));
    strcpy(rx->data + 3, module_name);
    break;
  case SMARTIO_GET_NO_OF_ATTRIBUTES:
    if ((ix < 0) || (ix >= ARRAY_SIZE(modules))) {
      dev_err(&this->dev, "Illegal module index %d\n", ix);
      rx->data[0] = SMARTIO_ILLEGAL_MODULE_INDEX;
      return -1;
    }
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
    attr_ix = smartio_read_16bit(tx, 2);

    if (!((attr_ix >= 0) && (attr_ix < modules[ix].no_of_attrs))) {
      rx->data[0] = SMARTIO_ILLEGAL_ATTRIBUTE_INDEX;
      dev_err(&this->dev, "Illegal attribute index %d\n", attr_ix);
      return -1;
    }
    attr = &modules[ix].attrs[attr_ix];
    rx->data[1] = attr->directions;
    rx->data[2] = attr->arraySize;
    rx->data[3] = attr->type;
    strcpy(rx->data + 4, attr->name);
    rx->data_len = 4 + strlen(attr->name) + 1;
    break;
  case SMARTIO_GET_ATTR_VALUE:
    if ((ix < 0) || (ix >= ARRAY_SIZE(modules))) {
      dev_err(&this->dev, "Illegal module index %d\n", ix);
      rx->data[0] = SMARTIO_ILLEGAL_MODULE_INDEX;
      return -1;
    }
    attr_ix = smartio_read_16bit(tx, 2);
    if (!((attr_ix >= 0) && (attr_ix < modules[ix].no_of_attrs))) {
      rx->data[0] = SMARTIO_ILLEGAL_ATTRIBUTE_INDEX;
      dev_err(&this->dev, "Illegal attribute index %d\n", attr_ix);
      return -1;
    }
    array_ix = tx->data[4];
    if (array_ix != 0xFF) {
      rx->data[0] = SMARTIO_ILLEGAL_ARRAY_INDEX;
      dev_err(&this->dev, "Illegal array index %d\n", array_ix);
      return -1;
    }
    attr = &modules[ix].attrs[attr_ix];
    if (attr->directions & IO_IS_DEVICE) {
      int no_of_words = (SMARTIO_DATA_SIZE-1) / 2;
      int i;
      static int cur_val = 0;
      uint8_t *p = rx->data + 1;

      pr_info("Received device read request. Starting at %d\n", cur_val);
      pr_info("no_of_words: %d\n", no_of_words);
#if 1
      for (i=0; i < no_of_words; i++) {
	*p++ = cur_val >> 8;
	*p++ = cur_val++;
      }
      rx->data_len = SMARTIO_DATA_SIZE;
#else
      *p++ = cur_val >> 8;
      *p++ = cur_val++;      
      *p++ = cur_val >> 8;
      *p++ = cur_val++;      
      rx->data_len = 5;
#endif
    }
    else {
      write_val_to_buffer(rx->data+1, &len, attr->type, attr->data);
      rx->data_len = len + 1;
    }
    break;
  case SMARTIO_SET_ATTR_VALUE:
    if ((ix < 0) || (ix >= ARRAY_SIZE(modules))) {
      dev_err(&this->dev, "Illegal module index %d\n", ix);
      rx->data[0] = SMARTIO_ILLEGAL_MODULE_INDEX;
      return -1;
    }
    attr_ix = smartio_read_16bit(tx, 2);
    if (!((attr_ix >= 0) && (attr_ix < modules[ix].no_of_attrs))) {
      rx->data[0] = SMARTIO_ILLEGAL_ATTRIBUTE_INDEX;
      dev_err(&this->dev, "Illegal attribute index %d\n", attr_ix);
      return -1;
    }
    array_ix = tx->data[4];
    if (array_ix != 0xFF) {
      rx->data[0] = SMARTIO_ILLEGAL_ARRAY_INDEX;
      dev_err(&this->dev, "Illegal array index %d\n", array_ix);
      return -1;
    }
    attr = &modules[ix].attrs[attr_ix];
    if (attr->directions & IO_IS_DEVICE) {
      int data_bytes = tx->data_len;
      int cur_ofs = 5;
  
      pr_info("Received device packet.\n");
      while (cur_ofs < data_bytes) {
	pr_info("%x ", (tx->data[cur_ofs] << 8) | tx->data[cur_ofs+1]);
	cur_ofs += 2;
      }
      pr_info("\n");
    }
    else 
      write_val_to_attr(tx->data+5, attr);
    rx->data_len = 1;
    break;
  case SMARTIO_GET_STRING:
    dev_err(&this->dev, "HAOD: communicate(): command %d not implemented\n", (int) tx->data[1]);
    break;
  }
  print_hex_dump_bytes("Comm:", DUMP_PREFIX_OFFSET, rx->data, rx->data_len);
  return 0;
} 


static int my_probe(struct i2c_client* client,
		    const struct i2c_device_id *id)
{
  int status;

  pr_info("Probing smart i2c driver for device %s\n", dev_name(&client->dev));

  //  status = devm_smartio_register_node(&client->dev);
  status = dev_smartio_register_node(&client->dev, "smartio-i2c", communicate);
  
  pr_warn("Probe status was %d\n", status);
  return status; 
}

static int my_remove(struct i2c_client* client)
{
  int status;

  pr_info("Removing smart i2c driver\n");
  status = device_for_each_child(&client->dev, NULL, smartio_unregister_node);
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