obj-m := smartio_i2c.o
obj-m += smartio_core.o
smartio_core-y = smartio-core.o \
	         convert.o \
             	 txbuf_list.o \
		 minor_id.o \
		 comm_buf.o
obj-m += smartio_adc.o
obj-m += smartio_uart.o
obj-m += edison_smbus.o
ccflags-y := -DVERSION=$(VERSION) -DPATCHLEVEL=$(PATCHLEVEL)
