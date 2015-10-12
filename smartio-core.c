#include <linux/device.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/kfifo.h>
#include <linux/ctype.h>
#include <asm-generic/uaccess.h>

#include "smartio.h"
#include "smartio_inline.h"
#include "convert.h"
#include "txbuf_list.h"
#include "minor_id.h"

struct smartio_devread_work;
#define DEV_FIFO_SIZE 128

/* Char device major number */
static int major;


static void free_groups(struct attribute_group** groups);

struct dev_attr_info {
  bool isInput; /* Present in user space or sysfs for udev to set rw flags */
  int type; /* Present to user space to simplify decoding */
  int attr_ix;
};


struct fcn_dev {
  int function_ix;
  struct device dev;
  struct dev_attr_info devattr;  
  DECLARE_KFIFO_PTR(fifo, uint8_t);
  struct smartio_devread_work *devread_work;  
};

static void smartio_node_release(struct device *dev)
{
#if 1
  struct smartio_node *node;

  node = container_of(dev, struct smartio_node, dev);
#endif
  dev_warn(dev, "Releasing smartio node\n");
#if 1
  kfree(node);
#else
  kfree(dev);
#endif
}

#if 0
static struct class smartio_node_class = {
  .name = "smartio_node",
  .owner = THIS_MODULE,
  //  .dev_release = smartio_node_release
};


static struct device_type function_devt = {
  .name = "smartio_function",
};
#endif

static struct device_type controller_devt = {
  .name = "smartio_controller",
  .release = smartio_node_release
};

int smartio_match(struct device* dev, struct device_driver* drv)
{
  struct smartio_driver* driver = to_smartio_driver(drv);
  const struct smartio_device_id* drv_id;

  dev_info(dev, "Matching device %s to driver %s\n", dev_name(dev), drv->name);  
  for(drv_id = driver->id_table; drv_id->name != NULL; drv_id++) {
    if (!strncmp(dev_name(dev), drv_id->name, strlen(drv_id->name))) {
      dev_info(dev, "Match of %s succeeded\n", drv_id->name);
      return 1;
    }
    else {
      dev_info(dev, "No match for %s\n", drv_id->name);
    }
  }
  return 0;
}


static void function_release(struct device* dev)
{
	struct fcn_dev *fcn;

	dev_info(dev, "Smartio function release called\n");	
	fcn = container_of(dev, struct fcn_dev, dev);
	free_groups((struct attribute_group**) dev->groups);
	kfree(dev->groups);
	kfree(fcn);
}

struct  bus_type smartio_bus = {
  .name = "smartio",
  .dev_name = "smartio_bus_master",
  .match = smartio_match,
};

static struct class smartio_function_class = {
  .name = "smartio_function",
  .owner = THIS_MODULE,
  .dev_release = function_release
};


struct fcn_attribute {
  struct device_attribute dev_attr;
  int attr_ix;
  int type;
};

// The workqueue used for all smartio work
static struct workqueue_struct *work_queue;

struct smartio_work {
  struct work_struct work;
  struct smartio_comm_buf *comm_buf;
  struct smartio_node* node; 
};

struct smartio_devread_work {
  struct delayed_work work;
  struct fcn_dev* fcn_dev; 
};


static DECLARE_WAIT_QUEUE_HEAD(wait_queue);



static void handle_response(struct smartio_comm_buf *resp)
{
  struct smartio_comm_buf *req;

  pr_info("Entering handle_response\n");

  req = smartio_find_transaction(smartio_get_transaction_id(resp));

  if (req) {
	req->cb(req, resp, req->cb_data);
	wake_up_interruptible(&wait_queue);
  }
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
      dev_info(&node->dev, "Got a response message\n");
      handle_response(&rx);
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

#ifdef DBG_TRANS
  pr_info("HAOD: request work function\n");
#endif
  smartio_add_transaction(my_work->comm_buf);
  talk_to_node(my_work->node, my_work->comm_buf);
  kfree(my_work);
}


static int transaction_done(struct smartio_comm_buf *buf)
{
#ifdef DBG_TRANS
  pr_info("HAOD: wakeup cond is %s\n",
	  (smartio_get_direction(buf) == SMARTIO_FROM_NODE) ?
	  "FROM_NODE" : "TO_NODE");
#endif
  return smartio_get_direction(buf) == SMARTIO_FROM_NODE;
}


static void request_completion_cb(struct smartio_comm_buf *req,
				  struct smartio_comm_buf *resp,
				  void *data)
{
  req->data_len = resp->data_len;
  memcpy(req->data, resp->data, resp->data_len);
  pr_info("Copied %d bytes from resp to req\n", req->data_len);
  /* Swapping the direction tells waiter it needs to sleep
     no more. */
  smartio_set_direction(req, SMARTIO_FROM_NODE);
}


static int post_request(struct smartio_node* node,
			struct smartio_comm_buf* buf)
{
  struct smartio_work *my_work;
  int status;

  // Set the transaction header
  smartio_set_msg_type(buf, SMARTIO_REQUEST);
  smartio_set_direction(buf, SMARTIO_TO_NODE);
  buf->cb = request_completion_cb;


  // Post deferred work
  my_work = kmalloc(sizeof *my_work, GFP_KERNEL);
  if (!my_work) {
    dev_err(&node->dev, "No memory for work item\n");
    return -ENOMEM;
  }

