#include <stdio.h>
#include <termios.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>

#include "convert.h"

#define STX 2
#define ETX 3
#define ESC 27

enum Code {
  REQ,
  IND,
  ACK,
  NAK
};


enum Cmd {
  SMARTIO_GET_NO_OF_MODULES = 1,
  SMARTIO_GET_NO_OF_ATTRIBUTES,
  SMARTIO_GET_ATTRIBUTE_DEFINITION,
  SMARTIO_GET_ATTR_VALUE,
  SMARTIO_SET_ATTR_VALUE,
  SMARTIO_GET_STRING
};

/* message format:
STX
size (from this byte up to but not including ETX)
header
data payload
CRC-16, MSB first
ETX
*/

/* The attribute definition */
#define IO_IS_INPUT 0x80
#define IO_IS_OUTPUT 0x40
#define IO_IS_DEVICE 0x20
#define IO_IS_DIR 0x10

struct dev_attr_info {
  uint8_t flags; /*  */
  uint8_t arraySize;
  uint8_t type;
};

void unescape_buffer(unsigned char *buf, int size);
static void write_buf(int fd, unsigned char *buf, int size);
static int read_buf(int fd, unsigned char *buf, const unsigned int max_size);
static void dump_buf(const unsigned char *buf);
int read_no_of_modules(int fd);
int read_no_of_attributes(int fd, unsigned int module);
void read_attr_def(int fd, unsigned int module, unsigned int attr_ix, struct dev_attr_info *attr);
void read_attr_value(int fd, unsigned int module, 
		     unsigned int attr_ix, unsigned int arr_ix,
		     unsigned int type);

char *serport = "/dev/ttyUSB0";
int trans_id = 4;

int main(int argc, char *argv[])
{
  struct termios settings;

  const int fd = open(serport, O_RDWR);
  int modules;
  int i;

  if (fd < 0) {
    printf("Failed to open %s due to: %s\n", serport, strerror(errno));
    goto failed_open;
  }
  if (tcgetattr(fd, &settings) < 0) {
    perror("Failed to get attributes\n");
    goto failed_attr;
  }
  printf("before: c_lflag = %x, c_cflag = %x\n", settings.c_lflag, settings.c_cflag);
  if (cfsetspeed(&settings, B9600) < 0) {
    perror("Failed to set attributes\n");
    goto failed_attr;
  }
  settings.c_cflag |= CLOCAL;
  settings.c_cflag &= ~CSTOPB;
  settings.c_cflag &= ~CSIZE;
  settings.c_cflag |= CS8;
  printf("after: c_lflag = %x, c_cflag = %x\n", settings.c_lflag, settings.c_cflag);
  if (tcsetattr(fd, TCSANOW, &settings) < 0) {
    perror("Failed to set attributes\n");
    goto failed_attr;
  }

  modules = read_no_of_modules(fd);

  for (i=0; i < modules; i++) {
    const int attrs = read_no_of_attributes(fd, i);
    int j;

    for (j=0; j < attrs; j++) {
      struct dev_attr_info attr;

      read_attr_def(fd, i, j, &attr);
      read_attr_value(fd, i, j, 0xFF, attr.type);
    }
  }


 failed_attr:
  close(fd);
 failed_open:
  return 0;
}


int read_no_of_modules(int fd)
{
  unsigned char get_no_of_modules[] = {
    0,
    (REQ << 4),
    0, // MODULE 0
    SMARTIO_GET_NO_OF_MODULES
  };
  int isOK;
  char node_name[20];
  unsigned char readbuf[100];
  int no_of_modules = -1;

  printf("Sending no_of_modules request.\n");
  write_buf(fd, get_no_of_modules, sizeof get_no_of_modules);
  isOK = read_buf(fd, readbuf, sizeof readbuf);
    printf("read status: %s\n", isOK ? "OK" : "FAIL");
  if (isOK) {
    const int cmd_status = readbuf[2];

    if (cmd_status == 0) {
      const int name_size = readbuf[0]- 7;

      no_of_modules = (readbuf[3] << 8) + readbuf[4];
      printf("name length: %d\n", name_size);
      strncpy(node_name, (char*)readbuf + 5, name_size);
      node_name[name_size] = '\0';
      printf("name : %s\n", node_name);
      printf("number of functions: %d\n", no_of_modules);
    }
    else {
      printf("Cmd failure result: %d\n", cmd_status);
      dump_buf(readbuf);
    }
  }
  return no_of_modules;
}

