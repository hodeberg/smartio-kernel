#include <linux/device.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/workqueue.h>

#include <linux/idr.h>
#include <linux/slab.h>
#include "smartio.h"

int smartio_match(struct device* dev, struct device_driver* drv)
{
  struct smartio_function_driver* driver = container_of(drv, struct smartio_function_driver, driver);
  const struct smartio_device_id* drv_id;

  dev_warn(dev, "Matching this dev to %s\n", drv->name);  
  for(drv_id = driver->id_table; drv_id->name != NULL; drv_id++) {
    if (!strcmp(dev_name(dev), drv_id->name)) {
      dev_warn(dev, "Match of %s succeeded\n", drv_id->name);
      return 1;
    }
    else {
      dev_warn(dev, "No match for %s\n", drv_id->name);
    }
  }
  return 0;
}

static int my_probe(struct device* dev)
{
  int status = 0;

  pr_warn("Bus probe for function  driver %s\n", dev_name(dev));
  return status; 
}

static int my_remove(struct device* dev)
{
  pr_warn("Bus remove for function driver\n");
  return 0;
}

static void function_release(struct device* dev)
{
	struct smartio_device *fcn;

	dev_warn(dev, "Smartio function release called\n");	
	fcn = container_of(dev, struct smartio_device, dev);
	/* Resource counting: parent and child should reference
	   each other. Thus, we want to decrement the parent's
	   count on the child, and the child's count on the
	   parent. Q: what of this does the device framework
	   do? */
	kfree(dev);
}

static struct  bus_type smartio_bus = {
  .name = "smartio",
  .dev_name = "function",
  .match = smartio_match,
  .probe = my_probe,
  .remove = my_remove,
};

static struct class smartio_function_class = {
  .name = "smartio_function",
  .owner = THIS_MODULE,
  .dev_release = function_release
};


// The workqueue used for all smartio work
static struct workqueue_struct *work_queue;

struct smartio_work {
  struct work_struct work;
  struct smartio_comm_buf *comm_buf;
  struct smartio_node* node; 
};

// TBD Add functions to add devices (nodes, functions) and register drivers
static DEFINE_MUTEX(core_lock);
static DEFINE_MUTEX(id_lock);
static DEFINE_IDR(node_idr);
static LIST_HEAD(smartio_node_list);
static LIST_HEAD(transactions);
static DECLARE_WAIT_QUEUE_HEAD(wait_queue);

#define TRANS_ID_BITS (1  << SMARTIO_TRANS_ID_SIZE)
DECLARE_BITMAP(transId, 1 << TRANS_ID_BITS);

static int getTransId(void)
{
  int newId;

  mutex_lock(&id_lock);
  newId = find_first_zero_bit(transId, TRANS_ID_BITS);

  if (newId == TRANS_ID_BITS)
  {
    // No free IDs
    newId = -1;
    goto cleanup;
  }
  set_bit(newId, transId);
 
 cleanup:
  mutex_unlock(&id_lock);
  return newId;
}

static int releaseTransId(int id)
{
  int status = id;

  mutex_lock(&id_lock);

  if (!test_and_clear_bit(id, transId)) {
    // This transaction ID was not claimed.
    status = -1;
  }

  mutex_unlock(&id_lock);
  return status;
}


static void handle_response(struct device* dev, struct smartio_comm_buf *resp)
{
  struct smartio_comm_buf *req;
  struct smartio_comm_buf *next;
  dev_warn(dev, "Entering handle_response\n");
  mutex_lock(&core_lock);
  dev_warn(dev, "Mutex is locked\n");

  if (list_empty(&transactions)) {
    dev_err(dev, "No transactions in list!\n");
    return;
  }

  list_for_each_entry_safe(req, next, &transactions, list) {
    dev_warn(dev, "Found an item in the transaction queue\n");
    dev_warn(dev, "Type: %d\n", smartio_get_msg_type(req));
    if (smartio_get_msg_type(req) == SMARTIO_REQUEST) {
      int reqId = smartio_get_transaction_id(req);
      int respId = smartio_get_transaction_id(resp);

      dev_warn(dev, "Found a response in the transaction queue\n");
      if (reqId == respId) {
	dev_warn(dev, "Matching transaction ID %d\n", respId);
	list_del(&req->list);
        releaseTransId(respId);
	req->data_len = resp->data_len;
	memcpy(req->data, resp->data, resp->data_len);
	dev_warn(dev, "Copied %d bytes from resp to req\n", req->data_len);
	smartio_set_direction(req, SMARTIO_FROM_NODE);
	wake_up_interruptible(&wait_queue);
	break;
      }
      else
	dev_warn(dev, "Transaction ID mismatch. Resp: %d, Req: %d\n", respId, reqId);
    }
  }
  mutex_unlock(&core_lock);
}

