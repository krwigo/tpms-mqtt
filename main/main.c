// https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/bluetooth/bt_le.html

#include "consts.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "lwip/inet.h"
#include "mqtt_client.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "TPMS_MQTT"

#define LED_GPIO 8
#define LED_ON 0
#define LED_OFF 1

#define MAX_ADV_DATA_LEN 31
#define MAX_DEVICE_NAME_LEN 32
#define MAX_JSON_BUFFER_SIZE 512
#define MAX_DEVICES 16

#define HDR_0 0x03
#define HDR_1 0x08
#define HDR_2 'B'
#define HDR_3 'R'

static esp_mqtt_client_handle_t mqtt_client;
static bool mqtt_connected = false;
static bool wifi_connected = false;

typedef struct {
  float voltage;
  float temperature_c;
  float pressure_psi;
  bool valid;
} sensor_data_t;

static sensor_data_t parse_sensor_payload_app(const uint8_t *payload,
                                              uint8_t len) {
  sensor_data_t result = {0};
  if (len < 11)
    return result;

  int pos = -1;
  for (int i = 0; i <= len - 4; i++) {
    if (payload[i] == HDR_0 && payload[i + 1] == HDR_1 &&
        payload[i + 2] == HDR_2 && payload[i + 3] == HDR_3) {
      pos = i;
      break;
    }
  }

  if (pos < 0 || pos + 11 > len)
    return result;

  const uint8_t *sl = &payload[pos];

  float voltage = sl[7] / 10.0f;
  float temp_c = (float)sl[8];

  uint16_t pr_raw = (sl[9] << 8) | sl[10];
  if (pr_raw < 148)
    pr_raw = 146;

  float pr_bar = (pr_raw - 145) / 145.0f;
  float pr_psi = pr_bar * 14.5038f;

  result.voltage = voltage;
  result.temperature_c = temp_c;
  result.pressure_psi = pr_psi;
  result.valid = true;
  return result;
}

typedef struct {
  char mac[18];
  char json[MAX_JSON_BUFFER_SIZE];
  bool valid;
} device_cache_t;

static device_cache_t device_cache[MAX_DEVICES];

static void cache_update(const char *mac, const char *json) {
  // Update existing entry
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (device_cache[i].valid && strcmp(device_cache[i].mac, mac) == 0) {
      strncpy(device_cache[i].json, json, sizeof(device_cache[i].json) - 1);
      device_cache[i].json[sizeof(device_cache[i].json) - 1] = '\0';
      return;
    }
  }

  // Insert new entry
  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!device_cache[i].valid) {
      strncpy(device_cache[i].mac, mac, sizeof(device_cache[i].mac));
      device_cache[i].mac[sizeof(device_cache[i].mac) - 1] = '\0';
      strncpy(device_cache[i].json, json, sizeof(device_cache[i].json) - 1);
      device_cache[i].json[sizeof(device_cache[i].json) - 1] = '\0';
      device_cache[i].valid = true;
      return;
    }
  }

  ESP_LOGW(TAG, "Device cache full, overwriting index 0");
  strncpy(device_cache[0].mac, mac, sizeof(device_cache[0].mac));
  device_cache[0].mac[sizeof(device_cache[0].mac) - 1] = '\0';
  strncpy(device_cache[0].json, json, sizeof(device_cache[0].json) - 1);
  device_cache[0].json[sizeof(device_cache[0].json) - 1] = '\0';
  device_cache[0].valid = true;
}

static bool led_state = false;

static void led_set_on(void) {
  led_state = true;
  gpio_set_level(LED_GPIO, LED_ON);
}

static void led_set_off(void) {
  led_state = false;
  gpio_set_level(LED_GPIO, LED_OFF);
}

static void led_toggle(void) {
  if (led_state)
    led_set_off();
  else
    led_set_on();
}

static void update_led_state(void) {
  if (wifi_connected && mqtt_connected) {
    led_set_off();
  } else {
    led_set_on();
  }
}

static void format_mac_address(const uint8_t *mac, char *output) {
  snprintf(output, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[5], mac[4], mac[3],
           mac[2], mac[1], mac[0]);
}

