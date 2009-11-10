obj-m += sony-laptop.o
#obj-m += nvidia_bl.o
obj-m += test.o

KDIR := /lib/modules/$(shell uname -r)
ifdef SYSSRC
 KERNEL_SOURCES := $(SYSSRC)
else
 ifdef SYSINCLUDE
 KERNEL_SOURCES := $(SYSINCLUDE)/..
 else
 KERNEL_SOURCES := $(shell test -d $(KDIR)/source && echo $(KDIR)/source || echo $(KDIR)/build)
 endif
endif
PWD  := $(shell pwd)

all: default

default:
	$(MAKE) -C $(KERNEL_SOURCES) SUBDIRS=$(PWD) modules

clean:
	$(MAKE) -C $(KERNEL_SOURCES) SUBDIRS=$(PWD) clean
	@rm -f modules.order
	@rm -f Module.markers
	@rm -f *~

install:
	mkdir -p $(KDIR)/updates/
	cp sony-laptop.ko $(KDIR)/updates/
	#cp nvidia_bl.ko $(KDIR)/updates/
	depmod -a

uninstall:
	rm $(KDIR)/updates/sony-laptop.ko
	#rm $(KDIR)/updates/nvidia_bl.ko
	rmmod sony-laptop
	#rmmod nvidia_bl
	depmod -a
	modprobe sony-laptop 

test:
	rmmod sony-laptop
	insmod sony-laptop.ko debug=1

dumpparams:
	rmmod test
	insmod test.ko
	dmesg > dump_dmesg.log