static int talk_to_node(struct smartio_node *node, 
			struct smartio_comm_buf *tx)
{
  struct smartio_comm_buf rx;
  int status;

  status = node->communicate(node, tx, &rx);

  if (rx.data_len > 0) {
    int msg_type = smartio_get_msg_type(&rx);

    switch (msg_type) {
    case SMARTIO_RESPONSE:
      dev_err(&node->dev, "Got a response message\n");
      handle_response(&node->dev, &rx);
      break;
    case SMARTIO_REQUEST:
    case SMARTIO_ACKNOWLEDGED:
    case SMARTIO_UNACKNOWLEDGED:
      dev_err(&node->dev, "Message type %d not implemented yet\n", msg_type);
      status = -1;
      break;
    }
  }
  
  return status;
}



static void wq_fcn_post_message(struct work_struct *w)
{
  struct smartio_work *my_work = container_of(w, struct smartio_work, work);

  pr_warn("HAOD: request work function\n");
  mutex_lock(&core_lock);
  list_add_tail(&my_work->comm_buf->list, &transactions);
  mutex_unlock(&core_lock);
  talk_to_node(my_work->node, my_work->comm_buf);
  kfree(my_work);
}


static int transaction_done(struct smartio_comm_buf *buf)
{
  pr_warn("HAOD: wakeup cond is %s\n",
	  (smartio_get_direction(buf) == SMARTIO_FROM_NODE) ?
	  "FROM_NODE" : "TO_NODE");
  return smartio_get_direction(buf) == SMARTIO_FROM_NODE;
}


static int post_request(struct smartio_node* node,
			struct smartio_comm_buf* buf)
{
  struct smartio_work *my_work;
  int status;

  // Set the transaction header
  smartio_set_msg_type(buf, SMARTIO_REQUEST);
  smartio_set_direction(buf, SMARTIO_TO_NODE);
  smartio_set_transaction_id(buf, getTransId());

  // Post deferred work
  my_work = kmalloc(sizeof *my_work, GFP_KERNEL);
  if (!my_work) {
    dev_err(&node->dev, "No memory for work item\n");
    return -ENOMEM;
  }

  INIT_WORK(&my_work->work, wq_fcn_post_message);
  my_work->comm_buf = buf;
  my_work->node = node;
  pr_warn("HAOD: before queueing work\n");
  status = queue_work(work_queue, &my_work->work);
  pr_warn("HAOD: after queueing work\n");
  if (!status) {
    pr_err("HAOD: failed to queue work\n");
    kfree(my_work);
    return -ENOMEM; // TBD better error code
  }
  status = wait_event_interruptible(wait_queue, transaction_done(buf));
  if (status == 0) {
    pr_warn("HAOD: woke up after request\n");
  }
  else {
    pr_warn("HAOD: Received a signal\n");
  }
  return status;
}



static int smartio_read_16bit(struct smartio_comm_buf* buf, int ofs)
{
  return (buf->data[ofs] << 8) + buf->data[ofs+1];
}

#if 0
static void smartio_write_16bit(struct smartio_comm_buf* buf, int ofs, int val)
{
  buf->data[ofs] = val >> 8;
  buf->data[ofs+1] = val;
}
#endif

int smartio_get_no_of_modules(struct smartio_node* node, char *name)
{
  struct smartio_comm_buf* buf;
  int status;

  buf = kzalloc(sizeof *buf, GFP_KERNEL);
  if (!buf) 
    return -ENOMEM;

  buf->data_len = 2; // module + command
  buf->data[0] = 0;
  buf->data[1] = SMARTIO_GET_NO_OF_MODULES;
  status = post_request(node, buf);
  if (status < 0)
    return status;

  if (buf->data_len <= 3)
    return -ENOMEM; // TBD: err to indicate wrong data size
  strncpy(name, buf->data+3, SMARTIO_NAME_SIZE);
  name[SMARTIO_NAME_SIZE] = '\0';
  
  return smartio_read_16bit(buf, 1);
}
EXPORT_SYMBOL_GPL(smartio_get_no_of_modules);

#define SOME_LATER_KERNEL_VERSION
static int alloc_new_node_number(struct smartio_node *node)
{
  int id;

#ifdef SOME_LATER_KERNEL_VERSION
  mutex_lock(&core_lock);
  id = idr_alloc(&node_idr, node, 1, 0, GFP_KERNEL);
  mutex_unlock(&core_lock);
#else
  int result = -EAGAIN;
  
  do {
    if (idr_pre_get(&node_idr, GFP_KERNEL) == 0) {
      pr_err("smartio: Failed to alloc idr\n");
      return -1;
    }
    mutex_lock(&core_lock);
    result = idr_get_new(&node_idr, node, &id);
    mutex_unlock(&core_lock);
  } while (result == -EAGAIN);
#endif

  return id;
}

