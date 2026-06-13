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
"<title>RGB Sync</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<style>"
":root{--glass:rgba(255,255,255,0.08);--border:rgba(255,255,255,0.15);}"
"*{box-sizing:border-box;margin:0;padding:0;font-family:'Inter',system-ui,sans-serif}"
"body{background:linear-gradient(135deg,#0f172a 0%,#020617 100%);color:#f8fafc;"
"display:flex;flex-direction:column;align-items:center;justify-content:center;min-height:100vh;padding:20px}"
".card{background:var(--glass);backdrop-filter:blur(16px);-webkit-backdrop-filter:blur(16px);"
"border:1px solid var(--border);border-radius:24px;padding:40px;width:100%;max-width:400px;"
"box-shadow:0 25px 50px -12px rgba(0,0,0,0.5)}"
"h1{font-size:2rem;font-weight:700;letter-spacing:-0.5px;margin-bottom:8px;text-align:center;"
"background:linear-gradient(to right,#fff,#94a3b8);-webkit-background-clip:text;-webkit-text-fill-color:transparent}"
"p.sub{font-size:0.9rem;color:#94a3b8;text-align:center;margin-bottom:32px}"
".preview-container{position:relative;margin-bottom:40px;display:flex;justify-content:center}"
".preview{width:120px;height:120px;border-radius:50%;background:#000;"
"border:2px solid var(--border);transition:all 0.1s ease-out;position:relative;z-index:2}"
".glow{position:absolute;width:120px;height:120px;border-radius:50%;background:#000;"
"filter:blur(30px);opacity:0.6;transition:all 0.1s ease-out;z-index:1}"
".row{margin-bottom:24px}"
"label{display:flex;justify-content:space-between;font-size:0.85rem;font-weight:600;"
"text-transform:uppercase;letter-spacing:1px;margin-bottom:12px;color:#cbd5e1}"
"label span{font-variant-numeric:tabular-nums;background:rgba(0,0,0,0.3);padding:2px 8px;"
"border-radius:6px;min-width:44px;text-align:center}"
"input[type=range]{width:100%;-webkit-appearance:none;height:8px;border-radius:4px;"
"outline:none;cursor:pointer;background:rgba(0,0,0,0.3);border:1px solid rgba(255,255,255,0.05)}"
"input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;"
"border-radius:50%;cursor:pointer;border:3px solid #fff;background:#0f172a;"
"box-shadow:0 0 10px rgba(0,0,0,0.5);transition:transform 0.1s}"
"input[type=range]::-webkit-slider-thumb:active{transform:scale(1.1)}"
"#rSlider::-webkit-slider-thumb{border-color:#ef4444}"
"#gSlider::-webkit-slider-thumb{border-color:#22c55e}"
"#bSlider::-webkit-slider-thumb{border-color:#3b82f6}"
".btn-off{width:100%;margin-top:16px;padding:16px;border:1px solid var(--border);"
"border-radius:14px;background:rgba(0,0,0,0.2);color:#cbd5e1;font-size:1rem;font-weight:600;"
"cursor:pointer;transition:all 0.2s;backdrop-filter:blur(4px)}"
".btn-off:hover{background:rgba(239,68,68,0.1);color:#ef4444;border-color:rgba(239,68,68,0.3)}"
"</style></head><body>"
"<div class='card'>"
"<h1>RGB Sync</h1>"
"<p class='sub'>ESP32 Wireless Controller</p>"
"<div class='preview-container'>"
"  <div id='glow' class='glow'></div>"
"  <div id='preview' class='preview'></div>"
"</div>"
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
"var t={r:0,g:0,b:0},p={r:false,g:false,b:false},R=0,G=0,B=0;"
"function update(ch){"
"  R=+document.getElementById('rSlider').value;"
"  G=+document.getElementById('gSlider').value;"
"  B=+document.getElementById('bSlider').value;"
"  document.getElementById('rv').textContent=R;"
"  document.getElementById('gv').textContent=G;"
"  document.getElementById('bv').textContent=B;"
"  var col='rgb('+R+','+G+','+B+')';"
"  document.getElementById('preview').style.background=col;"
"  document.getElementById('glow').style.background=col;"
"  var now=Date.now();"
"  if(now-t[ch]>30){"
"    sendRGB(R,G,B); t[ch]=now; p[ch]=false;"
"  } else if(!p[ch]){"
"    p[ch]=true;"
"    setTimeout(function(){sendRGB(R,G,B);t[ch]=Date.now();p[ch]=false;},30);"
"  }"
"}"
"function sendRGB(r,g,b){fetch('/api/rgb',{method:'POST',body:r+','+g+','+b,headers:{'Content-Type':'text/plain'}});}"
"function turnOff(){document.getElementById('rSlider').value=0;document.getElementById('gSlider').value=0;document.getElementById('bSlider').value=0;update();}"
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
