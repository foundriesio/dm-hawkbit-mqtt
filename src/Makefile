ccflags-y +=-I${ZEPHYR_BASE}/net/ip
ccflags-y +=-I${ZEPHYR_BASE}/tests/include

obj-y = main.o boot_utils.o device.o jsmn.o tcp.o flash_block.o
obj-y += bluemix.o
obj-$(CONFIG_FOTA_DM_BACKEND_HAWKBIT) += hawkbit.o
obj-$(CONFIG_BLUETOOTH) += bt_ipss.o bt_storage.o
obj-$(CONFIG_SYS_LOG_EXT_HOOK) += tstamp_log.o

# Library code for FOTA and other generic support services.
obj-y += lib/
