#include <linux/device.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/workqueue.h>

#include <linux/idr.h>
#include <linux/slab.h>
#include "smartio.h"
#include "smartio_inline.h"
#include "convert.h"
#include "txbuf_list.h"

static void free_groups(struct attribute_group** groups);

struct fcn_dev {
  int function_ix;
  struct device dev;
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
  struct smartio_function_driver* driver = container_of(drv, struct smartio_function_driver, driver);
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


static DEFINE_MUTEX(id_lock);
static DEFINE_IDR(node_idr);
static DECLARE_WAIT_QUEUE_HEAD(wait_queue);



static void handle_response(struct smartio_comm_buf *resp)
{
  struct smartio_comm_buf *req;

  pr_info("Entering handle_response\n");

  req = smartio_find_transaction(smartio_get_transaction_id(resp));

  if (req) {
	req->data_len = resp->data_len;
	memcpy(req->data, resp->data, resp->data_len);
	pr_info("Copied %d bytes from resp to req\n", req->data_len);
	/* Swapping the direction tells waiter it needs to sleep
           no more. */
	smartio_set_direction(req, SMARTIO_FROM_NODE);
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
      dev_err(&node->dev, "Got a response message\n");
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

  pr_warn("HAOD: request work function\n");
  smartio_add_transaction(my_work->comm_buf);
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


static void smartio_write_16bit(struct smartio_comm_buf* buf, int ofs, int val)
{
  buf->data[ofs] = val >> 8;
  buf->data[ofs+1] = val;
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

#define SOME_LATER_KERNEL_VERSION
static int alloc_new_node_number(void *node)
{
  int id;

#ifdef SOME_LATER_KERNEL_VERSION
#if 0
  mutex_lock(&core_lock);
#endif
  id = idr_alloc(&node_idr, node, 1, 0, GFP_KERNEL);
#if 0
  mutex_unlock(&core_lock);
#endif
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
				  void *data)
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
					mybuf);
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
int get_no_of_groups(struct attr_info* info, int size)
{
  int i;
  int count = 1;

  for (i=0; i < size; i++)
    if (info[i].isDir) count++;

  return count;
}

int get_no_of_attrs_in_group(const struct attr_info *attr, const struct attr_info * const end)
{
	int count = 0;

	while ((attr != end) && (!attr->isDir)) {
	  count++;
	  attr++;
	}

	return count;
}


void free_group(struct attribute_group *grp)
{
	if (grp->attrs) {
		struct attribute** attr;

		for (attr = grp->attrs; *attr != NULL; attr++) {
			struct attribute *a = *attr;
			struct device_attribute* d;
				
			pr_warn("Free: attr name %s\n", a->name);
			kfree(a->name);
			d = container_of(a, struct device_attribute, attr);
			pr_warn("Free: attribute\n");
			kfree(d);
		}
		pr_warn("Free: group attrs\n");
		kfree(grp->attrs);
		grp->attrs = NULL;
		pr_warn("Free: group name %s\n", grp->name);
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


/* Debugging function to ensure we created the right structure */
void dump_group_tree(const struct attribute_group** groups)
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


/* Create attributes for one group. If start is not a directory definition,
   then this is the default group.
   Returns pointer to the attribute after this group. That can be:
      the definition of a new group
      the end pointer when we are done
      null for error */
const struct attr_info *process_one_attr_group(const struct attr_info *head, 
					       const struct attr_info *start, 
					       const struct attr_info * const end,
					       struct attribute_group * const grp)
{
	const struct attr_info *cur_attr = start;
	int i;
	int size = 0;

#if 0
	pr_warn("attr_group: start = %p\n", start);
	pr_warn("attr_group: end = %p\n", end);
	pr_warn("attr_group: grp = %p\n", grp);
#endif
	if (cur_attr->isDir) {
	  	grp->name = kmalloc(strlen(cur_attr->name) + 1, GFP_KERNEL);
		if (!grp->name) {
			pr_err("Failed to allocate attribute name %s\n", cur_attr->name);
			return NULL;
		}
		pr_warn("attr_group: found directory attribute\n");
		cur_attr++;
	}
	size = get_no_of_attrs_in_group(cur_attr, end);
#if 0
	pr_warn("attr_group: size = %d\n", size);
#endif
	grp->attrs = kzalloc((sizeof grp->attrs) * (size + 1), GFP_KERNEL);
	for (i=0; i < size; i++, cur_attr++) {
#if 0
	  pr_warn("attr_group: attr name: %s\n", cur_attr->name);
	  pr_warn("attr_group: attr input: %d\n", cur_attr->input);
	  pr_warn("attr_group: attr type: %d\n", cur_attr->type);
#endif
	  grp->attrs[i] = create_attr(cur_attr, head);
	  if (!grp->attrs[i]) {
	    pr_err("process_one_attr_group: Failed to create attribute\n");
	    goto cleanup;
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
static struct attribute_group** 
define_function_attrs(struct smartio_node* node,
		      int module,
		      int no_of_attributes)
{
#if 0
	struct attribute **attrs_a;
	struct attribute **attrs_b;
	struct attribute_group *grp1;
	struct attribute_group *grp2;
	char* grp_name;
#endif
	struct attribute_group **groups = NULL;
	struct attr_info info[no_of_attributes];
	const struct attr_info * const info_end = info + no_of_attributes;
	const struct attr_info *info_current = info;
	int i;
	int no_of_groups;	

	dev_warn(&node->dev, "define_function_attrs: entry\n");
	dev_warn(&node->dev, "define_function_attrs: attrs to process: %d\n", no_of_attributes);
	for (i=0; i < no_of_attributes; i++) {
	  int res;

	  dev_warn(&node->dev, "processing attr ix: %d\n", i);
	  res = smartio_get_attr_info(node, module, i, &info[i]);

	  if (res != 0) {
	    dev_err(&node->dev, "Failed reading attribute #%d\n", i);
	    return NULL;
	  }
	}

	no_of_groups = get_no_of_groups(info, no_of_attributes);
        dev_warn(&node->dev, "No of groups are: %d\n", no_of_groups);

	/* Allocate array for group pointers and ending null pointer */
	groups = kzalloc((sizeof *groups) * (no_of_groups+1), GFP_KERNEL);
	if (!groups) {
	  dev_err(&node->dev, "Failed to allocate groups array\n");
	  goto fail_alloc_groups;
	}

	dev_warn(&node->dev, "define_function_attrs: beginning to create groups\n");
	for (i=0; i < no_of_groups; i++) {
	  if (!info_current) {
	    dev_err(&node->dev, "Failed parsing group\n");
	    goto cleanup;
	  }
	  groups[i] = kzalloc(sizeof *groups[i], GFP_KERNEL);
	  if (!groups[i]) {
	    dev_err(&node->dev, "Failed allocating a group\n");
	    goto cleanup;
	  }
	  info_current = process_one_attr_group(info, info_current, info_end, groups[i]);
	}
	return groups;


#if 0
	/* Ignoring failure for now, this is not the real code */
	attrs_a = kzalloc((sizeof *attrs_a)*3, GFP_KERNEL);
	attrs_a[0] = create_attr("attr1", true);
	attrs_a[1] = create_attr("attr2", false);
	grp1 = kzalloc(sizeof *grp1, GFP_KERNEL);
	grp1->attrs = attrs_a;
	attrs_b = kzalloc((sizeof *attrs_b)*3, GFP_KERNEL);
	attrs_b[0] = create_attr("attr3", true);
	attrs_b[1] = create_attr("attr4", false);
	grp2 = kzalloc(sizeof *grp2, GFP_KERNEL);
	grp2->attrs = attrs_b;
	grp_name = kmalloc(strlen("subattrs")+1, GFP_KERNEL);
	strcpy(grp_name, "subattrs");
	grp2->name = grp_name;
	groups = kzalloc((sizeof *groups)*3, GFP_KERNEL);
	groups[0] = grp1;
	groups[1] = grp2;
#endif
 cleanup:
	free_groups(groups);
	kfree(groups);
	groups = NULL;
 fail_alloc_groups:
	return groups;
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
				
				pr_warn("Free: attr name %s\n", a->name);
				kfree(a->name);
				d = container_of(a, struct device_attribute, attr);
				pr_warn("Free: attribute\n");
				kfree(d);
			}
			pr_warn("Free: group attrs\n");
			kfree((*group)->attrs);
		}
		pr_warn("Free: group name %s\n", (*group)->name);
		kfree((*group)->name);
		pr_warn("Free: group attrs\n");
		kfree(*group);
	}
	pr_warn("Free: groups\n");
	kfree(dev->groups);
}
#endif

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
		dev_set_name(&function_dev->dev, function_name);
		function_dev->dev.bus = &smartio_bus;
		function_dev->dev.id = 0;
		//      function_dev->dev.class = &smartio_function_class;
		function_dev->dev.release = function_release;
		function_dev->dev.groups = (const struct attribute_group**)
		  define_function_attrs(node, 
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
	}
	else {
		dev_err(&node->dev,
			"Failed to query node for function %s info\n",
			function_name);
	}
	goto done;
	
release_dev:
	if (function_dev->dev.groups) {
	  free_groups((struct attribute_group**) function_dev->dev.groups);
	  kfree(function_dev->dev.groups);
	  function_dev->dev.groups = NULL;
	}
	put_device(&function_dev->dev);
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

	dev_warn(dev, "HAOD: entering register_node\n");
	device_initialize(&node->dev);
	node->dev.parent = dev;
	node->dev.bus = &smartio_bus;
	node->dev.type = &controller_devt;
	node->dev.id = alloc_new_node_number(&controller_devt);
	dev_info(dev, "HAOD: allocated node number %d\n", node->dev.id);
	if (node->dev.id < 0)
		return node->dev.id;

	dev_set_drvdata(dev, node);
	status = device_add(&node->dev);
	dev_info(dev, "HAOD: added node %s\n", dev_name(&node->dev));
	if (status < 0) {
		dev_err(dev, "HAOD: failed to add node device\n");
		put_device(&node->dev);
		return status;
	}
	dev_info(dev, "HAOD: Registered master %s\n", dev_name(&node->dev));

	return 0;
}

static int dev_unregister_function(struct device* dev, void* null)
{
	dev_warn(dev, "Unregistering function %s\n", dev_name(dev));
	device_unregister(dev);
	return 0;
}

/* dev points to function bus controller device */
int smartio_unregister_node(struct device *dev, void* null)
{
	int status;

	dev_warn(dev, "Unregistering function bus controller node\n");
	dev_warn(dev, "First step: Unregistering child functions\n");
	status = device_for_each_child(dev, NULL, dev_unregister_function);

	dev_warn(dev, "2nd step: Unregistering node itself\n");
	device_unregister(dev);
	pr_warn("HAOD: Unregistering done.\n");
	return status;
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

  dev_warn(dev, "HAOD: about to register based on %s\n", dev_name(dev));

  node = kzalloc(sizeof *node, GFP_KERNEL);
  if (!node) 
    return -ENOMEM;
  dev_warn(dev, "HAOD: allocated node mem\n");

  node->communicate = cb;
  ret = smartio_register_node(dev, node, name);
  if (ret) {
    dev_warn(dev, "HAOD: dev_smartio_register_node failed\n");
    goto reclaim_node_memory;
  }
  dev_warn(dev, "HAOD: dev_smartio_register_node successful\n");


  return ret;
reclaim_node_memory:
  kfree(node);
  return ret;
}
EXPORT_SYMBOL_GPL(dev_smartio_register_node);


int smartio_add_driver(struct smartio_function_driver* sd)
{
  sd->driver.bus = &smartio_bus;
  return driver_register(&sd->driver);
}
EXPORT_SYMBOL_GPL(smartio_add_driver);

void smartio_del_driver(struct smartio_function_driver* sd)
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
  dev_info(dev, "HAOD: parent dev name is %s\n", dev_name(dev->parent));
  no_of_modules = smartio_get_no_of_modules(node, node_name);
  dev_info(dev, "HAOD: Node has %d modules\n", no_of_modules);
  dev_info(dev, "HAOD: Name read from device is %s\n", node_name);

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
  dev_info(dev, "Bus remove for function bus controller driver\n");
  return 0;
}


static const struct smartio_device_id fcn_ctrl_table[] = {
  { "smartio_bus_master" },
  { NULL }
};

static struct smartio_function_driver fcn_ctrl_driver = {
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

  work_queue = alloc_workqueue(smartio_bus.name, WQ_UNBOUND, 1);
  if (work_queue == NULL) {
    pr_err("smartio: Failed to create workqueue\n");
    goto fail_workqueue;
  }

  if (driver_register(&fcn_ctrl_driver.driver) < 0) {
    pr_err("smartio: Failed to register function bus controller driver\n");
    goto fail_bus_driver;
  }

  pr_info("smartio: Done registering  bus driver\n");
  return 0;

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

