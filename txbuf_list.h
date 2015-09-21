#ifndef __TXBUF_LIST_H__
#define __TXBUF_LIST_H__


struct smartio_comm_buf *smartio_find_transaction(int respId);
void smartio_add_transaction(struct smartio_comm_buf *comm_buf);

#endif
