/* Contains data and functions for converting between smartio binary values and 
   the strings used by sysfs 

How do we convert?

Consider voltage encoded in a raw 12-bit value [0..4095], 
where 0V is at 2048, -10V at 0 and +10V at 4095. To convert this to a floating-point
value:

float = (((float)raw) - 2048) * (20.0/4096.0)

Which results in:
offset = 2048
scale = 4096 / 20 = 1024

and the formula

float = (((float)raw) - offset) / scale


And the reverse operation will be:
raw = (unsigned int)(float*scale + offset)


Note that many type definitions have been borrowed from here:
http://www.lonmark.org/technical_resources/resource_files/snvt.pdf

*/


#include <linux/types.h>
#include <linux/string.h>
#include <linux/kernel.h>
#include "smartio.h"
#include "convert.h"

struct scaled_int {
  uint8_t no_of_bytes; /* 1, 2 or 4 */
  int8_t scale_power;
  uint32_t offset;
  char *unit;
};


const struct scaled_int variables[] = {
  {0, 0, 0, NULL},  /* null */
  {2, 1, 32768, "A"},  /* 1 Ampere */
  {2, 1, 32768, "mA"},  /* 2 milli-Ampere */
  {2, 3, 0, "radians"},      /* 3 angle */
  {2, 1, 32768, "radians/s"},  /* 4 angular velocity */
  {2, 0, 0, "kBTU"},  /* 5 thermal energy */
  {2, 0, 0, "MBTU"},  /* 6 thermal energy */
  {1, 0, 0, NULL},  /* 7 ASCII char */
  {2, -2, 0, NULL},  /* 8 absolute count */
  {2, 0, 32768, NULL},  /* 9 incremental count */
  {0, 0, 0, NULL},  /* 10 obsolete! */
  {1, 0, 1, NULL},  /* 11 day of week (-1 = invalid) */
  {0, 0, 0, NULL},  /* 12 obsolete! */
  {2, 0, 0, "kWh"},  /* 13 kilowatt hours */
  {2, 1, 0, "Wh"},  /* 14 Watt hours */
  {2, 0, 0, "l/s"},  /* 15 flow volume */
  {2, 0, 0, "ml/s"},  /* 16 flow volume */
  {2, 1, 0, "m"},  /* 17 length */
  {2, 1, 0, "km"},  /* 18 length */
  {2, 1, 0, "um"},  /* 19 length */
  {2, 1, 0, "mm"},  /* 20 length */
  {1, 0, 0, "%"},  /* 21 continuous level */
  {0, 0, 0, NULL},  /* 22 obsolete! */
  {2, 1, 0, "g"},  /* 23 mass */
  {2, 1, 0, "kg"},  /* 24 mass */
  {2, 1, 0, "tons"},  /* 25 mass */
  {2, 1, 0, "mg"},  /* 26 mass */
  {2, 1, 0, "W"},  /* 27 power */
  {2, 1, 0, "kW"},  /* 28 power */
  {2, 0, 0, "ppm"},  /* 29 concentration in ppm */
  {2, 1, 32768, "kPa"},  /* 30 pressure */
  {2, 1, 0, "Ohm"},  /* 31 resistance */
  {2, 1, 0, "kOhm"},  /* 32 resistance */
  {31, 0, 0, NULL},  /* 33 ASCII string */
};



static int buf2value(int no_of_bytes, u8* raw_value)
{
  unsigned int r = raw_value[0];
  int i;

  pr_warn("%s: raw value 1 is %u\n", __func__, r);
  if ((no_of_bytes != 1) && (no_of_bytes != 2) && (no_of_bytes != 4)) {
    pr_err("%s: wrong byte size %d\n", __func__, no_of_bytes);
    return -1;
  }

  for (i = 1; i < no_of_bytes; i++) {
    pr_warn("%s: raw value %d is %d\n", __func__, i+1, raw_value[i]);
    r = r * 256 + raw_value[i];
    pr_warn("%s: total value is %d\n", __func__, r);
  }
    
  return r;
}


static void int2buf(u8* raw, int v, int bytes)
{
  switch (bytes) {
  case 1:
    *raw = v;
    break;
  case 2:
    *raw++ = v >> 8;
    *raw = v & 0xFF;
    break;
  case 4:
    *raw++ = v >> 24;
    *raw++ = v >> 16;
    *raw++ = v >> 8;
    *raw = v;
    break;
  }
}

