/* ESPNOW Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

/*
   This example shows how to use ESPNOW.
   Prepare two device, one for sending ESPNOW data and another for receiving
   ESPNOW data.
*/
#include "driver/gpio.h"
#include "esp_crc.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_now.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "espnow_example.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "nvs_flash.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include "driver/i2c.h"

#define ESPNOW_MAXDELAY 512
#define BLINK_GPIO 25
#define ADXL345_ADD 0x53
#define THRESHOLD 300

static const char *TAG = "espnow_example";

// 24:6F:28:24:4B:C0
static uint8_t s_example_broadcast_mac[ESP_NOW_ETH_ALEN] = {0x24, 0x6F, 0x28,
                                                            0x24, 0x4B, 0xC0};

static void example_espnow_deinit(example_espnow_send_param_t *send_param);

typedef struct
{
  uint8_t myint;
} __attribute__((packed)) my_espnow_data_t;

static esp_err_t i2c_master_init(void)
{
  int i2c_master_port = 0;
  i2c_config_t conf = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = 21,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_io_num = 22,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master.clk_speed = 100000,
      // .clk_flags = 0,          /*!< Optional, you can use I2C_SCLK_SRC_FLAG_* flags to choose i2c source clock here. */
  };
  esp_err_t err = i2c_param_config(i2c_master_port, &conf);
  if (err != ESP_OK)
  {
    return err;
  }
  return i2c_driver_install(i2c_master_port, conf.mode, 0, 0, 0);
}

/* WiFi should start before using ESPNOW */
static void example_wifi_init(void)
{
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(ESPNOW_WIFI_MODE));
  ESP_ERROR_CHECK(esp_wifi_start());

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
  ESP_ERROR_CHECK(esp_wifi_set_protocol(
      ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G |
                          WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR));
#endif
}

/* ESPNOW sending or receiving callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
static void example_espnow_send_cb(const uint8_t *mac_addr,
                                   esp_now_send_status_t status)
{
  ESP_LOGI(TAG, "Message sent to " MACSTR ". ", MAC2STR(mac_addr));
}

static void example_espnow_task(void *pvParameter)
{
  int x, y, z;
  int xo = 0, yo = 0, zo = 0;
  // my function
  while (1)
  {
    //Reading value and doing calculation and calibration
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    uint8_t *_buff = (uint8_t *)malloc(6);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, ADXL345_ADD << 1 | I2C_MASTER_WRITE, true); //slave address
    i2c_master_write_byte(cmd, 0x32, true);                                //Accessign x_data_0
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, ADXL345_ADD << 1 | I2C_MASTER_READ, true);
    i2c_master_read(cmd, _buff, 5, I2C_MASTER_ACK);
    i2c_master_read_byte(cmd, _buff + 5, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t ret = i2c_master_cmd_begin(0, cmd, 2000 / portTICK_PERIOD_MS);
    i2c_cmd_link_delete(cmd);
    if (ret != ESP_OK)
    {
      ESP_LOGE(TAG, "%s\n", esp_err_to_name(ret));
    }
    else
    {
      x = (int16_t)((((int)_buff[1]) << 8) | _buff[0]);
      y = (int16_t)((((int)_buff[3]) << 8) | _buff[2]);
      z = (int16_t)((((int)_buff[5]) << 8) | _buff[4]);

      x -= 4;
      y -= 34;
      z += 10;

      //ESP_LOGI(TAG, "Actual: %d\t%d\t%d", x, y, z);
      //ESP_LOGI(TAG, "Old: %d\t%d\t%d", xo, yo, zo);
      
      //VALUE READ AND CALIBRATED

      if (abs(xo - x) > THRESHOLD || abs(yo - y) > THRESHOLD || abs(zo - z) > THRESHOLD)
      {
        //send espnow command
        
        uint8_t *buffer = malloc(CONFIG_ESPNOW_SEND_LEN);
        my_espnow_data_t *buf = (my_espnow_data_t *)buffer;
        buf->myint = 'A';
        
        gpio_set_level(BLINK_GPIO, 1);
        if (esp_now_send(s_example_broadcast_mac, buffer, sizeof(my_espnow_data_t)) != ESP_OK)
        {
          ESP_LOGE(TAG, "Send error");
          //example_espnow_deinit(send_param);
          vTaskDelete(NULL);
        }
        gpio_set_level(BLINK_GPIO, 0);
        free(buffer);
        xo = x;
        yo = y;
        zo = z;
        vTaskDelay(50/portTICK_RATE_MS);
      }
    }
    free(_buff);
    //vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

static esp_err_t example_espnow_init(void)
{

  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb(example_espnow_send_cb));

  /* Add broadcast peer information to peer list. */
  esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
  if (peer == NULL)
  {
    ESP_LOGE(TAG, "Malloc peer information fail");
    //esp_now_deinit();
    return ESP_FAIL;
  }
  memset(peer, 0, sizeof(esp_now_peer_info_t));
  peer->channel = CONFIG_ESPNOW_CHANNEL;
  peer->ifidx = ESPNOW_WIFI_IF;
  peer->encrypt = false;
  memcpy(peer->peer_addr, s_example_broadcast_mac, ESP_NOW_ETH_ALEN);
  ESP_ERROR_CHECK(esp_now_add_peer(peer));
  free(peer);

  /* Initialize sending parameters. */
  ESP_LOGI(TAG, "Everything Initialized");
  xTaskCreate(example_espnow_task, "EXAMPLE_task", 2048, NULL, 4, NULL);

  return ESP_OK;
}

static void example_espnow_deinit(example_espnow_send_param_t *send_param)
{
  free(send_param->buffer);
  free(send_param);
  //vSemaphoreDelete(s_example_espnow_queue);
  //esp_now_deinit();
}

void app_main(void)
{
  // Initialize NVS
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  ESP_ERROR_CHECK(i2c_master_init());

  gpio_reset_pin(BLINK_GPIO);
  /* Set the GPIO as a push/pull output */
  gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

  example_wifi_init(); //initialize wifi

  //now let's initialize sensor
  i2c_cmd_handle_t cmd = i2c_cmd_link_create();
  i2c_master_start(cmd);
  i2c_master_write_byte(cmd, ADXL345_ADD << 1 | I2C_MASTER_WRITE, true);
  i2c_master_write_byte(cmd, 0x2D, true);       //enable register
  i2c_master_write_byte(cmd, 0b00001000, true); //enable command
  i2c_master_stop(cmd);
  ret = i2c_master_cmd_begin(0, cmd, 1000 / portTICK_RATE_MS); //using i2c port 0
  i2c_cmd_link_delete(cmd);
  if (ret != ESP_OK)
  {
    ESP_LOGE(TAG, "%s\n", esp_err_to_name(ret));
  }
  else
  {
    ESP_LOGI(TAG, "Sensor Initialized\n");
  }

  example_espnow_init();
}
