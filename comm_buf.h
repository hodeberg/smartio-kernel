#ifndef __SMARTIO_COMM_BUF__
#define __SMARTIO_COMM_BUF__

#include <linux/list.h>

enum smartio_cmds {
  SMARTIO_GET_NO_OF_MODULES = 1,
  SMARTIO_GET_NO_OF_ATTRIBUTES,
  SMARTIO_GET_ATTRIBUTE_DEFINITION,
  SMARTIO_GET_ATTR_VALUE,
  SMARTIO_SET_ATTR_VALUE,
  SMARTIO_GET_STRING,
};

#define SMARTIO_DATA_SIZE 31

struct smartio_comm_buf;

typedef void (*smartio_tx_completion_cb)(struct smartio_comm_buf *req,
					 struct smartio_comm_buf *resp,
					 void *data);

struct smartio_comm_buf {
  struct list_head list;
  void *cb_data;
  smartio_tx_completion_cb cb;
  uint8_t data_len;
  uint8_t msg_type;
  uint8_t transport_header;
#if 0
  uint8_t module;
  uint8_t command;
  uint16_t attr_index;
  uint8_t array_index;
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

void smartio_set_msg_type(struct smartio_comm_buf* buf, int t);
int smartio_get_msg_type(struct smartio_comm_buf* buf);

#define SMARTIO_TO_NODE 1
#define SMARTIO_FROM_NODE 0
void smartio_set_direction(struct smartio_comm_buf* buf, int d);
int smartio_get_direction(struct smartio_comm_buf* buf);
void smartio_set_transaction_id(struct smartio_comm_buf* buf, int d);
int smartio_get_transaction_id(struct smartio_comm_buf* buf);

void fillbuf_get_attr_value(struct smartio_comm_buf *buf, int fcn, int attr, int array);

int smartio_read_16bit(struct smartio_comm_buf* buf, int ofs);
void smartio_write_16bit(struct smartio_comm_buf* buf, int ofs, int val);


#endif
