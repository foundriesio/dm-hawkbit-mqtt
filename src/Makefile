ccflags-y +=-I${ZEPHYR_BASE}/net/ip
ccflags-y +=-I${ZEPHYR_BASE}/tests/include
ccflags-y +=-I${SOURCE_DIR}/lib

obj-y = main.o device.o tcp.o
obj-y += bluemix.o
obj-$(CONFIG_BLUETOOTH) += bt_ipss.o bt_storage.o
obj-$(CONFIG_SYS_LOG_EXT_HOOK) += tstamp_log.o

# Library code for FOTA and other generic support services.
obj-y += lib/
