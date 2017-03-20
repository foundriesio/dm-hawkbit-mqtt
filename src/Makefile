ccflags-y +=-I${ZEPHYR_BASE}/net/ip
ccflags-y +=-I${ZEPHYR_BASE}/tests/include

obj-y = main.o boot_utils.o device.o jsmn.o tcp.o flash_block.o
obj-y += bluemix.o
obj-$(CONFIG_FOTA_DM_BACKEND_HAWKBIT) += hawkbit.o
obj-$(CONFIG_BLUETOOTH) += bt_ipss.o bt_storage.o
obj-$(CONFIG_CONSOLE_SHELL) += fota_shell.o
