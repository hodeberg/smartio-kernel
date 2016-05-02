KDIR ?= /lib/modules/$$(uname -r)/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD

clean:
	$(MAKE) -C $(KDIR) M=$$PWD $@



dev_write: dev_write.c
	$(CC) -Wall $^ -o $@

scanf_test: scanf_test.c
	$(CC) -Wall $^ -o $@

dev_read: dev_read.c
	$(CC) -Wall $^ -o $@

enable_smartio_line: enable_smartio_line.c
	$(CC) -Wall $^ -o $@

serio: serio.c
	$(CC) -Wall $^ -o $@

