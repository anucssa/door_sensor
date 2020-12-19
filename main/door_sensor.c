/*-
 * SPDX-License-Identifier: Apache-2.0
 *
 * Copyright (c) 2006-2016 ARM Limited
 * All rights reserved.
 *
 * Copyright (c) 2015-2016 Espressif Systems (Shanghai) PTE LTD
 * Copyright (c) 2020 Felix Friedlander <felix.friedlander@anu.edu.au>
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
 */

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"

#include "esp_idf_version.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_netif.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "esp_tls.h"
#include "esp_crt_bundle.h"

#define xstr(s) str(s)
#define str(s) #s

#define API_BODY(s) "code=" CONFIG_API_KEY "&state=" s

/*
 * HTTP/1.0 is preferred because non-persistent connections
 * are the default behaviour.
 *
 * Yes, hard-coding the Content-Length is smelly, but I can't think
 * of a way to make the C preprocessor do it for me.
 *
 * No, xstr(sizeof(...)) doesn't work.
 */
#define WEB_REQUEST(s) "POST " CONFIG_API_PATH " HTTP/1.0\r\n" \
    "Host: " CONFIG_API_SERVER "\r\n" \
    "User-Agent: esp-idf/" xstr(ESP_IDF_VERSION_MAJOR) "." \
    xstr(ESP_IDF_VERSION_MINOR) "." xstr(ESP_IDF_VERSION_PATCH) " \r\n" \
    "Content-Length: 43\r\n" \
    "Content-Type: application/x-www-form-urlencoded\r\n" \
    "\r\n" \
    API_BODY(s)

#define GPIO_SENSOR GPIO_NUM_4
#define ESP_INTR_FLAG_DEFAULT 0

#define CONNECTED_BIT BIT0

EventGroupHandle_t wifi_event_group;

TaskHandle_t door_sensor_task_handle;

const char *tag = "door_sensor";

#ifdef CONFIG_ANUSECURE_VALIDATE_CERT
extern const unsigned char *ca_pem_start asm("_binary_wpa2_ca_pem_start");
extern const unsigned char *ca_pem_end  asm("_binary_wpa2_ca_pem_end");
#endif /* CONFIG_ANUSECURE_VALIDATE_CERT */

static void IRAM_ATTR
gpio_isr_handler(void *arg)
{
	xTaskNotifyFromISR(door_sensor_task_handle, 0, eNoAction, NULL);
}

static void
event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		esp_wifi_connect();
		xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
	}
}

void
initialise_gpio(void)
{
	gpio_config_t io_conf = {
		.pin_bit_mask = (1ULL << GPIO_SENSOR),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,
		.pull_down_en = GPIO_PULLDOWN_DISABLE,
		.intr_type = GPIO_INTR_ANYEDGE
	};
	gpio_config(&io_conf);

	gpio_install_isr_service(ESP_INTR_FLAG_DEFAULT);
	gpio_isr_handler_add(GPIO_SENSOR, gpio_isr_handler, (void *)GPIO_SENSOR);
}

static void
initialise_wifi(void)
{
	ESP_ERROR_CHECK(esp_netif_init());
	wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
	assert(sta_netif);

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
	ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
	wifi_config_t wifi_config = {
		.sta = {
			.ssid = "ANU-Secure",
		},
	};
	ESP_LOGI(tag, "Setting Wi-Fi configuration...");
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

	ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_username((uint8_t *)CONFIG_ANUSECURE_USERNAME,
	    strlen(CONFIG_ANUSECURE_USERNAME)));
	ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_password((uint8_t *)CONFIG_ANUSECURE_PASSWORD,
	    strlen(CONFIG_ANUSECURE_PASSWORD)));
#ifdef CONFIG_ANUSECURE_VALIDATE_CERT
	ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_set_ca_cert(ca_pem_start, ca_pem_end - ca_pem_start));
#endif
	ESP_ERROR_CHECK(esp_wifi_sta_wpa2_ent_enable());

	ESP_ERROR_CHECK(esp_wifi_start());
}

static void
door_sensor_task(void *pvParameters)
{
	char buf[512];
	int ret, len;

	for (;;) {
		ESP_LOGI(tag, "Waiting for network");
		while (!(xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY) & CONNECTED_BIT));

		esp_tls_t *tls = esp_tls_init();
		if (tls == NULL) {
			ESP_LOGE(tag, "TLS initialisation failed...");
			goto exit;
		}

		esp_tls_cfg_t cfg = {
			.crt_bundle_attach = esp_crt_bundle_attach,
		};

		ret = esp_tls_conn_new_sync(CONFIG_API_SERVER,
		    strlen(CONFIG_API_SERVER), 443, &cfg, tls);
		if (ret == 1) {
			ESP_LOGI(tag, "Connection established...");
		} else {
			ESP_LOGE(tag, "Connection failed...");
			goto exit;
		}

		xTaskNotifyStateClear(door_sensor_task_handle);
		int door_state = gpio_get_level(GPIO_SENSOR);
		ESP_LOGI(tag, "Read GPIO level %d", door_state);

		size_t written_bytes = 0;
		const char *request = door_state ? WEB_REQUEST("1") : WEB_REQUEST("0");
		do {
			ret = esp_tls_conn_write(tls, request + written_bytes,
			    strlen(request) - written_bytes);
			if (ret >= 0) {
				ESP_LOGI(tag, "%d bytes written", ret);
				written_bytes += ret;
			} else if (ret != ESP_TLS_ERR_SSL_WANT_READ
			    && ret != ESP_TLS_ERR_SSL_WANT_WRITE) {
				ESP_LOGE(tag, "esp_tls_conn_write returned %#x", ret);
				goto exit;
			}
		} while (written_bytes < strlen(request));

		ESP_LOGI(tag, "Reading HTTP response...");

		for (;;) {
			len = sizeof(buf) - 1;
			bzero(buf, sizeof(buf));
			ret = esp_tls_conn_read(tls, (char *)buf, len);

			if (ret == ESP_TLS_ERR_SSL_WANT_WRITE  || ret == ESP_TLS_ERR_SSL_WANT_READ)
				continue;

			if (ret < 0) {
				ESP_LOGE(tag, "esp_tls_conn_read returned %#x", ret);
				break;
			}

			if (ret == 0) {
				ESP_LOGI(tag, "Connection closed");
				break;
			}

			len = ret;
			for (int i = 0; i < len; i++)
				putchar(buf[i]);
		}

	exit:
		esp_tls_conn_delete(tls);
		putchar('\n');

		ESP_LOGI(tag, "Request completed");
		vTaskDelay(pdMS_TO_TICKS(2 * 1000));

		/* Wait for edge interrupt or 5min timeout */
		if (xTaskNotifyWait(0, 0, NULL,
		    CONFIG_API_PERIODIC ? pdMS_TO_TICKS(CONFIG_API_PERIODIC_MINUTES * 60 * 1000) : portMAX_DELAY))
			ESP_LOGI(tag, "Received edge notification");
		else
			ESP_LOGI(tag, "Timed out waiting for notification, sending periodic update");
	}
}

void
app_main(void)
{
	ESP_ERROR_CHECK(nvs_flash_init());
	initialise_wifi();
	xTaskCreate(&door_sensor_task, "door_sensor_task", 4096, NULL, 5, &door_sensor_task_handle);
	initialise_gpio();
}
