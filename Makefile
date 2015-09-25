KDIR ?= /lib/modules/$$(uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD



dev_write:
	$(CC) -Wall dev_write.c -o $@