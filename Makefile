BOARD ?= nrf52_pca10040
MDEF_FILE = prj.mdef
KERNEL_TYPE = micro
CONF_FILE = prj.conf

include $(ZEPHYR_BASE)/Makefile.inc
