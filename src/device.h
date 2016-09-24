/*
 * Copyright (c) 2016 Linaro Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

/* Flash specific configs */
#if defined(CONFIG_SOC_SERIES_NRF52X)
#define FLASH_DRIVER_NAME	"NRF5_FLASH"
#define FLASH_BANK0_OFFSET	0x00008000
#define FLASH_BANK1_OFFSET	0x00042000
#define FLASH_BANK_SIZE		(232 * 1024)
#define FLASH_STATE_OFFSET	0x0007d000
#define FLASH_STATE_SIZE	(12 * 1024)
#elif defined(CONFIG_SOC_STM32F401RE)
#define FLASH_DRIVER_NAME	"STM32F4_FLASH"
#define FLASH_BANK0_OFFSET	0x00020000
#define FLASH_BANK1_OFFSET	0x00040000
#define FLASH_BANK_SIZE		(128 * 1024)
#define FLASH_STATE_OFFSET	0x00008000
#define FLASH_STATE_SIZE	(32 * 1024)
#endif

/* GPIO */
#if defined(CONFIG_BOARD_NRF52_PCA10040)
#define LED1_GPIO	17
#define LED2_GPIO	18
#elif defined(CONFIG_BOARD_NRF52_NITROGEN)
#define LED1_GPIO	29
#define LED2_GPIO	28
#elif defined(CONFIG_BOARD_NUCLEO_F401RE)
#define LED1_GPIO	2
#define LED2_GPIO	3
#elif defined(CONFIG_BOARD_96B_CARBON)
#define LED1_GPIO	15
#define LED2_GPIO	5
#endif

/* LED */
#define LED_BLINK	LED1_GPIO

/* GPIO Driver */
#if defined(CONFIG_SOC_FAMILY_NRF5)
#define GPIO_DRV_NAME	"GPIO_0"
#elif defined(CONFIG_BOARD_NUCLEO_F401RE)
#define GPIO_DRV_NAME	"GPIOC"
#elif defined(CONFIG_BOARD_96B_CARBON)
#if (LED_BLINK == LED1_GPIO)
#define GPIO_DRV_NAME   "GPIOA"
#else
#define GPIO_DRV_NAME   "GPIOB"
#endif
#endif

/* Bluetooth */
#define DEVICE_NAME "Linaro IPSP node"
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)
#define UNKNOWN_APPEARANCE 0x0000

/* General hardware specific configs */
#if defined(CONFIG_SOC_SERIES_STM32F4X)
#define DEVICE_ID_BASE	0x1fff7a10
#endif

struct product_id_t {
	const char *name;
	uint32_t number;
};

extern struct product_id_t product_id;
extern struct device *flash_dev;

void set_device_id(void);