  INIT_WORK(&my_work->work, wq_fcn_post_message);
  my_work->comm_buf = buf;
  my_work->node = node;
#ifdef DBG_WORK
  pr_info("Before queueing work\n");
#endif
  status = queue_work(work_queue, &my_work->work);
#ifdef DBG_WORK
  pr_info("After queueing work\n");
#endif
  if (!status) {
    pr_err("Failed to queue work\n");
    kfree(my_work);
    return -ENOMEM; // TBD better error code
  }
  status = wait_event_interruptible(wait_queue, transaction_done(buf));
  if (status == 0) {
#ifdef DBG_WORK
    pr_info("Woke up after request\n");
#endif
  }
  else {
    pr_info("Received a signal\n");
  }
  return status;
}



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
EXPORT_SYMBOL(smartio_get_no_of_modules);



static int smartio_get_function_info(struct smartio_node* node, 
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

struct attr_info {
  uint8_t input:1;
  uint8_t output:1;
  uint8_t device:1;
  uint8_t isDir:1;
  uint8_t arr_size;
  uint8_t type;
  char name[SMARTIO_NAME_SIZE+1];
};

int smartio_get_attr_info(struct smartio_node* node, 
	       	          int module,
			  int attr,
			  struct attr_info *info)
{
  struct smartio_comm_buf* buf;
  int status;

  buf = kzalloc(sizeof *buf, GFP_KERNEL);
  if (!buf) 
    return -ENOMEM;

  buf->data_len = 4; // module + command + attr ix
  buf->data[0] = module;
  buf->data[1] = SMARTIO_GET_ATTRIBUTE_DEFINITION;
  smartio_write_16bit(buf, 2, attr);
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

  info->input = (buf->data[1] & IO_IS_INPUT) ? 1 : 0;
  info->output = (buf->data[1] & IO_IS_OUTPUT) ? 1 : 0;
  info->device = (buf->data[1] & IO_IS_DEVICE) ? 1 : 0;
  info->isDir = (buf->data[1] & IO_IS_DIR) ? 1 : 0;
  info->arr_size = buf->data[2];
  info->type = buf->data[3];
  strncpy(info->name, buf->data+4, SMARTIO_NAME_SIZE);
  info->name[SMARTIO_NAME_SIZE] = '\0';
  #if 0
  dev_warn(&node->dev, "Read attribute def\n");
  dev_warn(&node->dev, "buf len: %d\n", buf->data_len);
  dev_warn(&node->dev, "flags: %d\n", buf->data[1]);
  dev_warn(&node->dev, "arr_size: %d\n", buf->data[2]);
  dev_warn(&node->dev, "type: %d\n", buf->data[3]);
  dev_warn(&node->dev, "name: %s\n", info->name);
#endif
  return 0;
}


static int smartio_get_attr_value(struct smartio_node* node, 
				  int function,
				  int attr,
				  int arr_ix,
				  void *data,
				  int *len)
{
  struct smartio_comm_buf* buf;
  int status;

  buf = kzalloc(sizeof *buf, GFP_KERNEL);
  if (!buf) 
    return -ENOMEM;

  buf->data_len = 5; // module + command + attr ix + array ix
  buf->data[0] = function;
  buf->data[1] =  SMARTIO_GET_ATTR_VALUE;
  smartio_write_16bit(buf, 2, attr);
  buf->data[4] = arr_ix;
  status = post_request(node, buf);
  if (status < 0) {
    dev_err(&node->dev, "DARN DARN DARN\n");
    return status;
  }

  if (buf->data_len <= 2) {
    dev_err(&node->dev, "get_attr_value: illegal data length %d\n", buf->data_len);
    return -ENOMEM; // TBD: err to indicate wrong data size
  }
  if (buf->data[0] != SMARTIO_SUCCESS) {
    switch (buf->data[0]) {
    case SMARTIO_ILLEGAL_MODULE_INDEX:
      dev_err(&node->dev, "get_attr_value: illegal module index %d\n", node->dev.id);
      break;
    case SMARTIO_ILLEGAL_ATTRIBUTE_INDEX:
      dev_err(&node->dev, "get_attr_value: illegal attribute index %d\n", attr);
      break;
    case SMARTIO_ILLEGAL_ARRAY_INDEX:
      dev_err(&node->dev, "get_attr_value: illegal array index %d\n", arr_ix);
      break;
    default:
      dev_err(&node->dev, "get_attr_value: unknown msg status %d\n", buf->data[0]);
      break;
    }
    return -ENOMEM; // TBD: err to indicate wrong module index
  }

  memcpy(data, buf->data + 1, buf->data_len - 1);
  *len = buf->data_len - 1;
  return 0;
}

int smartio_set_attr_value(struct smartio_node* node, 
			   int function,
			   int attr,
			   int arr_ix,
			   void *data,
			   int len)
{
  struct smartio_comm_buf* buf;
  int status;

  buf = kzalloc(sizeof *buf, GFP_KERNEL);
  if (!buf) 
    return -ENOMEM;

  buf->data_len = 5 + len; // module + command + attr ix + array ix
  buf->data[0] = function;
  buf->data[1] =  SMARTIO_SET_ATTR_VALUE;
  smartio_write_16bit(buf, 2, attr);
  buf->data[4] = arr_ix;
  memcpy(buf->data + 5, data, len);
  dev_info(&node->dev, "Posting %d bytes of attr data\n", len);
  status = post_request(node, buf);
  if (status < 0) {
    dev_err(&node->dev, "%s: request failed. Error %d\n", __func__, status);
    return status;
  }

  if (buf->data_len != 1) {
    dev_err(&node->dev, "%s: illegal data length %d\n", __func__, buf->data_len);
    return -ENOMEM; // TBD: err to indicate wrong data size
  }
  if (buf->data[0] != SMARTIO_SUCCESS) {
    switch (buf->data[0]) {
    case SMARTIO_ILLEGAL_MODULE_INDEX:
      dev_err(&node->dev, "%s: illegal module index %d\n", __func__, node->dev.id);
      break;
    case SMARTIO_ILLEGAL_ATTRIBUTE_INDEX:
      dev_err(&node->dev, "%s: illegal attribute index %d\n", __func__, attr);
      break;
    case SMARTIO_ILLEGAL_ARRAY_INDEX:
      dev_err(&node->dev, "%s: illegal array index %d\n", __func__, arr_ix);
      break;
    case   SMARTIO_NO_PERMISSION:
      dev_err(&node->dev, "%s: no permission to write attribute %d\n", __func__, arr_ix);
      break;
    default:
      dev_err(&node->dev, "%s: unknown msg status %d\n", __func__, buf->data[0]);
    }
    return -ENOMEM; // TBD: err to indicate wrong module index
  }

  return 0;
}


static void dump_node(struct device * dev)
{
  	struct smartio_node *node = container_of(dev->parent, struct smartio_node, dev);

	dev_info(dev, "dumping node %d\n", dev->id);
	dev_info(dev, "node communicate fcn: %p\n", node->communicate);
}

static ssize_t show_fcn_attr(struct device *dev,
			     struct device_attribute *attr,
			     char *buf)
{
	u8 mybuf[40];
	int result;
	int bytes_read;
	struct smartio_node *node = container_of(dev->parent, struct smartio_node, dev);
	struct fcn_dev *fcn = container_of(dev, struct fcn_dev, dev);
	struct fcn_attribute* fcn_attr = container_of(attr, struct fcn_attribute, dev_attr);

	dev_info(dev, "Calling show fcn for node %d, fcn ix %d, attr %s, ix %d, type %d\n", 
		 dev->parent->id, fcn->function_ix, attr->attr.name, 
                 fcn_attr->attr_ix, fcn_attr->type);
	dump_node(dev);

	result = smartio_get_attr_value(node, 
					fcn->function_ix,
					fcn_attr->attr_ix,
					0xFF, /* No arrays for now */
					mybuf,
					&bytes_read);
	smartio_raw_to_string(fcn_attr->type, mybuf, buf);
	return strlen(buf);
}

static ssize_t store_fcn_attr(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf,
			      size_t count)
{
	char rawbuf[40];
	int raw_len;
	struct smartio_node *node = container_of(dev->parent, struct smartio_node, dev);
	struct fcn_dev *fcn = container_of(dev, struct fcn_dev, dev);
	struct fcn_attribute* fcn_attr = container_of(attr, struct fcn_attribute, dev_attr);

       	dev_info(dev, "Calling store fcn for node %d, fcn %d, attr %s\n, ix %d,  value: %s\n", 
		 dev->parent->id,
		 fcn->function_ix,
		 attr->attr.name,
		 fcn_attr->attr_ix,
		 buf);
	smartio_string_to_raw(fcn_attr->type, buf, rawbuf, &raw_len);
	dev_info(dev, "rawbuf: %s, len: %d\n", rawbuf, raw_len);
	smartio_set_attr_value(node,
			       fcn->function_ix,
			       fcn_attr->attr_ix,
			       0xFF, /* No arrays for now */
			       rawbuf,
			       raw_len);
	return count;
}			    


/* All attributes have to be allocated dynamically, as we do not
   know in advance which attributes there are. This is a bit
   unorthodox, leading to the usual DEVICE_ATTR macro being
   a bit unusable.
   To define a set of attributes in their own directory, just
   create an additional attribute group with a name (which will
   be the name of the directory). */
static struct attribute *create_attr(const struct attr_info *cur_attr,
				     const struct attr_info *head)
{
  const char* name = cur_attr->name;
  bool readonly = cur_attr->input == 0;
  int fcn_number = cur_attr - head;
  int type = cur_attr->type;
	char* name_cpy;
	struct fcn_attribute *fcn_attr = kzalloc(sizeof *fcn_attr, GFP_KERNEL);
	
	if (!fcn_attr)
		return NULL;

	if (!readonly)
	  fcn_attr->dev_attr.store = store_fcn_attr;
	fcn_attr->dev_attr.attr.mode = readonly ? 0444 : 0644;
	fcn_attr->dev_attr.show = show_fcn_attr;
	fcn_attr->attr_ix = fcn_number;
	fcn_attr->type = type;
	name_cpy = kmalloc(strlen(name)+1, GFP_KERNEL);
	if (!name_cpy)
		goto release_attr;
	strcpy(name_cpy, name);
	fcn_attr->dev_attr.attr.name = name_cpy;
	sysfs_attr_init(fcn_attr->dev_attr.attr);
	return &fcn_attr->dev_attr.attr;
	
release_attr:
	kfree(fcn_attr);
	return NULL;	
}



/* Read the number of groups. Note that there always is at least
   one group; the default one. */
static int get_no_of_groups(struct attr_info* info, int size)
{
  int i;
  int count = 1;

  for (i=0; i < size; i++)
    if (info[i].isDir) count++;

  return count;
}

static int get_no_of_attrs_in_group(const struct attr_info *attr, const struct attr_info * const end)
{
	int count = 0;

	while ((attr != end) && (!attr->isDir)) {
	  if (!attr->device) count++;
	  attr++;
	}

	return count;
}

static int get_no_of_devs_in_group(const struct attr_info *attr, const struct attr_info * const end)
{
	int count = 0;

	while ((attr != end) && (!attr->isDir)) {
	  if (attr->device) count++;
	  attr++;
	}

	return count;
}


static void free_group(struct attribute_group *grp)
{
	if (grp->attrs) {
		struct attribute** attr;

		for (attr = grp->attrs; *attr != NULL; attr++) {
			struct attribute *a = *attr;
			struct device_attribute* d;

#if 0				
			pr_warn("Free: attr name %s\n", a->name);
#endif
			kfree(a->name);
			d = container_of(a, struct device_attribute, attr);
#if 0
			pr_warn("Free: attribute\n");
#endif
			kfree(d);
		}
#if 0
		pr_warn("Free: group attrs\n");
#endif
		kfree(grp->attrs);
		grp->attrs = NULL;
#if 0
		pr_warn("Free: group name %s\n", grp->name);
#endif
		kfree(grp->name);
		grp->name = NULL;
	}

}


static void free_groups(struct attribute_group** groups)
{
	while (*groups) {
	  pr_warn("Free: group\n");
	  free_group((struct attribute_group *) *groups);
	  groups++;
	}
}

#if 0
/* Debugging function to ensure we created the right structure */
static void dump_group_tree(const struct attribute_group** groups)
{
  int g;
  

  pr_warn("dumping group tree\n");
  for (g=0; *groups != NULL; groups++, g++) {
    struct attribute **attr = (*groups)->attrs;

    pr_warn("group %d\n", g);
    pr_warn("group ptr %p\n", *groups);
    pr_warn("group name ptr %p\n", (*groups)->name);
    pr_warn("group name:%s\n", (*groups)->name);
    for (; *attr != 0; attr++) {
      pr_warn("attrs ptr:%p\n", attr);
      pr_warn("attr ptr:%p\n", *attr);
      pr_warn("attr name:%s\n", (*attr)->name);
      pr_warn("attr mode:%o\n", (unsigned int) (*attr)->mode);
    }

  }
}
#endif

/* Create attributes for one group. If start is not a directory definition,
   then this is the default group.
   Returns pointer to the attribute after this group. That can be:
      the definition of a new group
      the end pointer when we are done
      null for error */
const struct attr_info *process_one_attr_group(const struct attr_info *head, 
					       const struct attr_info *start, 
					       const struct attr_info * const end,
					       struct attribute_group * const grp,
					       const struct attr_info **dev_attr)
{
	const struct attr_info *cur_attr = start;
	int i;
	int size = 0;
	int no_of_devs = 0;

	if (cur_attr->isDir) {
	  	grp->name = kstrdup(cur_attr->name, GFP_KERNEL);
		if (!grp->name) {
			pr_err("Failed to allocate attribute name %s\n", cur_attr->name);
			return NULL;
		}
		pr_warn("attr_group: found directory attribute\n");
		cur_attr++;
	}
	size = get_no_of_attrs_in_group(cur_attr, end);
	no_of_devs = get_no_of_devs_in_group(cur_attr, end);
	grp->attrs = kzalloc((sizeof grp->attrs) * (size + 1), GFP_KERNEL);
	for (i=0; (cur_attr < end) && !cur_attr->isDir; i++, cur_attr++) {
	  if (cur_attr->device && (*dev_attr == NULL)) {
	    *dev_attr = cur_attr;
	  }
	  else {
	    grp->attrs[i] = create_attr(cur_attr, head);
	    if (!grp->attrs[i]) {
	      pr_err("process_one_attr_group: Failed to create attribute\n");
	      goto cleanup;
	    }
	  }
	}
	return cur_attr;

 cleanup:
	free_group(grp);
	return NULL;
}




/* The structure of the attribute group is:
1) Each attribute is in a struct attribute
2) Arrays of attributes are gathered in struct attribute_group. If a name
   is given to the group, that shows up as an additional directory
   in sysfs
   3) The groups are gathered in an array.
During introspection, information about each attribute is returned.
If an attribute has "isDirectory" bit set, it will denote the 
name of a group, and subsequent attributes until the next isDirectory
item is put in that group.
Current limitations:
  Device nodes not handled.
  Attribute arrays not handled. 
*/
static void
define_function_attrs(struct smartio_node* node,
		      struct fcn_dev *function_dev,
		      int module,
		      int no_of_attributes)
{
	struct attr_info info[no_of_attributes];
	const struct attr_info * const info_end = info + no_of_attributes;
	const struct attr_info *info_current = info;
	int i;
	int no_of_groups;	
	struct attribute_group **groups = NULL;

	dev_info(&node->dev, "define_function_attrs: entry\n");
	dev_info(&node->dev, "define_function_attrs: attrs to process: %d\n", no_of_attributes);
	for (i=0; i < no_of_attributes; i++) {
	  int res;

	  dev_info(&node->dev, "processing attr ix: %d\n", i);
	  res = smartio_get_attr_info(node, module, i, &info[i]);

	  if (res != 0) {
	    dev_err(&node->dev, "Failed reading attribute #%d\n", i);
	    return;
	  }
	}

	no_of_groups = get_no_of_groups(info, no_of_attributes);
        dev_info(&node->dev, "No of groups are: %d\n", no_of_groups);

	/* Allocate array for group pointers and ending null pointer */
	groups = kzalloc((sizeof *groups) * (no_of_groups+1), GFP_KERNEL);
	if (!groups) {
	  dev_err(&node->dev, "Failed to allocate groups array\n");
	  goto fail_alloc_groups;
	}

	dev_info(&node->dev, "define_function_attrs: beginning to create groups\n");
	for (i=0; i < no_of_groups; i++) {
	  const struct attr_info *dev_attr = NULL;

	  if (!info_current) {
	    dev_err(&node->dev, "Failed parsing group\n");
	    goto cleanup;
	  }
	  groups[i] = kzalloc(sizeof *groups[i], GFP_KERNEL);
	  if (!groups[i]) {
	    dev_err(&node->dev, "Failed allocating a group\n");
	    goto cleanup;
	  }
	  info_current = process_one_attr_group(info, info_current, info_end, 
						groups[i], &dev_attr);
	  if (dev_attr) {
	    function_dev->dev.devt = MKDEV(major, get_minor_number());
	    function_dev->devattr.type = dev_attr->type;
	    function_dev->devattr.isInput = dev_attr->input;
	    function_dev->devattr.attr_ix = dev_attr - info;
	    dev_attr = NULL;
	  }
	}
	function_dev->dev.groups = (const struct attribute_group**) groups;
	return;

 cleanup:
	free_groups(groups);
	kfree(groups);
 fail_alloc_groups:
	return;
}

#if 0
static void free_attributes(struct device* dev)
{
	const struct attribute_group** group;
	if (!dev->groups) 
		return;
	for (group = dev->groups; *group != NULL; group++) {
		if ((*group)->attrs) {
			struct attribute** attr;

			for (attr = (*group)->attrs; *attr != NULL; attr++) {
				struct attribute *a = *attr;
				struct device_attribute* d;
				
				pr_info("Free: attr name %s\n", a->name);
				kfree(a->name);
				d = container_of(a, struct device_attribute, attr);
				pr_info("Free: attribute\n");
				kfree(d);
			}
			pr_info("Free: group attrs\n");
			kfree((*group)->attrs);
		}
		pr_info("Free: group name %s\n", (*group)->name);
		kfree((*group)->name);
		pr_info("Free: group attrs\n");
		kfree(*group);
	}
	pr_info("Free: groups\n");
	kfree(dev->groups);
}
#endif

struct dev_match {
  int id;
  const char *devname;
};


static int update_devid_if_match(struct device *dev, void *data)
{
  struct dev_match *info = data;
  const char *name_to_test = dev_name(dev);
  
  if (!strncmp(info->devname, name_to_test, strlen(info->devname)) && 
      isdigit(name_to_test[strlen(info->devname)])) {
    info->id = max(info->id, (int) dev->id);
  }

  return 0;
}

static int get_highest_dev_id(const char *devname)
{
  struct dev_match data = { -1, devname };

  bus_for_each_dev(&smartio_bus, NULL, &data, update_devid_if_match);

  return data.id;
}


static int create_function_device(struct smartio_node *node,
				  int function_ix)
{
	char function_name[SMARTIO_NAME_SIZE+1];
	int no_of_attributes;
	int status;
	struct fcn_dev* function_dev = NULL;
	
	status = smartio_get_function_info(node, function_ix,
					   &no_of_attributes,
					   function_name);

	if (!status) {
		// Create a new device for this function
		function_dev = kzalloc(sizeof *function_dev, GFP_KERNEL);

		if (!function_dev) {
			dev_err(&node->dev,
				"No memory for function device %s\n",
				function_name);
			status = -1;
			goto done;
		}
		function_dev->function_ix = function_ix;
		dev_warn(&node->dev, "Function name is %s\n", function_name);
		dev_warn(&node->dev, "Function ix is %d\n", function_ix);
		dev_warn(&node->dev, "Function has %d attributes\n",
			 no_of_attributes);

		function_dev->dev.parent = &node->dev;
		function_dev->dev.bus = &smartio_bus;
		function_dev->dev.id = get_highest_dev_id(function_name) + 1;
		dev_set_name(&function_dev->dev, "%s%d", function_name,
			     function_dev->dev.id);
		//      function_dev->dev.class = &smartio_function_class;
		function_dev->dev.release = function_release;
		define_function_attrs(node, 
				      function_dev,
				      function_ix,
				      no_of_attributes);
		if (!function_dev->dev.groups) {
			dev_err(&node->dev,
				"Could not define function attrs\n");
			goto release_memory;
		}
#if 0
		dump_group_tree(function_dev->dev.groups);

		function_dev->dev.groups = NULL;
#endif
		status = device_register(&function_dev->dev);
		if (status < 0) {
			dev_err(&node->dev, 
				"Failed to add function device %s\n",
				function_name);
			goto release_dev;
		}
		dev_info(&function_dev->dev, "MAJOR = %d, MINOR = %d\n", 
			 MAJOR(function_dev->dev.devt), 
			 MINOR(function_dev->dev.devt));

	}
	else {
		dev_err(&node->dev,
			"Failed to query node for function %s info\n",
			function_name);
	}
	goto done;
	
release_dev:
	put_device(&function_dev->dev);
	return status;
release_memory:
	kfree(function_dev);
done:
	return status;
}


/*
  dev: the hardware device (i2c, spi, ...)
  node: the function bus controller device
*/
#define USE_TYPE
static int smartio_register_node(struct device *dev, struct smartio_node *node, char *name)
{
	int status = -1;

	device_initialize(&node->dev);
	node->dev.parent = dev;
	node->dev.bus = &smartio_bus;
	node->dev.type = &controller_devt;
	node->dev.id = get_highest_dev_id(smartio_bus.dev_name) + 1;
	dev_info(dev, "Allocated node number %d\n", node->dev.id);
	if (node->dev.id < 0)
		return node->dev.id;

	dev_set_drvdata(dev, node);
	status = device_add(&node->dev);
	dev_info(dev, "Added node %s\n", dev_name(&node->dev));
	if (status < 0) {
		dev_err(dev, "Failed to add node device\n");
		put_device(&node->dev);
		return status;
	}
	dev_info(dev, "Registered master %s\n", dev_name(&node->dev));

	return 0;
}

static int dev_unregister_function(struct device* dev, void* null)
{
  dev_t devt;

  dev_warn(dev, "Unregistering function %s\n", dev_name(dev));
  /* Read the major/minor number now, just in case the struct device
     is freed, alloced and overwritten by somebody else before
     we can access it after unregistering. */
  devt = dev->devt;
  device_unregister(dev);
  if (MAJOR(devt))
    release_minor_number(MINOR(devt));
  return 0;
}


/* dev points to function bus controller device */
int smartio_unregister_node(struct device *dev, void* null)
{
	dev_warn(dev, "Unregistering function bus controller node\n");
	device_unregister(dev);
	pr_warn("Unregistering done.\n");
	return 0;
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

  dev_warn(dev, "About to register based on %s\n", dev_name(dev));

  node = devm_kzalloc(dev, sizeof *node, GFP_KERNEL);
  if (!node)
    return -ENOMEM;

  dev_warn(dev, "Allocated node mem\n");
  ptr = devres_alloc(devm_smartio_unregister, sizeof(*ptr), GFP_KERNEL);
  if (!ptr)
    return -ENOMEM;
  dev_warn(dev, "Allocated devres\n");
  ret = smartio_register_node(dev, node);
  if (!ret) {
    dev_warn(dev, "smartio_register_node successful\n");
    *ptr = node;
    devres_add(dev, ptr);
  }
  else {
    devres_free(ptr);
  }
  return ret;
  
}
EXPORT_SYMBOL(devm_smartio_register_node);
#endif

int dev_smartio_register_node(struct device *dev, 
			      char* name, 
			      int (*cb)(struct smartio_node* this, 
					struct smartio_comm_buf* tx,
					struct smartio_comm_buf* rx))
{
  struct smartio_node *node;
  int ret = 0;

  dev_warn(dev, "About to register based on %s\n", dev_name(dev));

  node = kzalloc(sizeof *node, GFP_KERNEL);
  if (!node) 
    return -ENOMEM;
  dev_warn(dev, "Allocated node mem\n");

  node->communicate = cb;
  ret = smartio_register_node(dev, node, name);
  if (ret) {
    dev_warn(dev, "%s failed\n", __func__);
    goto reclaim_node_memory;
  }
  dev_warn(dev, "%s successful\n", __func__);


  return ret;
reclaim_node_memory:
  kfree(node);
  return ret;
}
EXPORT_SYMBOL_GPL(dev_smartio_register_node);


static int match_minor(struct device *dev, void *data)
{
  int *minor = (int *) data;

  return MAJOR(dev->devt) && (MINOR(dev->devt) == *minor);
}


static void dev_read_completion_cb(struct smartio_comm_buf *req,
				   struct smartio_comm_buf *resp,
				   void *data)
{
  struct fcn_dev *dev = (struct fcn_dev *) data;

  if (kfifo_avail(&dev->fifo) < (resp->data_len-1)) {
    dev_err(&dev->dev, "read fifo overrun\n");
    goto free_buffers;
  }
  kfifo_in(&dev->fifo, resp->data + 1, resp->data_len-1);
 free_buffers:
  kfree(req);
}



static void wq_fcn_dev_read(struct work_struct *w)
{
  struct smartio_devread_work *my_work = 
    container_of(to_delayed_work(w), struct smartio_devread_work, work);
  struct smartio_node *node = container_of(my_work->fcn_dev->dev.parent,
					   struct smartio_node, 
					   dev);
  struct smartio_comm_buf* tx;
  int status;

  tx = kzalloc(sizeof *tx, GFP_KERNEL);
  if (tx) { 
    fillbuf_get_attr_value(tx, my_work->fcn_dev->function_ix,
			   my_work->fcn_dev->devattr.attr_ix, 0xFF);
    tx->cb_data = my_work->fcn_dev;
    tx->cb = dev_read_completion_cb;

    smartio_add_transaction(tx);
    status = talk_to_node(node, tx);
  }
  else 
    pr_err("Failed to allocate dev read comms buffer\n");

  schedule_delayed_work(&my_work->work, msecs_to_jiffies(1000));
}

static int dev_open(struct inode *i, struct file *filep)
{
  int minor = iminor(i);
  struct device *dev;
  struct fcn_dev *fcn_dev;

  pr_info("char_dev: %s called for minor %d!\n", __func__, minor);
  dev = bus_find_device(&smartio_bus, NULL, &minor, match_minor);

  if (!dev) {
    pr_err("%s: cannot open() as there is no matching device\n", __func__);
    return -ENODEV;
  }
  pr_info("device: %s\n", dev_name(dev));
  fcn_dev = container_of(dev, struct fcn_dev, dev);
  
  if (filep->f_mode & FMODE_READ) {
    if (kfifo_alloc(&fcn_dev->fifo, DEV_FIFO_SIZE, GFP_KERNEL)) {
      dev_err(dev, "%s: failed to allocate memory for device kfifo buffer\n", __func__);
      return -ENOMEM;
    }
    // Post deferred work
    fcn_dev->devread_work = kmalloc(sizeof *fcn_dev->devread_work, GFP_KERNEL);
    if (!fcn_dev->devread_work) {
      dev_err(dev, "No memory for work item\n");
      goto free_kfifo_mem;
    }
    INIT_DELAYED_WORK(&fcn_dev->devread_work->work, wq_fcn_dev_read);
    fcn_dev->devread_work->fcn_dev = fcn_dev;
    if (!queue_delayed_work(work_queue, &fcn_dev->devread_work->work, 0)) {
      dev_err(dev, "Failed to queue work\n");
      goto free_work;
    }
  }

  filep->private_data = fcn_dev;
  return 0;

 free_work:
  kfree(fcn_dev->devread_work);
 free_kfifo_mem:
  kfifo_free(&fcn_dev->fifo);
  return -ENOMEM;
}

static int dev_release(struct inode *i, struct file *filep)
{
  struct fcn_dev *fcn_dev = (struct fcn_dev*) filep->private_data;

  dev_info(&fcn_dev->dev, "%s called for minor %d!\n", __func__, iminor(i)); 
  if (filep->f_mode & FMODE_READ) {
    if (!cancel_delayed_work(&fcn_dev->devread_work->work))
      flush_workqueue(work_queue);
    kfree(fcn_dev->devread_work);
    fcn_dev->devread_work = NULL;
    kfifo_free(&fcn_dev->fifo);
  }

  return 0;
}




/* The read will continue until the requested count has been reached
   before returning.
   Whenever something is present in the kfifo, it is copied to the user-space
   buffer. 
   When there is nothing in the kfifo, the function sleeps. */
static ssize_t dev_read(struct file *filep, char __user *buf, size_t count, loff_t *ppos)
{
  struct fcn_dev *fcn_dev = (struct fcn_dev*) filep->private_data;

  int bytes_left = count;
  int bytes_available;

  pr_info("%s called!\n", __func__);
  pr_info("len: %d, ofs: %d\n", (int) count, (int) *ppos);
  pr_info("device: %s\n", dev_name(&fcn_dev->dev));
#if 0
  pr_info("input: %s\n", fcn_dev->devattr.isInput ? "yes" : "no");
  pr_info("attr ix = %d\n", fcn_dev->devattr.attr_ix);
  pr_info("type = %d\n", fcn_dev->devattr.type);
#endif

  if (*ppos < 0)
    return -EINVAL;
  if (!count)
    return 0;

  while (bytes_left > 0) {
    if (kfifo_is_empty(&fcn_dev->fifo)) {
      int status;
      const int fifo_threshold = kfifo_size(&fcn_dev->fifo) / 2;

      dev_info(&fcn_dev->dev,"Fifo empty; sleeping\n");
      status = wait_event_interruptible(wait_queue, 
					kfifo_len(&fcn_dev->fifo) >= 
					min(bytes_left, fifo_threshold));
      dev_info(&fcn_dev->dev,"Fifo no longer empty; woke up\n");
      if (status == 0) {
#ifdef DBG_WORK
	dev_info(&fcn_dev->dev, "Woke up after request\n");
#endif
      }
      else {
	dev_info(&fcn_dev->dev, "%s: Received a signal\n", __func__);
	// TBD: handle signal interruption correctly
	return -EFAULT; 
      }
    }

    dev_info(&fcn_dev->dev, "About to read from fifo\n");
    bytes_available = kfifo_len(&fcn_dev->fifo);
    if (bytes_available  > 0) {
      int bytes_to_read = min(bytes_available, bytes_left);
      int bytes_read;
    
      dev_info(&fcn_dev->dev, "bytes in fifo: %d\n", bytes_available);
      dev_info(&fcn_dev->dev, "bytes left: %d\n", bytes_left);
      dev_info(&fcn_dev->dev, "bytes to read: %d\n", bytes_to_read);
      if (kfifo_to_user(&fcn_dev->fifo, 
			buf + count - bytes_left, 
			bytes_to_read,
			&bytes_read) < 0) {
	dev_err(&fcn_dev->dev, "%s: Failed to read from kfifo\n", __func__);
	return -EFAULT;
      }
      bytes_left -= bytes_read;
      *ppos += bytes_read;
    };
    dev_info(&fcn_dev->dev, "Done (for now) reading from fifo\n");
  }
  return count - bytes_left;
}



static ssize_t dev_write(struct file *filep, const char __user *buf, 
			 size_t count, loff_t *ppos)
{
  struct fcn_dev *fcn_dev = (struct fcn_dev*) filep->private_data;
  struct smartio_node *node = container_of(fcn_dev->dev.parent, struct smartio_node, dev);
  char rawbuf[ATTR_MAX_PAYLOAD];

  int bytes_left = count;
  
  pr_info("%s called!\n", __func__);
  pr_info("len: %d, ofs: %d", (int) count, (int) *ppos);
  pr_info("device: %s\n", dev_name(&fcn_dev->dev));

  if (*ppos < 0)
    return -EINVAL;
  if (!count)
    return 0;
  do {
    const int bytes_to_copy = min(bytes_left, (int) (sizeof rawbuf));
    const int bytes_not_copied = copy_from_user(rawbuf, buf+(count-bytes_left), bytes_to_copy);
    const int bytes_to_send = bytes_to_copy - bytes_not_copied;

    pr_info("to copy: %d, not copied: %d, to send: %d\n", bytes_to_copy, bytes_not_copied, bytes_to_send);
    if (!bytes_to_send) {
      if (bytes_left != count)
	break;
      else
	return EFAULT;
    }
    smartio_set_attr_value(node,
			   fcn_dev->function_ix,
			   fcn_dev->devattr.attr_ix,
			   0xFF, /* No arrays for now */
			   rawbuf,
			   bytes_to_send);
    bytes_left -= bytes_to_send;
  } while (bytes_left > 0);

  *ppos += count - bytes_left;
  return count - bytes_left;
}

static struct file_operations char_dev_fops = {
  .owner = THIS_MODULE,
  .open = dev_open,
  .read = dev_read,
  .write = dev_write,
  .release = dev_release
};


int smartio_add_driver(struct smartio_driver* sd)
{
  sd->driver.bus = &smartio_bus;
  return driver_register(&sd->driver);
}
EXPORT_SYMBOL_GPL(smartio_add_driver);

void smartio_del_driver(struct smartio_driver* sd)
{
  driver_unregister(&sd->driver);
}
EXPORT_SYMBOL_GPL(smartio_del_driver);



static int fcn_ctrl_probe(struct device* dev)
{
  int status = 0;
  int no_of_modules = 0;
  char node_name[30];
  int i;
  struct smartio_node *node;  

  node = container_of(dev, struct smartio_node, dev);
  dev_info(dev, "Bus probe for function bus controller driver\n");
  dev_info(dev, "Parent dev name is %s\n", dev_name(dev->parent));
  no_of_modules = smartio_get_no_of_modules(node, node_name);
  dev_info(dev, "Node has %d modules\n", no_of_modules);
  dev_info(dev, "Name read from device is %s\n", node_name);

  if (no_of_modules < 0)
    return -EINVAL;

  for (i=1; i < no_of_modules; i++) {
    status = create_function_device(node, i);
    if (status) {
      dev_err(dev, "Failed creating function %d of %d\n", i, no_of_modules);
      return status;
    }
  }

  return status;
}

static int fcn_ctrl_remove(struct device* dev)
{
  int status;

  dev_info(dev, "Bus remove for function bus controller driver\n");
  dev_info(dev, "About to unregister child functions\n");
  status =  device_for_each_child(dev, NULL, dev_unregister_function);
  dev_info(dev, "Done unregistering child functions\n");

  return status;
}

static const struct smartio_device_id fcn_ctrl_table[] = {
  { "smartio_bus_master" },
  { NULL }
};

static struct smartio_driver fcn_ctrl_driver = {
  .driver = {
    .name = "smartio_bus_controller",
    .owner = THIS_MODULE,
    .bus = &smartio_bus,
    .probe = fcn_ctrl_probe,
    .remove = fcn_ctrl_remove,
  },
  .id_table = fcn_ctrl_table,
};


static int __init my_init(void)
{
  if (bus_register(&smartio_bus) < 0) {
    pr_err("smartio: Failed to register bus\n");
    goto fail_bus_register;
  }

#if 0
  if (class_register(&smartio_node_class) < 0) {
    pr_err("smartio: Failed to register node class\n");
    goto fail_node_class_register;
  }
#endif
  if (class_register(&smartio_function_class) < 0) {
    pr_err("smartio: Failed to register function class\n");
    goto fail_function_class_register;
  }

  work_queue = create_singlethread_workqueue(smartio_bus.name);
  if (work_queue == NULL) {
    pr_err("smartio: Failed to create workqueue\n");
    goto fail_workqueue;
  }

  if (driver_register(&fcn_ctrl_driver.driver) < 0) {
    pr_err("smartio: Failed to register function bus controller driver\n");
    goto fail_bus_driver;
  }

  major = register_chrdev(0, "smartio", &char_dev_fops);
  if (major < 0) {
    pr_err("smartio: Failed to allocate major number\n");
    goto fail_major_number;
  }

  pr_info("smartio: Done registering  bus driver\n");
  return 0;

 fail_major_number:
  driver_unregister(&fcn_ctrl_driver.driver);
 fail_bus_driver:
  destroy_workqueue(work_queue);
fail_workqueue:
  class_unregister(&smartio_function_class);
fail_function_class_register:
#if 0
  class_unregister(&smartio_node_class);
fail_node_class_register:
#endif
  bus_unregister(&smartio_bus);
fail_bus_register:
  return -1;
}
module_init(my_init);

static void __exit my_cleanup(void)
{
  unregister_chrdev(major, "smartio");
  driver_unregister(&fcn_ctrl_driver.driver);
  destroy_workqueue(work_queue);
  class_unregister(&smartio_function_class);
#if 0
  class_unregister(&smartio_node_class);
#endif
  bus_unregister(&smartio_bus);
  pr_warn("Removed smartio bus driver\n");
}
module_exit(my_cleanup);




MODULE_AUTHOR("Hans Odeberg <hans.odeberg@intel.com>");
MODULE_DESCRIPTION("SmartIO bus driver");
MODULE_LICENSE("GPL v2");

