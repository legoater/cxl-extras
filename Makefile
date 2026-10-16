KDIR ?= /lib/modules/$(shell uname -r)/build
CC ?= gcc
CFLAGS ?= -Wall -O2

all: modules cxl_accel_test

modules:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

cxl_accel_test: cxl_accel_test.c cxl_type2.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f cxl_accel_test
