BOARD ?= nrf52_pca10040
CONF_FILE = prj.conf
CONF_FILE += $(wildcard boards/$(BOARD).conf)

KBUILD_KCONFIG = $(CURDIR)/Kconfig
export KBUILD_KCONFIG

include $(ZEPHYR_BASE)/Makefile.inc
