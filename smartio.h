#include <linux/device.h>


// Put the following in include/linux/mod_devicetable.h
/* smartio */ 
#define SMARTIO_NAME_SIZE 20
#define SMARTIO_MODULE_PREFIX "smartio:"


struct smartio_device_id {
  char *name;
};
// End of what to put in mod_devicetable.h

// Initial dummy wrapper, just in case we would want to add
// additional device-specific data.
struct smartio_device {
  struct device dev;
};

struct smartio_comm_buf;

struct smartio_function_driver {
	/* Standard driver model interfaces */
	int (*probe)(struct smartio_device *, const struct smartio_device_id *);
	int (*remove)(struct smartio_device *);

	/* driver model interfaces that don't relate to enumeration  */
	void (*shutdown)(struct smartio_device *);
	int (*suspend)(struct smartio_device *, pm_message_t mesg);
	int (*resume)(struct smartio_device *);

	struct device_driver driver;
	const struct smartio_device_id *id_table;
};
#define to_smartio_function_driver(d) container_of(d, struct smartio_function_driver, driver)

int smartio_add_driver(struct smartio_function_driver* sd);
void smartio_del_driver(struct smartio_function_driver* sd);

struct smartio_node {
  struct device dev;
  struct list_head list;
  int nr; // Instance number
  // Send a message, and receive one.
  // tx may be null, in which case the remote node is polled.
  // rx may be empty, if remote node returned no data.
  int (*communicate)(struct smartio_node* this, 
		     struct smartio_comm_buf* tx,
		     struct smartio_comm_buf* rx);
};



int devm_smartio_register_node(struct device *dev);
int dev_smartio_register_node(struct device *dev, 
			      char* name,
			      int (*communicate)(struct smartio_node* this, 
						 struct smartio_comm_buf* tx,
						 struct smartio_comm_buf* rx));
/* dev: the encapsulating hardware device (i2c, spi, etc...) */
void smartio_unregister_node(struct device *dev);

#define SMARTIO_HEADER_SIZE (1 + 1)
#define SMARTIO_DATA_SIZE 30

int smartio_get_no_of_modules(struct smartio_node* node, char* name);

enum smartio_cmds {
  SMARTIO_GET_NO_OF_MODULES = 1,
  SMARTIO_GET_NO_OF_ATTRIBUTES,
  SMARTIO_GET_ATTRIBUTE_DEFINITION,
  SMARTIO_GET_ATTR_VALUE,
  SMARTIO_SET_ATTR_VALUE,
  SMARTIO_GET_STRING,
};

enum smartio_io_types {
  IO_STRING,
  IO_TEMP_C,
  IO_TEMP_K,
  IO_INT8,
  IO_INT16,
  IO_INT32,
  /* To be continued */
};

enum smartio_status {
  SMARTIO_SUCCESS,
  SMARTIO_ILLEGAL_MODULE_INDEX,
  SMARTIO_ILLEGAL_ATTRIBUTE_INDEX,
};

/* The attribute definition */
#define IO_IS_INPUT 0x80
#define IO_IS_OUTPUT 0x40
#define IO_IS_DEVICE 0x20


struct smartio_comm_buf {
  struct list_head list;
  uint8_t data_len;
  uint8_t msg_type;
  uint8_t transport_header;
#if 0
  uint8_t module;
  uint8_t command;
  uint16_t attr_index;
#endif
  uint8_t data[SMARTIO_DATA_SIZE];
};


/* Transaction header is a byte with the following bits:
ID: 3 bits
Direction: 1 bit
Type: 2 bits */

#define MY_SIZE2MASK(x) ((1<<(x)) - 1)
#define SMARTIO_TRANS_ID_SIZE 3

#define SMARTIO_TRANS_DIR_OFS SMARTIO_TRANS_ID_SIZE
#define SMARTIO_TRANS_DIR_SIZE 1


#define SMARTIO_TRANS_TYPE_OFS (SMARTIO_TRANS_ID_SIZE + SMARTIO_TRANS_DIR_SIZE)
#define SMARTIO_TRANS_TYPE_SIZE 2

#define SMARTIO_REQUEST 0
#define SMARTIO_RESPONSE 1
#define SMARTIO_ACKNOWLEDGED 2
#define SMARTIO_UNACKNOWLEDGED 3

inline void smartio_set_msg_type(struct smartio_comm_buf* buf, int t)
{

  buf->transport_header &= (~(MY_SIZE2MASK(SMARTIO_TRANS_TYPE_SIZE)) << SMARTIO_TRANS_TYPE_OFS);
  buf->transport_header |= (t && MY_SIZE2MASK(SMARTIO_TRANS_TYPE_SIZE)) << SMARTIO_TRANS_TYPE_OFS;
}

inline int smartio_get_msg_type(struct smartio_comm_buf* buf)
{
  return (buf->transport_header >> SMARTIO_TRANS_TYPE_OFS) & MY_SIZE2MASK(SMARTIO_TRANS_TYPE_SIZE);
}

#define SMARTIO_TO_NODE 1
#define SMARTIO_FROM_NODE 0
inline void smartio_set_direction(struct smartio_comm_buf* buf, int d)
{
  buf->transport_header &= (~(MY_SIZE2MASK(SMARTIO_TRANS_DIR_SIZE)) << SMARTIO_TRANS_DIR_OFS);
  buf->transport_header |= (d && MY_SIZE2MASK(SMARTIO_TRANS_DIR_SIZE)) << SMARTIO_TRANS_DIR_OFS;
}

inline int smartio_get_direction(struct smartio_comm_buf* buf)
{
  return (buf->transport_header >> SMARTIO_TRANS_DIR_OFS) & MY_SIZE2MASK(SMARTIO_TRANS_DIR_SIZE);
}

inline void smartio_set_transaction_id(struct smartio_comm_buf* buf, int d)
{
  buf->transport_header &= ~MY_SIZE2MASK(SMARTIO_TRANS_ID_SIZE);
  buf->transport_header |= d && MY_SIZE2MASK(SMARTIO_TRANS_ID_SIZE);
}

inline int smartio_get_transaction_id(struct smartio_comm_buf* buf)
{
  return buf->transport_header & MY_SIZE2MASK(SMARTIO_TRANS_ID_SIZE);
}

