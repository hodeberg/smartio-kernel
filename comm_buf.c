#include "comm_buf.h"



void fillbuf_get_attr_value(struct smartio_comm_buf *buf, int fcn, int attr, int array)
{
    buf->data_len = 5; // module + command + attr ix + array ix
    buf->data[0] = fcn;
    buf->data[1] =  SMARTIO_GET_ATTR_VALUE;
    smartio_write_16bit(buf, 2, attr);
    buf->data[4] = array;
}
