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
  
  dev_warn(&node->dev, "Read attribute def\n");
  dev_warn(&node->dev, "buf len: %d\n", buf->data_len);
  dev_warn(&node->dev, "flags: %d\n", buf->data[1]);
  dev_warn(&node->dev, "arr_size: %d\n", buf->data[2]);
  dev_warn(&node->dev, "type: %d\n", buf->data[3]);
  dev_warn(&node->dev, "name: %s\n", info->name);
  return 0;
}

static size_t show_fcn_attr(struct device *dev,
			    struct device_attribute *attr,
			    char *buf)
{
	dev_warn(dev, "Calling show fcn for attr %s\n", 
		 attr->attr.name);
	return scnprintf(buf, PAGE_SIZE, "%d\n", 5);
}

static size_t store_fcn_attr(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf,
			    size_t count)
{
	char mybuf[20];
	
	snprintf(mybuf, (int) min(count, sizeof mybuf - 1), "%s", buf);
	dev_warn(dev, "Calling store fcn for attr %s\n, value: %s", 
		 attr->attr.name,
		 mybuf);
	return count;
}

			    
EXPORT_SYMBOL_GPL(smartio_get_function_info);

/* All attributes have to be allocated dynamically, as we do not
   know in advance which attributes there are. This is a bit
   unorthodox, leading to the usual DEVICE_ATTR macro being
   a bit unusable.
   To define a set of attributes in their own directory, just
   create an additional attribute group with a name (which will
   be the name of the directory). */
   
static struct attribute *create_attr(const char *name, bool readonly)
{
	char* name_cpy;
	DEVICE_ATTR(ro,0444, show_fcn_attr, NULL);
	DEVICE_ATTR(rw,0644, show_fcn_attr, store_fcn_attr);
	struct device_attribute *dev_attr = kmalloc(sizeof *dev_attr, GFP_KERNEL);
	
	if (!dev_attr)
		return NULL;
	*dev_attr = readonly ? dev_attr_ro : dev_attr_rw;
	name_cpy = kmalloc(strlen(name)+1, GFP_KERNEL);
	if (!name_cpy)
		goto release_attr;
	strcpy(name_cpy, name);
	dev_attr->attr.name = name_cpy;
	return &dev_attr->attr;
	
release_attr:
	kfree(dev_attr);
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


void free_groups(struct attribute_group** groups)
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
const struct attr_info *process_one_attr_group(const struct attr_info *start, 
					       const struct attr_info * const end,
					       struct attribute_group * const grp)
{
	const struct attr_info *cur_attr = start;
	int i;
	int size = 0;

	pr_warn("attr_group: start = %p\n", start);
	pr_warn("attr_group: end = %p\n", end);
	pr_warn("attr_group: grp = %p\n", grp);
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
	pr_warn("attr_group: size = %d\n", size);
	grp->attrs = kzalloc((sizeof grp->attrs) * (size + 1), GFP_KERNEL);
	for (i=0; i < size; i++, cur_attr++) {
	  pr_warn("attr_group: attr name: %s\n", cur_attr->name);
	  pr_warn("attr_group: attr input: %d\n", cur_attr->input);
	  grp->attrs[i] = create_attr(cur_attr->name, cur_attr->input == 0);
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
	  info_current = process_one_attr_group(info_current, info_end, groups[i]);
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
	struct device* function_dev = NULL;
	
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
		dev_warn(&node->dev, "Function name is %s\n", function_name);
		dev_warn(&node->dev, "Function has %d attributes\n",
			 no_of_attributes);

		function_dev->parent = &node->dev;
		dev_set_name(function_dev, function_name);
		function_dev->bus = &smartio_bus;
		function_dev->id = 0;
		//      function_dev->class = &smartio_function_class;
		function_dev->release = function_release;
		function_dev->groups = (const struct attribute_group**)
		  define_function_attrs(node, 
					function_ix,
					no_of_attributes);
		if (!function_dev->groups) {
			dev_err(&node->dev,
				"Could not define function attrs\n");
			goto release_memory;
		}

		dump_group_tree(function_dev->groups);
#if 0
		function_dev->groups = NULL;
#endif
		status = device_register(function_dev);
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
	if (function_dev->groups) {
	  free_groups((struct attribute_group**) function_dev->groups);
	  kfree(function_dev->groups);
	  function_dev->groups = NULL;
	}
	put_device(function_dev);
release_memory:
	kfree(function_dev);
done:
	return status;
}


static int smartio_register_node(struct device *dev, struct smartio_node *node, char *name)
{
	int status = -1;
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
		status = create_function_device(node, i);
		if (!status) 
			goto done;
	}
	return 0;

done:
	return status;
}

static int dev_unregister_function(struct device* dev, void* null)
{
	struct smartio_device *function;
	
	dev_warn(dev, "Unregistering function %s\n", dev_name(dev));
	function = container_of(dev, struct smartio_device, dev); 
	device_unregister(dev);
	free_groups((struct attribute_group**) dev->groups);
	kfree(dev->groups);
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

