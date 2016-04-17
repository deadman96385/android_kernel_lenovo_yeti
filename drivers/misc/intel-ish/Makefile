#
# Makefile - Intel ISH drivers
# Copyright (c) 2014-2016, Intel Corporation.
#
obj-$(CONFIG_INTEL_ISH) += ishtp.o
ishtp-objs := init.o
ishtp-objs += hbm.o
ishtp-objs += client.o
ishtp-objs += ishtp-api.o
ishtp-objs += bus.o
ishtp-$(CONFIG_DEBUG_FS) += debugfs.o

obj-$(CONFIG_INTEL_ISH) += ish.o
ish-objs := ipc.o
ish-objs += pci-ish.o

obj-$(CONFIG_INTEL_ISH) += hid-ishtp.o
hid-ishtp-objs := ishtp-hid.o
hid-ishtp-objs += ishtp-hid-client.o

obj-$(CONFIG_INTEL_ISH) += ish-hid-drv.o
ish-hid-drv-objs := ish-hid-dd.o

obj-$(CONFIG_INTEL_ISH) += sens-col-core.o
sens-col-core-objs := senscol-core.o

