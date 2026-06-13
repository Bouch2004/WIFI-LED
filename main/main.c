#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "led_strip.h"

// ===================================================================
//  HARDWARE DEFINES
// ===================================================================
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define RGB_LED_GPIO     GPIO_NUM_48

// ===================================================================
//  WI-FI CONFIG (set via: idf.py menuconfig -> RGB Web Controller Config)
// ===================================================================
#define WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define MAXIMUM_RETRY  5

static const char *TAG = "RGB_DASHBOARD";

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;

// ===================================================================
//  SHARED LED STATE  (written by HTTP handler, read by LED task)
// ===================================================================
static volatile uint8_t g_r = 0, g_g = 0, g_b = 0;
static volatile bool    g_led_dirty = false; // flag: new value pending

static led_strip_handle_t led_strip;

// ===================================================================
//  HTML PAGE  (minified, stored in flash)
// ===================================================================
static const char *html_page =
"<!DOCTYPE html><html><head><meta charset='UTF-8'>"
"<title>ESP32 RGB Dashboard</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0d0d0d;color:#eee;font-family:'Segoe UI',sans-serif;"
"display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh}"
"h1{font-size:1.6rem;font-weight:600;letter-spacing:2px;text-transform:uppercase;"
"margin-bottom:8px;color:#fff}"
"p.sub{font-size:.85rem;color:#666;margin-bottom:36px}"
".card{background:#1a1a1a;border-radius:18px;padding:36px 44px;"
"box-shadow:0 20px 60px rgba(0,0,0,.7);width:360px}"
".preview{width:100%;height:100px;border-radius:12px;margin-bottom:32px;"
"background:rgb(0,0,0);transition:background .1s;border:1px solid #2a2a2a}"
".row{margin-bottom:22px}"
"label{display:flex;justify-content:space-between;font-size:.8rem;"
"text-transform:uppercase;letter-spacing:1px;margin-bottom:8px}"
"label span{font-size:1rem;font-weight:700;min-width:38px;text-align:right}"
"input[type=range]{width:100%;-webkit-appearance:none;height:6px;border-radius:3px;"
"outline:none;cursor:pointer;transition:opacity .2s}"
"input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;"
"border-radius:50%;cursor:pointer;border:2px solid #fff}"
"#rSlider{background:linear-gradient(to right,#1a0000,#ff0000)}"
"#rSlider::-webkit-slider-thumb{background:#ff3333}"
"#gSlider{background:linear-gradient(to right,#001a00,#00ff00)}"
"#gSlider::-webkit-slider-thumb{background:#33ff33}"
"#bSlider{background:linear-gradient(to right,#00001a,#0000ff)}"
"#bSlider::-webkit-slider-thumb{background:#3333ff}"
".btn-off{width:100%;margin-top:10px;padding:12px;border:none;border-radius:10px;"
"background:#222;color:#888;font-size:.9rem;text-transform:uppercase;"
"letter-spacing:1px;cursor:pointer;transition:all .2s}"
".btn-off:hover{background:#333;color:#fff}"
"</style></head><body>"
"<div class='card'>"
"<h1>RGB Dashboard</h1>"
"<p class='sub'>ESP32-S3 Live Control</p>"
"<div id='preview' class='preview'></div>"

"<div class='row'>"
"<label>Red <span id='rv'>0</span></label>"
"<input type='range' id='rSlider' min='0' max='255' value='0'>"
"</div>"

"<div class='row'>"
"<label>Green <span id='gv'>0</span></label>"
"<input type='range' id='gSlider' min='0' max='255' value='0'>"
"</div>"

"<div class='row'>"
"<label>Blue <span id='bv'>0</span></label>"
"<input type='range' id='bSlider' min='0' max='255' value='0'>"
"</div>"

"<button class='btn-off' onclick='turnOff()'>Turn Off</button>"
"</div>"

"<script>"
// Per-slider independent timers — no slider waits on another
"var t={r:0,g:0,b:0},p={r:false,g:false,b:false},R=0,G=0,B=0;"
"function update(ch){"
"  R=+document.getElementById('rSlider').value;"
"  G=+document.getElementById('gSlider').value;"
"  B=+document.getElementById('bSlider').value;"
"  document.getElementById('rv').textContent=R;"
"  document.getElementById('gv').textContent=G;"
"  document.getElementById('bv').textContent=B;"
"  document.getElementById('preview').style.background='rgb('+R+','+G+','+B+')';"
"  var now=Date.now();"
"  if(now-t[ch]>30){"
"    sendRGB(R,G,B); t[ch]=now; p[ch]=false;"
"  } else if(!p[ch]){"
"    p[ch]=true;"
"    setTimeout(function(){sendRGB(R,G,B);t[ch]=Date.now();p[ch]=false;},30);"
"  }"
"}"
"function sendRGB(r,g,b){"
"  fetch('/api/rgb',{method:'POST',body:r+','+g+','+b,"
"    headers:{'Content-Type':'text/plain'}});"
"}"
"function turnOff(){"
"  document.getElementById('rSlider').value=0;"
"  document.getElementById('gSlider').value=0;"
"  document.getElementById('bSlider').value=0;"
"  update();"
"}"
// Sync sliders from ESP32 on page load
"fetch('/api/rgb_state').then(r=>r.json()).then(d=>{"
"  document.getElementById('rSlider').value=d.r;"
"  document.getElementById('gSlider').value=d.g;"
"  document.getElementById('bSlider').value=d.b;"
"  update();"
"});"
"document.getElementById('rSlider').addEventListener('input',function(){update('r')});"
"document.getElementById('gSlider').addEventListener('input',function(){update('g')});"
"document.getElementById('bSlider').addEventListener('input',function(){update('b')});"
"</script></body></html>";