static void smartio_node_release(struct device *dev)
{
#if 0
  struct smartio_node *node;

  node = container_of(dev, struct smartio_node, dev);
#endif
  dev_warn(dev, "Releasing smartio node\n");
  /* Consider reference counting of i2c vs node pointers */
  kfree(dev);
}

static struct class smartio_node_class = {
  .name = "smartio_node",
  .owner = THIS_MODULE,
  .dev_release = smartio_node_release
};


int smartio_get_function_info(struct smartio_node* node, 
			      int module,
			      int *no_of_attrs,
			      char *name)
{
  struct smartio_comm_buf* buf;
  int status;

  buf = kzalloc(sizeof *buf, GFP_KERNEL);
  if (!buf) 
    return -ENOMEM;

  buf->data_len = 2; // module + command
  buf->data[0] = module;
  buf->data[1] = SMARTIO_GET_NO_OF_ATTRIBUTES;
  status = post_request(node, buf);
  if (status < 0) {
    dev_err(&node->dev, "DARN DARN DARN\n");
    return status;
  }

  if (buf->data_len <= 3) {
    dev_err(&node->dev, "DARN DARN DARN 2\n");
    return -ENOMEM; // TBD: err to indicate wrong data size
  }
  if (buf->data[0]) {
    dev_err(&node->dev, "DARN DARN DARN msg status was %d\n", buf->data[0]);
    return -ENOMEM; // TBD: err to indicate wrong module index
  }

  *no_of_attrs = smartio_read_16bit(buf, 1);
  strncpy(name, buf->data+3, SMARTIO_NAME_SIZE);
  name[SMARTIO_NAME_SIZE] = '\0';
  
  return 0;
}
EXPORT_SYMBOL_GPL(smartio_get_function_info);

static int smartio_register_node(struct device *dev, struct smartio_node *node, char *name)
{
  int status;
  int no_of_modules;
  int i;
  char node_name[SMARTIO_NAME_SIZE+1];

  dev_warn(dev, "HAOD: entering register_node\n");
  device_initialize(&node->dev);
  node->dev.parent = dev;
  node->dev.class = &smartio_node_class;
  node->nr = alloc_new_node_number(node);
  dev_warn(dev, "HAOD: allocated node number %d\n", node->nr);
  if (node->nr < 0)
    return node->nr;
  // TBD: Use name of parent device as base!!!
  dev_warn(dev, "HAOD: parent dev name is %s\n", dev_name(dev));
  no_of_modules = smartio_get_no_of_modules(node, node_name);
  dev_warn(dev, "HAOD: Node has %d modules\n", no_of_modules);
  dev_warn(dev, "HAOD: Node name is %s\n", node_name);
  if (no_of_modules < 0)
    goto done;
  dev_set_name(&node->dev, "%s-%d", node_name, node->nr);
  dev_warn(dev, "HAOD: set node name %s\n", dev_name(&node->dev));

  status = device_add(&node->dev);
  pr_warn("HAOD: added node %s\n", dev_name(&node->dev));
  if (status < 0) {
    dev_err(dev, "HAOD: failed to add node device\n");
    put_device(&node->dev);
    return status;
  }
  dev_warn(dev, "HAOD: Registered master %s\n", dev_name(&node->dev));

  mutex_lock(&core_lock);
  list_add_tail(&node->list, &smartio_node_list);
  mutex_unlock(&core_lock);
  dev_set_drvdata(dev, node);

  for (i=1; i < no_of_modules; i++) {
    char function_name[SMARTIO_NAME_SIZE+1];
    int no_of_attributes;

    status = smartio_get_function_info(node, i, &no_of_attributes, function_name);

    if (!status) {
      // Create a new device for this function
      struct device* function_dev = kzalloc(sizeof *function_dev, GFP_KERNEL);

      if (!function_dev) {
	dev_err(&node->dev, "No memory for function device %s\n", function_name);
	continue;
      }
      dev_warn(&node->dev, "Function name is %s\n", function_name);
      dev_warn(&node->dev, "Function has %d attributes\n", no_of_attributes);
      
      function_dev->parent = &node->dev;
      dev_set_name(function_dev, function_name);
      function_dev->bus = &smartio_bus;
      function_dev->id = 0;
//      function_dev->class = &smartio_function_class;
	function_dev->release = function_release;
      status = device_register(function_dev);
      if (status < 0) {
	dev_err(&node->dev, "Failed to add function device %s\n", function_name);
	put_device(function_dev);
	kfree(function_dev);
      }
    }
    else {
      dev_err(&node->dev, "Failed to query node for function %s info\n", function_name);
    }
  }
  return 0;

 done:
  return -1;
}

static int dev_unregister_function(struct device* dev, void* null)
{
	struct smartio_device *function;
	
	dev_warn(dev, "Unregistering function %s\n", dev_name(dev));
	function = container_of(dev, struct smartio_device, dev); 
	device_unregister(dev);
	return 0;
}

