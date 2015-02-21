#
# Makefile for the linux kernel.
#

VERSION_STRING ?= $(shell date "+%Y%m%d")
EXTRA_CFLAGS += -D DORA_OS_VERSION=$(VERSION_STRING)
ccflags-y	+= -Iarch/arm/mach-tegra
obj-y		+= sysinfo.o