void smartio_raw_to_string(int ix, void* raw_value, char *result)
{
  const struct scaled_int * const def = &variables[ix];
  int v;

  pr_info("Converting index %d\n", ix);
  switch (ix) {
  case IO_LEVEL_PERCENT:
    /* Multiplier of 0.5 needs special handling */
    v = buf2value(2, raw_value);
    sprintf(result, "%d %s\n", v / 2, def->unit);
    break;
  case IO_ASCII_STRING:
    /* Text string */
    strcpy(result, raw_value);
    strcat(result, "\n");
    break;    
  default:
    v = buf2value(def->no_of_bytes, raw_value);
    pr_warn("%s: raw value is %d\n", __func__, v);
    v -= def->offset;
    pr_warn("%s: removing offset %d results in %d\n", __func__, (int) def->offset, v);
    pr_warn("%s: scale power is %d\n", __func__, (int) def->scale_power);
    if (def->scale_power > 0) {
      int i;
      int integral;
      int fraction;
      int divide_by = 1;

      for (i = 0; i < def->scale_power; i++) {
	divide_by  *= 10;
      }
      pr_warn("%s: divisor is %d\n", __func__, divide_by);
      integral = v / divide_by;
      fraction = abs(v % divide_by);
      pr_warn("%s: integral is %d\n", __func__, integral);
      pr_warn("%s: fraction is %d\n", __func__, fraction);
      if (def->unit) {
	pr_warn("%s: unit %s\n", __func__, def->unit);
	sprintf(result, "%d%s%d %s\n", integral, ".", fraction, def->unit);
      }
      else {
	pr_warn("%s: no unit\n", __func__);
	sprintf(result, "%d%s%d\n", integral, ".", fraction);
      }
    }
    else if (def->scale_power < 0) {
      int i;

      for (i = 0; i < -def->scale_power; i++) {
	v *= 10;
      }
      pr_warn("%s: multiplied value is %d\n", __func__, v);
      if (def->unit) {
	pr_warn("%s: unit %s\n", __func__, def->unit);
	sprintf(result, "%d %s\n", v, def->unit);
      }
      else {
	pr_warn("%s: no unit\n", __func__);
	sprintf(result, "%d\n", v);
      }
    }
    else {
      if (def->unit) {
	pr_warn("%s: unit %s\n", __func__, def->unit);
	sprintf(result, "%d %s\n", v, def->unit);
      }
      else {
	pr_warn("%s: no unit\n", __func__);
	sprintf(result, "%d\n", v);
      }
    }
  }
}


void smartio_string_to_raw(int ix, const char* str, char *raw, int *raw_len)
{
  const struct scaled_int * const def = &variables[ix];
  char *dot_pos = strchr(str, '.');
  int v;
  u8 v8;
  int result;
  int i;
  char buf[30];
  int dot_scaling;
  int total_scaling;

  strcpy(buf, str);
  dot_pos = strchr(str, '.');
  dot_scaling = dot_pos ? strlen(str) - (dot_pos-str) - 1 : 0;
  total_scaling = def->scale_power - dot_scaling;

  switch (ix) {
  case IO_LEVEL_PERCENT:
    /* Multiplier of 0.5 needs special handling */
    if (dot_pos) buf[dot_pos - str] = '\0';
    result = kstrtou8(buf, 10, &v8);
    *raw = v8;
    *raw_len = 1;
    break;
  case IO_ASCII_STRING:
    /* Text string */
    strcpy(raw, str);
    *raw_len = strlen(raw);
    break;    
  default:
    if (dot_pos) {
      strcpy(buf+(dot_pos-str), dot_pos+1);
    }
    result = kstrtoint(buf, 10, &v);

    if (total_scaling > 0) {      
      for (i = 0; i < total_scaling; i++)
	v *= 10;
    }
    else if (total_scaling < 0) {
      for (i = 0; i < -total_scaling; i++)
	v /= 10;
    }
    int2buf(raw, v, def->no_of_bytes);
    *raw_len = def->no_of_bytes;
    break;
  }
}


void write_val_to_buffer(char *buf, int *len, int type, union val value)
{
  if (type == IO_ASCII_STRING) {
    *len = strlen(value.str) + 1;  /* Send null byte too */
    strcpy(buf, value.str);
  }
  else {
    const struct scaled_int * const def = &variables[type];
    int bytes = def->no_of_bytes;

    int2buf(buf, value.intval, bytes);
    *len = bytes;
  }
}
EXPORT_SYMBOL_GPL(write_val_to_buffer);

