#include "smartio.h"
#include "txbuf_list.h"

#define TRANS_ID_BITS (1  << SMARTIO_TRANS_ID_SIZE)
static DECLARE_BITMAP(transId, 1 << TRANS_ID_BITS);
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



void smartio_add_transaction(struct smartio_comm_buf *comm_buf)
{
  mutex_lock(&tx_lock);
  smartio_set_transaction_id(comm_buf, getTransId());
  list_add_tail(&comm_buf->list, &transactions);
  mutex_unlock(&tx_lock);
}





struct smartio_comm_buf *smartio_find_transaction(int respId)
{
  struct smartio_comm_buf *req;
  struct smartio_comm_buf *next;
  struct smartio_comm_buf *result = NULL;

  pr_info("Entering %s\n", __FUNCTION__);
  mutex_lock(&tx_lock);
  pr_info("Mutex has been claimed\n");
  if (list_empty(&transactions)) {
    pr_err("No transactions in list!\n");
    return NULL;
  }

  list_for_each_entry_safe(req, next, &transactions, list) {
    pr_info("Inspecting an item in the transaction queue\n");
    pr_info("Type: %d\n", smartio_get_msg_type(req));
    if (smartio_get_msg_type(req) == SMARTIO_REQUEST) {
      int reqId = smartio_get_transaction_id(req);

      pr_info("Found a request in the transaction queue\n");
      if (reqId == respId) {
	pr_info("Matching transaction ID %d\n", respId);
	list_del(&req->list);
        releaseTransId(respId);
	result = req;
	break;
      }
      else
	pr_debug("Transaction ID mismatch. Resp: %d, Req: %d\n", respId, reqId);
    }
  }

  mutex_unlock(&tx_lock);
  return result;
}