// ===================================================================
//  LED UPDATE TASK  (dedicated task — never blocks HTTP server)
// ===================================================================
static void led_task(void *arg)
{
    while (1) {
        if (g_led_dirty) {
            g_led_dirty = false;
            led_strip_set_pixel(led_strip, 0, g_r, g_g, g_b);
            led_strip_refresh(led_strip);
        }
        vTaskDelay(pdMS_TO_TICKS(8)); // ~120Hz update rate — snappy response
    }
}

// ===================================================================
//  HTTP HANDLERS
// ===================================================================

// GET / → serve dashboard page
esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// POST /api/rgb  body: "R,G,B"  e.g. "255,128,0"
esp_err_t rgb_set_handler(httpd_req_t *req)
{
    char buf[16];
    int len = httpd_req_recv(req, buf, MIN(req->content_len, sizeof(buf) - 1));
    if (len <= 0) return ESP_FAIL;
    buf[len] = '\0';

    // Fast CSV parse — no malloc, no JSON library
    int r = 0, g = 0, b = 0;
    sscanf(buf, "%d,%d,%d", &r, &g, &b);

    // Clamp to 0–255
    g_r = (uint8_t)MIN(MAX(r, 0), 255);
    g_g = (uint8_t)MIN(MAX(g, 0), 255);
    g_b = (uint8_t)MIN(MAX(b, 0), 255);
    g_led_dirty = true; // signal led_task

    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

// GET /api/rgb_state → return current RGB as JSON (for page load sync)
esp_err_t rgb_state_handler(httpd_req_t *req)
{
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"r\":%d,\"g\":%d,\"b\":%d}", g_r, g_g, g_b);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static httpd_uri_t uri_root      = { .uri="/",             .method=HTTP_GET,  .handler=root_handler,     .user_ctx=NULL };
static httpd_uri_t uri_rgb_set   = { .uri="/api/rgb",      .method=HTTP_POST, .handler=rgb_set_handler,  .user_ctx=NULL };
static httpd_uri_t uri_rgb_state = { .uri="/api/rgb_state",.method=HTTP_GET,  .handler=rgb_state_handler,.user_ctx=NULL };

static void start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_rgb_set);
        httpd_register_uri_handler(server, &uri_rgb_state);
        ESP_LOGI(TAG, "HTTP server started!");
    }
}

// ===================================================================
//  WI-FI
// ===================================================================
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "Retrying Wi-Fi...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR " — open this in your browser!", IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h1, h2;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,  &wifi_event_handler, NULL, &h1));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &h2));

    wifi_config_t wifi_cfg = {
        .sta = {
            .ssid     = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
}

// ===================================================================
//  BOOT BUTTON ISR  (resets LED to off when pressed)
// ===================================================================
static QueueHandle_t gpio_evt_queue;

static void IRAM_ATTR gpio_isr_handler(void *arg)
{
    uint32_t pin = (uint32_t)arg;
    xQueueSendFromISR(gpio_evt_queue, &pin, NULL);
}

static void button_task(void *arg)
{
    uint32_t pin;
    TickType_t last_t = 0;
    while (1) {
        if (xQueueReceive(gpio_evt_queue, &pin, portMAX_DELAY)) {
            TickType_t now = xTaskGetTickCount();
            if ((now - last_t) < pdMS_TO_TICKS(50)) continue;
            last_t = now;

            // Pressing the boot button resets the LED to off
            if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
                g_r = g_g = g_b = 0;
                g_led_dirty = true;
                ESP_LOGI(TAG, "Boot button: LED reset to off");
            }
        }
    }
}

// ===================================================================
//  MAIN
// ===================================================================
void app_main(void)
{
    ESP_LOGI(TAG, "Booting ESP32 RGB Dashboard...");

    // --- NVS (required for Wi-Fi) ---
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // --- LED Strip ---
    led_strip_config_t strip_cfg = {
        .strip_gpio_num        = RGB_LED_GPIO,
        .max_leds              = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model             = LED_MODEL_WS2812,
        .flags.invert_out      = false,
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src       = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &led_strip));
    ESP_ERROR_CHECK(led_strip_clear(led_strip));

    // --- LED update task (priority 5) ---
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, NULL);

    // --- Boot button ISR ---
    gpio_config_t btn_cfg = {
        .pin_bit_mask = (1ULL << BOOT_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .intr_type    = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&btn_cfg);
    gpio_evt_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(button_task, "btn_task", 2048, NULL, 5, NULL);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_BUTTON_GPIO, gpio_isr_handler, (void *)BOOT_BUTTON_GPIO);

    // --- Wi-Fi ---
    wifi_init();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        start_webserver();
    } else {
        ESP_LOGE(TAG, "Wi-Fi failed. Check SSID/Password in menuconfig.");
    }
}