void smartio_unregister_node(struct device *dev)
{
	struct smartio_node *node = dev_get_drvdata(dev);
	int status;

	dev_warn(dev, "Unregistering node %s\n", dev_name(&node->dev));
	mutex_lock(&core_lock);
	list_del(&node->list);
	mutex_unlock(&core_lock);
	
	status = device_for_each_child(&node->dev, NULL, dev_unregister_function);

	device_unregister(&node->dev);
	pr_warn("HAOD: Unregistering done.\n");
}
EXPORT_SYMBOL_GPL(smartio_unregister_node);


#if 0
static void devm_smartio_unregister(struct device* dev, void* res)
{
  smartio_unregister_node(*(struct smartio_node**)res);
}


int devm_smartio_register_node(struct device *dev)
{
  struct smartio_node *node;
  struct smartio_node **ptr;
  int ret;

  pr_warn("HAOD: should reach this!!!\n");
  dev_warn(dev, "HAOD: about to register based on %s\n", dev_name(dev));

  node = devm_kzalloc(dev, sizeof *node, GFP_KERNEL);
  if (!node)
    return -ENOMEM;

  dev_warn(dev, "HAOD: allocated node mem\n");
  ptr = devres_alloc(devm_smartio_unregister, sizeof(*ptr), GFP_KERNEL);
  if (!ptr)
    return -ENOMEM;
  dev_warn(dev, "HAOD: allocated devres\n");
  ret = smartio_register_node(dev, node);
  if (!ret) {
    dev_warn(dev, "HAOD: smartio_register_node successful\n");
    *ptr = node;
    devres_add(dev, ptr);
  }
  else {
    devres_free(ptr);
  }
  return ret;
  
}
EXPORT_SYMBOL_GPL(devm_smartio_register_node);
#endif

int dev_smartio_register_node(struct device *dev, 
			      char* name, 
			      int (*cb)(struct smartio_node* this, 
					struct smartio_comm_buf* tx,
					struct smartio_comm_buf* rx))
{
  struct smartio_node *node;
  int ret = 0;

  dev_warn(dev, "HAOD: about to register based on %s\n", dev_name(dev));

  node = kzalloc(sizeof *node, GFP_KERNEL);
  if (!node) 
    return -ENOMEM;
  dev_warn(dev, "HAOD: allocated node mem\n");

  node->communicate = cb;
  ret = smartio_register_node(dev, node, name);
  if (ret) {
    dev_warn(dev, "HAOD: smartio_register_node failed\n");
    goto reclaim_node_memory;
  }
  dev_warn(dev, "HAOD: smartio_register_node successful\n");


  return ret;
reclaim_node_memory:
  kfree(node);
  return ret;
}
EXPORT_SYMBOL_GPL(dev_smartio_register_node);


int smartio_add_driver(struct smartio_function_driver* sd)
{
  sd->driver.owner = THIS_MODULE;
  sd->driver.bus = &smartio_bus;
  return driver_register(&sd->driver);
}
EXPORT_SYMBOL_GPL(smartio_add_driver);

void smartio_del_driver(struct smartio_function_driver* sd)
{
  driver_unregister(&sd->driver);
}
EXPORT_SYMBOL_GPL(smartio_del_driver);


static int __init my_init(void)
{
  if (bus_register(&smartio_bus) < 0) {
    pr_err("smartio: Failed to register bus\n");
    goto fail_bus_register;
  }

  if (class_register(&smartio_node_class) < 0) {
    pr_err("smartio: Failed to register node class\n");
    goto fail_node_class_register;
  }

  if (class_register(&smartio_function_class) < 0) {
    pr_err("smartio: Failed to register function class\n");
    goto fail_function_class_register;
  }


  work_queue = alloc_workqueue(smartio_bus.name, WQ_UNBOUND, 1);
  if (work_queue == NULL) {
    pr_err("smartio: Failed to create workqueue\n");
    goto fail_workqueue;
  }
  pr_warn("smartio: Done registering  bus driver\n");
  return 0;

fail_workqueue:
  class_unregister(&smartio_function_class);
fail_function_class_register:
  class_unregister(&smartio_node_class);
fail_node_class_register:
  bus_unregister(&smartio_bus);
fail_bus_register:
  return -1;
}
module_init(my_init);

static void __exit my_cleanup(void)
{
  class_unregister(&smartio_function_class);
  class_unregister(&smartio_node_class);
  bus_unregister(&smartio_bus);
  pr_warn("Removed smartio bus driver\n");
}
module_exit(my_cleanup);


MODULE_AUTHOR("Hans Odeberg <hans.odeberg@intel.com>");
MODULE_DESCRIPTION("SmartIO bus driver");
MODULE_LICENSE("GPL v2");

