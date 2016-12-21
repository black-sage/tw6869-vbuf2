#
# tw6869, Intersil|Twchwell TW6869 video driver with V4L/2 support
#
# Makefile - driver build system
#

# Requires GNU Make

# take version info from last module build if available
#KERNELRELEASE := $(shell cat $(obj)/.version 2>/dev/null || uname -r)

# Usual paths to the kernel source tree
KDIR := /lib/modules/`uname -r`/build

# Standard path to install the driver
MODULE_INSTALLDIR = /lib/modules/`uname -r`/kernel/drivers/media/video/tw686x
KDRV_INSTALLDIR = /lib/modules/`uname -r`/kernel/drivers/media/pci/tw686x



ifneq ($(KERNELRELEASE),)   # We were called by build system.


tw6869-objs := TW68-core.o TW68-video.o TW68-ALSA.o
EXTRA_CFLAGS += -DAUDIO_BUILD

#obj-$(CONFIG_VIDEO_TW6869) += tw6869.o
obj-m += tw6869.o

EXTRA_CFLAGS += -Idrivers/media/video

else   # We were called from command line

PWD := "$(shell pwd)"

Debug: default
Release: default
cleanDebug: clean
cleanRelease: clean

audio: modules

default: modules

modules:
	@echo '**************************************************************************'
	@echo '* Building Intersil|Techwell TW6869 driver...                            *'
	@echo '* Type "make help" for a list of available targets.                      *'
	@echo '**************************************************************************'
	@echo
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

endif

install: modules_install

modules_install:
	mkdir -p kernel_backup
	find /lib/modules/$(shell uname -r)/ -name 'tw6869.ko' | xargs -I{} cp --parent {} kernel_backup
	find /lib/modules/$(shell uname -r)/ -name 'tw686x.ko' | xargs -I{} cp --parent {} kernel_backup
	find /lib/modules/$(shell uname -r)/ -name 'tw686x.ko.xz' | xargs -I{} cp --parent {} kernel_backup
	find /lib/modules/$(shell uname -r)/ -name 'tw6869.ko' | sudo xargs rm -f
	find /lib/modules/$(shell uname -r)/extra/ -name 'tw686x.ko' | sudo xargs rm -f
	find /lib/modules/$(shell uname -r)/extra/ -name 'tw686x.ko.*' | sudo xargs rm -f
	/bin/mkdir -p $(MODULE_INSTALLDIR)
	/bin/cp ./tw6869.ko $(MODULE_INSTALLDIR)
	/bin/chmod 644 $(MODULE_INSTALLDIR)/tw6869.ko
	/sbin/depmod -ae
	@echo
	@echo '**************************************************************************'
	@echo '* Driver installed successfully. Type "make load" to load it.            *'
	@echo '**************************************************************************'
	@echo

load:
	/sbin/modprobe tw6869

unload:
	/sbin/modprobe -r tw6869

reload:
	/sbin/modprobe -r tw6869
	/sbin/modprobe tw6869

uninstall: modules_uninstall

modules_uninstall:
	/bin/rm -rf $(MODULE_INSTALLDIR)/tw6869.ko
	/bin/rm -rf $(MODULE_INSTALLDIR)
	/sbin/depmod -ae
	@echo
	@echo '**************************************************************************'
	@echo '* Driver uninstalled successfully.                                       *'
	@echo '**************************************************************************'
	@echo

help:
	@echo 'List of available targets. Type:'
	@echo '- "make modules" to build the modules'
	@echo '- "make modules_install" to install the built modules'
	@echo '- "make reload" to reload the modules'
	@echo '- "make load" to load the installed modules'
	@echo '- "make modules_uninstall" to uninstall the installed modules'
	@echo '- "make clean" to remove all generated files in the current directory'
	@echo '- "make tar" to create an source code archive of the current directory in $(TARFILE)'
	@echo '- "make help" to print the list of available targets'


RELEASE_VERSION := $(shell date +%Y%m%d)
TARFILE=../tw6869-$(RELEASE_VERSION).tar.gz
DIR=../
FILE=tw6869

tar:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	/bin/rm -rf *~
	tar -choz --verbose -C $(DIR) --file=$(TARFILE) $(FILE)
	@echo
	@echo '**************************************************************************'
	@echo '* Source package created in the parent folder ../                        *'
	@echo '**************************************************************************'
	@echo

rl:
	sudo make modules_install;sudo /sbin/modprobe -r tw6869;sudo /sbin/modprobe tw6869 TW68_debug_alsa=1
