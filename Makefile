KDIR ?= /lib/modules/$$(uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD



dev_write: dev_write.c
	$(CC) -Wall $^ -o $@

dev_read: dev_read.c
	$(CC) -Wall $^ -o $@
