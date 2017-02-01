#Linaro Firmware Over The Air Programming Example

Example application that uses Hawkbit to implement FOTA.

##Requirements:
  * Newt's bootloader
  * Hawkbit server

##Board compatibility:

###nRF52
  * PCA10040
  * Nitrogen

##Creating and signing the image:

Check https://collaborate.linaro.org/display/MET/Dual+bank+boot+with+RSA for
the complete overview.


Quick example (Nitrogen; run this from zephyr-utils):


`./zep2newt.py --bin <zephyr-fota-hawkbit>/outdir/96b_nitrogen/zephyr.bin \
	      --key root.pem --sig RSA --vtoff 0x100 --out zephyr.img.bin`


Then just upload zephyr.img.bin to the Hawkbit server.

###TODO
  * Extend readme explaining how to setup the server environment
  * Explain how the FOTA process work
  * How to build and use newt's bootloader
  * OpenSSL support
  * Add support to use Hawkbit's security token when updating the server
