#include <stdio.h>
#include <string.h>
#include <linux/i2c-dev.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <unistd.h>
#include <ctype.h>


void dump_rx(char *buf)
{
  int i;

  for (i=0; i < 16; i++) {
    printf("%x %c\n", buf[i], isprint(buf[i]) ? (char) buf[i] : ' ');
  }
}


static char rbuf[32];

int main()
{
  char *dev = "/dev/i2c-6";
  const  int fd = open(dev, O_RDWR);
  const int addr = 0x48;
  char wbuf[] = { 4, 4, 0, 1 };
  const int rc = 16;
  int res;

  if (fd < 0) {
    printf("Darn. Could not open device %s, due to %s\n",
	   dev, strerror(errno));
    return 1;
  }
  if (ioctl(fd, I2C_SLAVE, addr) < 0) {
    printf("Darn. Could not set slave address %d, due to %s\n",
	   addr, strerror(errno));
    return 1;
  }
  res = write(fd, wbuf, sizeof wbuf);
  if (res < 0) {
    printf("Darn. Could not write data. Error is  %s\n", strerror(errno));
    return 1;
  }
  else if (res < sizeof wbuf) {
    printf("Darn. Only wrote %d bytes, expected  %d\n", 
	   res, sizeof wbuf);
    return 1;
  }
  res = read(fd, rbuf, rc);
  if (res < 0) {
    printf("Darn. Could not read data. Error is  %s\n", strerror(errno));
    return 1;
  }
  else if (res < rc) {
    printf("Darn. Only read %d bytes, expected  %d\n", 
	   res, sizeof rbuf);
    return 1;
  }
  dump_rx(rbuf);

  close(fd);

  return 0;
}
