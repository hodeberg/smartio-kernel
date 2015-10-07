#include "smartio.h"
#include "txbuf_list.h"
#include "comm_buf.h"

#define TRANS_ID_BITS (1  << SMARTIO_TRANS_ID_SIZE)
static DECLARE_BITMAP(transId, TRANS_ID_BITS);
static DEFINE_MUTEX(tx_lock);
static LIST_HEAD(transactions);



static int getTransId(void)
{
  int newId;

  newId = find_first_zero_bit(transId, TRANS_ID_BITS);

  if (newId == TRANS_ID_BITS)
  {
    // No free IDs
    pr_err("There are no free transaction IDs\n");
    return -1;
  }
  set_bit(newId, transId);
 
  return newId;
}

static int releaseTransId(int id)
{
  int status = id;

  if (!test_and_clear_bit(id, transId)) {
    // This transaction ID was not claimed.
    pr_err("Trying to release unclaimed transaction ID %d\n", id);
    status = -1;
  }

  return status;
}


/* TBD: add checking for id claim success */
int smartio_add_transaction(struct smartio_comm_buf *comm_buf)
{
  int id;

  mutex_lock(&tx_lock);
  id = getTransId();
  smartio_set_transaction_id(comm_buf, id);
  list_add_tail(&comm_buf->list, &transactions);
  mutex_unlock(&tx_lock);
  return 0;
}





struct smartio_comm_buf *smartio_find_transaction(int respId)
{
  struct smartio_comm_buf *req;
  struct smartio_comm_buf *next;
  struct smartio_comm_buf *result = NULL;

#ifdef DBG_TRANS
  pr_info("Entering %s\n", __FUNCTION__);
#endif
  mutex_lock(&tx_lock);
#ifdef DBG_TRANS
  pr_info("Mutex has been claimed\n");
#endif
  if (list_empty(&transactions)) {
    pr_err("No transactions in list!\n");
    return NULL;
  }

  list_for_each_entry_safe(req, next, &transactions, list) {
#ifdef DBG_TRANS
    pr_info("Inspecting an item in the transaction queue\n");
    pr_info("Type: %d\n", smartio_get_msg_type(req));
#endif
    if (smartio_get_msg_type(req) == SMARTIO_REQUEST) {
      int reqId = smartio_get_transaction_id(req);

#ifdef DBG_TRANS
      pr_info("Found a request in the transaction queue\n");
#endif
      if (reqId == respId) {
#ifdef DBG_TRANS
	pr_info("Matching transaction ID %d\n", respId);
#endif
	list_del(&req->list);
        releaseTransId(respId);
	result = req;
	break;
      }
      else {
#ifdef DBG_TRANS
	pr_debug("Transaction ID mismatch. Resp: %d, Req: %d\n", respId, reqId);
#endif
      }
    }
  }

  mutex_unlock(&tx_lock);
  return result;
}
