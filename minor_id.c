#include <linux/mutex.h>
#include <linux/bitops.h>

#include "minor_id.h"

#define NO_OF_BITS 256

static DECLARE_BITMAP(minorId, NO_OF_BITS);
static DEFINE_MUTEX(lock);


int get_minor_number(void)
{
  int newId;

  mutex_lock(&lock);
  newId = find_first_zero_bit(minorId, NO_OF_BITS);

  if (newId == NO_OF_BITS)
  {
    pr_err("There are no free minor number IDs\n");
    newId = -1;
    goto done;
  }
  set_bit(newId, minorId);

 done:
  mutex_unlock(&lock);
  return newId;
}

int release_minor_number(int id)
{
  int status = id;

  mutex_lock(&lock);
  if (!test_and_clear_bit(id, minorId)) {
    pr_err("Failed to release unclaimed minor number %d\n", id);
    status = -1;
  }
  mutex_unlock(&lock);
  return status;
}
