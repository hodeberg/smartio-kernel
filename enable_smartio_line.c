#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <stdlib.h>

int fd;

void termination_handler(int signum)
{
  const int ldisc = 0;
  if (fd > 0) {
    ioctl(fd, TIOCSETD, &ldisc);
  }
  exit(0);
}


char fname[] = "/dev/ttyUSB0";


int main()
{
  const int ldisc = 28;
  const int ldisc_default = 0;
  struct sigaction new_action, old_action;

  new_action.sa_handler = termination_handler;
  sigemptyset(&new_action.sa_mask);
  new_action.sa_flags = 0;
  sigaction(SIGINT, NULL, &old_action);
  if (old_action.sa_handler != SIG_IGN)
    sigaction(SIGINT, &new_action, NULL);
  sigaction(SIGHUP, NULL, &old_action);
  if (old_action.sa_handler != SIG_IGN)
    sigaction(SIGHUP, &new_action, NULL);
  sigaction(SIGTERM, NULL, &old_action);
  if (old_action.sa_handler != SIG_IGN)
    sigaction(SIGTERM, &new_action, NULL);

  fd = open(fname, O_RDWR); 
  if (fd < 0) {
    printf("Failed to open device %s. Reason: %s",
	   fname,
	   strerror(errno));
    return 1;
  }
  ioctl(fd, TIOCSETD, &ldisc);
  for(;;) sleep(10);  

  if (fd > 0) {
    ioctl(fd, TIOCSETD, &ldisc_default);
  }
  return 0;
}
