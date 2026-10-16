obj-m := cxl_type2.o

ifdef KBUILD_MODULES
ifeq ($(shell test -f $(srctree)/include/cxl/pci.h && echo y),)
$(error kernel >= v7.3 required (cxl/pci.h not found))
endif
endif
