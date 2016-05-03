#include <linux/device.h>


// Put the following in include/linux/mod_devicetable.h
/* smartio */ 
#define SMARTIO_NAME_SIZE 20
#define SMARTIO_MODULE_PREFIX "smartio:"


struct smartio_device_id {
  char *name;
};
// End of what to put in mod_devicetable.h


struct smartio_comm_buf;

struct smartio_driver {
	struct device_driver driver;
	const struct smartio_device_id *id_table;
};
#define to_smartio_driver(d) container_of(d, struct smartio_driver, driver)

int smartio_add_driver(struct smartio_driver* sd);
void smartio_del_driver(struct smartio_driver* sd);

struct smartio_node {
  struct device dev;
  // Send a message, and receive one.
  // tx may be null, in which case the remote node is polled.
  // rx may be empty, if remote node returned no data.
  int (*communicate)(struct smartio_node* this, 
		     struct smartio_comm_buf* tx,
		     struct smartio_comm_buf* rx);
};

#define to_node(d) container_of(d,struct smartio_node, dev)

int devm_smartio_register_node(struct device *dev);
int dev_smartio_register_node(struct device *dev, 
			      char* name,
			      int (*communicate)(struct smartio_node* this, 
						 struct smartio_comm_buf* tx,
						 struct smartio_comm_buf* rx));
/* dev: the function bus controller to unregister */
int smartio_unregister_node(struct device *dev, void* null);


int smartio_get_no_of_modules(struct smartio_node* node, char* name);



enum smartio_io_types {
  IO_ZERO,
  IO_AMPERE, /* 1 Ampere */
  IO_MILLIAMPERE,  /* 2 milli-Ampere */
  IO_ANGLE_RADIAN,  /* 3 angle */
  IO_ANGULAR_VELOCITY,  /* 4 angular velocity */
  IO_THERMAL_KBTU,  /* 5 thermal energy */
  IO_THERMAL_MBTU,  /* 6 thermal energy */
  IO_ASCII_CHAR,  /* 7 ASCII char */
  IO_ABSOLUTE_COUNT,  /* 8 absolute count */
  IO_INCREMENTAL_COUNT,  /* 9 incremental count */
  IO_10,  /* 10 obsolete! */
  IO_DAY_OF_WEEK,  /* 11 day of week (-1 = invalid) */
  IO_OBSOLETE,  /* 12 obsolete! */
  IO_ENERGY_KWH,  /* 13 kilowatt hours */
  IO_ENERGY_WH,  /* 14 Watt hours */
  IO_FLOW_LPS,  /* 15 flow volume */
  IO_FLOW_MLPS,  /* 16 flow volume */
  IO_LENGTH_METER,  /* 17 length */
  IO_LENGTH_KILOMETER,  /* 18 length */
  IO_LENGTH_MICROMETER,  /* 19 length */
  IO_LENGTH_MILLIMETER,  /* 20 length */
  IO_LEVEL_PERCENT,  /* 21 continuous level */
  IO_22,  /* 22 obsolete! */
  IO_MASS_GRAM,  /* 23 mass */
  IO_MASS_KILOGRAM,  /* 24 mass */
  IO_MASS_TON,  /* 25 mass */
  IO_MASS_MILLIGRAM,  /* 26 mass */
  IO_POWER_WATT,  /* 27 power */
  IO_POWER_KILOWATT,  /* 28 power */
  IO_CONCENTRATION_PPM,  /* 29 concentration in ppm */
  IO_PRESSURE_KPA,  /* 30 pressure */
  IO_RESISTANCE_OHM,  /* 31 resistance */
  IO_RESISTANCE_KOHM,  /* 32 resistance */
  IO_ASCII_STRING,     /* 33 ascii string */
  /*  IO_TEMP_C,
      IO_TEMP_K,*/
};



enum smartio_status {
  SMARTIO_SUCCESS,
  SMARTIO_ILLEGAL_MODULE_INDEX,
  SMARTIO_ILLEGAL_ATTRIBUTE_INDEX,
  SMARTIO_ILLEGAL_ARRAY_INDEX,
  SMARTIO_NO_PERMISSION,
};

/* The attribute definition */
#define IO_IS_INPUT 0x80
#define IO_IS_OUTPUT 0x40
#define IO_IS_DEVICE 0x20
#define IO_IS_DIR 0x10
#define ATTR_MAX_PAYLOAD (SMARTIO_DATA_SIZE - 5)


extern struct bus_type smartio_bus;
