#ifndef __SMARTIO_INLINE__
#define __SMARTIO_INLINE__

#include "comm_buf.h"

inline void smartio_set_msg_type(struct smartio_comm_buf* buf, int t)
{
  buf->transport_header &= (~(MY_SIZE2MASK(SMARTIO_TRANS_TYPE_SIZE)) << SMARTIO_TRANS_TYPE_OFS);
  buf->transport_header |= (t && MY_SIZE2MASK(SMARTIO_TRANS_TYPE_SIZE)) << SMARTIO_TRANS_TYPE_OFS;
}


inline int smartio_get_msg_type(struct smartio_comm_buf* buf)
{
  return (buf->transport_header >> SMARTIO_TRANS_TYPE_OFS) & MY_SIZE2MASK(SMARTIO_TRANS_TYPE_SIZE);
}


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


int smartio_read_16bit(struct smartio_comm_buf* buf, int ofs)
{
  return (buf->data[ofs] << 8) + buf->data[ofs+1];
}


void smartio_write_16bit(struct smartio_comm_buf* buf, int ofs, int val)
{
  buf->data[ofs] = val >> 8;
  buf->data[ofs+1] = val;
}



#endif