int read_no_of_attributes(int fd, unsigned int module)
{
  unsigned char req[] = {
    0,
    (REQ << 4),
    module,
    SMARTIO_GET_NO_OF_ATTRIBUTES
  };
  int isOK;
  char name[20];
  unsigned char readbuf[100];
  int no_of_attrs = -1;

  printf("Sending no_of_attrs request for function %d.\n", module);
  write_buf(fd, req, sizeof req);
  isOK = read_buf(fd, readbuf, sizeof readbuf);
    printf("read status: %s\n", isOK ? "OK" : "FAIL");
  if (isOK) {
    const int cmd_status = readbuf[2];

    if (cmd_status == 0) {
      const int name_size = readbuf[0]- 7;

      no_of_attrs = (readbuf[3] << 8) + readbuf[4];
      printf("name length: %d\n", name_size);
      strncpy(name, (char*)readbuf + 5, name_size);
      name[name_size] = '\0';
      printf("name : %s\n", name);
      printf("number of functions: %d\n", no_of_attrs);
    }
    else {
      printf("Cmd failure result: %d\n", cmd_status);
      dump_buf(readbuf);
    }
  }
  return no_of_attrs;
}


void read_attr_def(int fd, unsigned int module, unsigned int attr_ix, struct dev_attr_info *attr)
{
  unsigned char req[] = {
    0,
    (REQ << 4),
    module,
    SMARTIO_GET_ATTRIBUTE_DEFINITION,
    attr_ix >> 8,
    attr_ix
  };
  int isOK;
  char name[20];
  unsigned char readbuf[100];

  printf("Sending attrdef request for function %d, attr %d.\n", module, attr_ix);
  write_buf(fd, req, sizeof req);
  isOK = read_buf(fd, readbuf, sizeof readbuf);
    printf("read status: %s\n", isOK ? "OK" : "FAIL");
  if (isOK) {
    const int cmd_status = readbuf[2];

    if (cmd_status == 0) {
      const int name_size = readbuf[0]- 8;

      attr->flags = readbuf[3];
      attr->arraySize = readbuf[4];
      attr->type = readbuf[5];
      printf("input bit: %c\n", (attr->flags & IO_IS_INPUT) ? '1' : '0');
      printf("output bit: %c\n", (attr->flags & IO_IS_OUTPUT) ? '1' : '0');
      printf("device bit: %c\n", (attr->flags & IO_IS_DEVICE) ? '1' : '0');
      printf("dir bit: %c\n", (attr->flags & IO_IS_DIR) ? '1' : '0');
      printf("Array size: %d\n", (int) attr->arraySize);
      printf("Type: %d\n", (int) attr->type);

      printf("name length: %d\n", name_size);
      strncpy(name, (char*)readbuf + 6, name_size);
      name[name_size] = '\0';
      printf("name : %s\n", name);
    }
    else {
      printf("Cmd failure result: %d\n", cmd_status);
      dump_buf(readbuf);
    }
  }
}