static void bin_to_hex_string(const uint8_t *data, size_t len, char *output) {
  size_t count = len > MAX_ADV_DATA_LEN ? MAX_ADV_DATA_LEN : len;
  for (size_t i = 0; i < count; i++) {
    sprintf(output + (i * 2), "%02X", data[i]);
  }
  output[count * 2] = '\0';
}

static const char *extract_device_name(const uint8_t *adv_data,
                                       uint8_t data_len) {
  static char name[MAX_DEVICE_NAME_LEN] = {0};
  uint8_t ad_length = 0;
  uint8_t ad_type = 0;
  uint8_t ad_data_len = 0;

  name[0] = '\0';

  for (int i = 0; i < data_len;) {
    ad_length = adv_data[i];
    if (ad_length == 0)
      break;
    ad_type = adv_data[i + 1];
    ad_data_len = ad_length - 1;

    if (ad_type == 0x08 || ad_type == 0x09) {
      if (ad_data_len > 0 && ad_data_len < MAX_DEVICE_NAME_LEN) {
        memset(name, 0, sizeof(name));
        memcpy(name, &adv_data[i + 2], ad_data_len);
        name[ad_data_len] = '\0';
        return name;
      }
    }

    i += ad_length + 1;
  }

  return "";
}

static void send_advertisement_to_mqtt(const uint8_t *mac_address,
                                       const char *device_name,
                                       const uint8_t *adv_data,
                                        uint8_t adv_data_len) {
  sensor_data_t sensor = parse_sensor_payload_app(adv_data, adv_data_len);
  if (!sensor.valid) {
    return;
  }

  char mac_str[18];
  format_mac_address(mac_address, mac_str);

  char adv_data_str[MAX_ADV_DATA_LEN * 2 + 1];
  bin_to_hex_string(adv_data, adv_data_len, adv_data_str);

  char json_buffer[MAX_JSON_BUFFER_SIZE];
  int json_len =
      snprintf(json_buffer, sizeof(json_buffer),
               "{\"mac\":\"%s\",\"name\":\"%s\",\"data\":\"%s\",\"voltage\":%.1f,"
               "\"temperature_c\":%.1f,\"pressure_psi\":%.2f}\n",
               mac_str, device_name, adv_data_str, sensor.voltage,
               sensor.temperature_c, sensor.pressure_psi);

  char topic_buffer[128];
  snprintf(topic_buffer, sizeof(topic_buffer), "ble/scanner/data/%s",
           mac_str);

  if (!mqtt_client || !mqtt_connected) {
    if (device_name && strcmp(device_name, "BR") == 0) {
      cache_update(mac_str, json_buffer);
    }
    return;
  }

  esp_mqtt_client_publish(mqtt_client, topic_buffer, json_buffer, json_len, 1,
                          0);
}

static void send_all_cached_to_mqtt(void) {
  if (!mqtt_client || !mqtt_connected) {
    ESP_LOGW(TAG, "MQTT not connected, skipping send");
    return;
  }

  for (int i = 0; i < MAX_DEVICES; i++) {
    if (!device_cache[i].valid)
      continue;

    char topic_buffer[128];
    snprintf(topic_buffer, sizeof(topic_buffer), "ble/scanner/data/%s/debug",
             device_cache[i].mac);

    int msg_id =
        esp_mqtt_client_publish(mqtt_client, topic_buffer, device_cache[i].json,
                                strlen(device_cache[i].json), 1, 0);

    if (msg_id >= 0) {
      device_cache[i].valid = false;
    }
  }
}

static void mqtt_event_handler(void *handler_args __attribute__((unused)),
                               esp_event_base_t base,
                               int32_t event_id, void *event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

  ESP_LOGD(TAG, "MQTT event base=%s id=%ld client=%p", base, (long)event_id,
           mqtt_client);

  switch (event_id) {
  case MQTT_EVENT_CONNECTED:
    mqtt_connected = true;
    ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
    send_all_cached_to_mqtt();
    break;
  case MQTT_EVENT_DISCONNECTED:
    mqtt_connected = false;
    ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
    break;
  case MQTT_EVENT_SUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED");
    break;
  case MQTT_EVENT_UNSUBSCRIBED:
    ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED");
    break;
  case MQTT_EVENT_PUBLISHED:
    ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED");
    break;
  case MQTT_EVENT_DATA:
    if (event && event->topic) {
      ESP_LOGI(TAG, "MQTT data topic: %.*s", event->topic_len, event->topic);
    }
    ESP_LOGI(TAG, "MQTT_EVENT_DATA");
    break;
  case MQTT_EVENT_ERROR:
    ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
    break;
  default:
    ESP_LOGI(TAG, "MQTT Event: %d", event_id);
    break;
  }

  update_led_state();
}

