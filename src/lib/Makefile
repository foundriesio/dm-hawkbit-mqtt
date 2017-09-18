ccflags-y += -I${ZEPHYR_BASE}/lib/json/ -I${SOURCE_DIR}/lib

obj-y += hawkbit.o
obj-y += flash_block.o
obj-$(CONFIG_FOTA_CONSOLE_SHELL) += fota_shell.o
obj-y += mcuboot.o
obj-y += product_id.o
obj-$(CONFIG_NET_TCP) += tcp.o
obj-$(CONFIG_FOTA_BLUEMIX) += bluemix.o