void read_attr_value(int fd, unsigned int module,
		     unsigned int attr_ix, unsigned int arr_ix,
		     unsigned int type)
{
  unsigned char req[] = {
    0,
    (REQ << 4),
    module,
    SMARTIO_GET_ATTR_VALUE,
    attr_ix >> 8,
    attr_ix,
    arr_ix
  };
  int isOK;
  unsigned char readbuf[100];

  printf("Sending attr value request for function %d, attr %d, arr_ix %d.\n", 
	 module, attr_ix, arr_ix);
  write_buf(fd, req, sizeof req);
  isOK = read_buf(fd, readbuf, sizeof readbuf);
    printf("read status: %s\n", isOK ? "OK" : "FAIL");
  if (isOK) {
    const int cmd_status = readbuf[2];
    char value[20];

    if (cmd_status == 0) {
      const int data_size = readbuf[0] - 5;
      int i;

      printf("Data size: %d\nData (hex): ", data_size);
      for(i=0; i < data_size; i++)
	printf("%x ", (int) readbuf[i+3]);
      printf("\n");
      smartio_raw_to_string(type, readbuf + 3, value);
      value[strlen(value)-1] = '\0'; // Get rid of \n at the end
      printf("Converted value: '%s'\n", value);
    }
    else {
      printf("Cmd failure result: %d\n", cmd_status);
      dump_buf(readbuf);
    }
  }
}




static void dump_buf(const unsigned char *buf)
{
  int i;

  printf("Dumping read buffer:\n");
  for (i=0; i < buf[0]; i++)
    printf("%d: %x (%c)\n", i, (int) buf[i], buf[i]);
  printf("\n\n");
}


static void write_buf(int fd, unsigned char *buf, int size)
{
  int bytes_written;
  int i;
  unsigned char escaped_buf[100];
  unsigned char *dest = escaped_buf;


  buf[1] |= trans_id;
  trans_id = (trans_id + 1) % 16;
  *dest++ = STX;
  for (i=0; i < size; i++) {
    if ((buf[i] == STX) ||  (buf[i] == ETX) ||  (buf[i] == ESC)) {
      *dest++ = ESC;
      *dest++ = buf[i] + 0x80;
    }
    else 
      *dest++ = buf[i];
  }
  *dest++ = 0x22; // CRC for now
  *dest++ = 0x11;
  *dest++ = ETX;
  escaped_buf[1] = dest - escaped_buf - 2;
  bytes_written = write(fd, escaped_buf, dest - escaped_buf);
  if (bytes_written != (dest - escaped_buf)) {
    perror("Failed to write message\n");
  }
}


static int read_buf(int fd, unsigned char *buf, const unsigned int max_size)
{
  unsigned int wr_ix = 0;
  int hunting = 1;
  int escaping = 0;

  printf("Scanning for STX\n");
  while (1) {
    const int bytes_read = read(fd, buf + wr_ix, 1);

    if (bytes_read != 1) { 
      printf("Error return %d, restarting scanning for STX.\n", bytes_read);
      perror("Error reading byte\n");
      hunting = 1;
      continue;
    }
    if (hunting) {
      if (buf[wr_ix] == STX) {
	printf("Found STX!\n");
	hunting = 0;
      }
    }
    else {
#if 0
      printf("Read char %d at ix %d\n", (int) buf[wr_ix], wr_ix);
#endif
      switch (buf[wr_ix]) {
      case STX:
      printf("Found STX in mid-stream offset %d, resetting.\n", wr_ix);
	wr_ix = 0;
	break;
      case ESC:
	escaping = 1;
	break;
      case ETX:
	return buf[0] == wr_ix;
	break;
      default:
	if (escaping) {
	  buf[wr_ix] -= 0x80;
	  escaping = 0;
	  buf[0]--;
	}
	wr_ix++;
	if (wr_ix >= max_size) {
	  wr_ix = 0;
	  hunting = 1;
	  escaping = 0;
	  printf("Read beyond buffer size, starting to hunt again.\n");
	}
	break;
      }
    }
  }
}

void unescape_buffer(unsigned char *buf, int size)
{
  int wr_ix = 2;
  int rd_ix = 2;
  int newSize = buf[1];

  while ((rd_ix+1) < size) {
    if ((buf[rd_ix] == STX) ||  (buf[rd_ix] == ETX) ||  (buf[rd_ix] == ESC)) {
      rd_ix++;
      newSize--;
      buf[wr_ix++] = buf[rd_ix++] - 0x80;
    }
    else 
      buf[wr_ix++] = buf[rd_ix++];
  }
  buf[1] = newSize;
}
