#include <stdio.h>
#include <termios.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

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

void unescape_buffer(unsigned char *buf, int size);
static void write_buf(int fd, unsigned char *buf, int size);
static int read_buf(int fd, unsigned char *buf, const unsigned int max_size);
static void dump_buf(const unsigned char *buf);
int read_no_of_modules(int fd);
int read_no_of_attributes(int fd, unsigned int module);

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

    (void) attrs;
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


#if 0
static void read_buf(int fd)
{
  unsigned char buf[100];
  unsigned int wr_ix = 0;
  unsigned int i;
  int bytes_to_read = 2;  // STX and size

  while (bytes_to_read > 0) {
    printf("About to read %d bytes.\n", bytes_to_read);
#if 1
    int bytes_read = read(fd, buf + wr_ix, bytes_to_read);
#else
    int bytes_read = read(fd, buf + wr_ix, 1);
#endif

    if (wr_ix == 0) {
      if (bytes_read < 2) {
	printf("Only read %d bytes 1st time.\n", bytes_read);
	return;
      }
      else {
	printf("Size is %d.\n", (int) buf[1]);
        bytes_to_read = buf[1];
      }
    }
    printf("Actual # of bytes: %d bytes.\n", bytes_read);
    if (bytes_read < 0) {
      perror("Failed to write message\n");
      return;
    }
    wr_ix += bytes_read;
    bytes_to_read = buf[1] - wr_ix + 2;
  }

  printf("Dumping read buffer:\n");
  for (i=0; i < wr_ix; i++)
    printf("%x ", (int) buf[i]);
  printf("\n\n");

  unescape_buffer(buf, wr_ix);

  printf("Dumping unescaped read buffer:\n");
  for (i=0; i < (buf[1]+2); i++)
    printf("%x ", (int) buf[i]);
  printf("\n\n");
}
#else
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
      printf("Read char %d at ix %d\n", (int) buf[wr_ix], wr_ix);
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

#endif

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