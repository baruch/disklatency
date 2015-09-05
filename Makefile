ifneq ($(KERNELRELEASE),)
# kbuild part of makefile
obj-m  := scsi_host_sniffer_main.o
scsi_host_sniffer-m := scsi_host_sniffer.o

else
# normal makefile
KDIR ?= /lib/modules/`uname -r`/build

default:
	$(MAKE) -C $(KDIR) M=$$PWD

clean:
	rm -f *.o *.ko modules.order Module.symvers *.mod.c

endif