static void wifi_event_handler(void *arg __attribute__((unused)),
                               esp_event_base_t event_base, int32_t event_id,
                               void *event_data) {

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
    wifi_connected = true;
    ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED");
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
  } else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_connected = false;
    ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED");
    esp_wifi_connect();
  }

  update_led_state();
}

static int64_t last_blink_time_us = 0;

static int ble_gap_event_listener(struct ble_gap_event *event,
                                  void *arg __attribute__((unused))) {
  switch (event->type) {
  case BLE_GAP_EVENT_DISC:
    const uint8_t *mac_address = event->disc.addr.val;
    uint8_t adv_data_len = event->disc.length_data;
    const uint8_t *adv_data = event->disc.data;

    const char *device_name = extract_device_name(adv_data, adv_data_len);

    // if (device_name && strcmp(device_name, "BR") == 0)
    send_advertisement_to_mqtt(mac_address, device_name, adv_data,
                               adv_data_len);

    int64_t now = esp_timer_get_time();
    if (now - last_blink_time_us >= 1000000) { // 1s
      last_blink_time_us = now;
      led_toggle();
    }
    break;

  default:
    ESP_LOGI(TAG, "GAP event: %d", event->type);
    break;
  }
  return 0;
}

static void ble_app_on_sync(void) {
  struct ble_gap_disc_params disc_params = {
      .passive = 1,
  };

  int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &disc_params,
                        ble_gap_event_listener, NULL);

  if (rc != 0) {
    ESP_LOGE(TAG, "Failed to start BLE scan: %d", rc);
    return;
  }
}

static void bt_host_task(void *param __attribute__((unused))) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void init_led(void) {
  gpio_config_t io_conf = {0};
  io_conf.pin_bit_mask = (1ULL << LED_GPIO);
  io_conf.mode = GPIO_MODE_OUTPUT;
  io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&io_conf);

  led_set_on();
}

static void init_nvs(void) {
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
}

static void init_wifi(void) {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_t *sta_netif = esp_netif_create_default_wifi_sta();
  if (sta_netif == NULL) {
    ESP_LOGE(TAG, "Failed to create default WiFi STA interface");
    abort();
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  esp_event_handler_instance_t instance_any_id;
  esp_event_handler_instance_t instance_got_ip;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
      IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL,
      &instance_got_ip));
  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = "",
              .password = "",
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
          },
  };

  strlcpy((char *)wifi_config.sta.ssid, WIFI_SSID,
          sizeof(wifi_config.sta.ssid));
  strlcpy((char *)wifi_config.sta.password, WIFI_PASSWORD,
          sizeof(wifi_config.sta.password));

  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());
}

static void init_ble(void) {
  int ret = nimble_port_init();
  if (ret != 0) {
    ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
    abort();
  }
}

static void init_mqtt(void) {
  const esp_mqtt_client_config_t mqtt_config = {
      .broker.address.uri = MQTT_URI,
  };

  mqtt_client = esp_mqtt_client_init(&mqtt_config);
  if (mqtt_client == NULL) {
    ESP_LOGE(TAG, "Failed to initialize MQTT client");
    abort();
  }

  esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID,
                                 mqtt_event_handler, NULL);
  esp_mqtt_client_start(mqtt_client);
}

void app_main(void) {
  init_led();
  init_nvs();
  init_wifi();

  init_ble();
  init_mqtt();

  static struct ble_gap_event_listener listener;

  int ret =
      ble_gap_event_listener_register(&listener, ble_gap_event_listener, NULL);
  if (ret != 0) {
    ESP_LOGE(TAG, "Failed to register GAP event listener: %d", ret);
    abort();
  }

  ble_hs_cfg.sync_cb = ble_app_on_sync;
  nimble_port_freertos_init(bt_host_task);

  ESP_LOGI(TAG, "BLE scanning started successfully");

  while (1) {
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}
