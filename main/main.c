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
"<title>Chroma Control</title>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<link href='https://fonts.googleapis.com/css2?family=Outfit:wght@300;500;700&display=swap' rel='stylesheet'>"
"<style>"
":root{--bg:#030712;--card:rgba(17,24,39,0.7);--border:rgba(255,255,255,0.08)}"
"*{box-sizing:border-box;margin:0;padding:0;font-family:'Outfit',sans-serif}"
"body{background:var(--bg);color:#f9fafb;display:flex;align-items:center;justify-content:center;min-height:100vh;overflow:hidden;position:relative}"
".bg-mesh{position:absolute;top:-50%;left:-50%;width:200%;height:200%;background:radial-gradient(circle at 50% 50%,rgba(56,189,248,0.05) 0%,transparent 50%),radial-gradient(circle at 80% 20%,rgba(139,92,246,0.05) 0%,transparent 50%);z-index:0;animation:meshRotate 30s linear infinite}"
"@keyframes meshRotate{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}"
".card{position:relative;z-index:10;background:var(--card);backdrop-filter:blur(24px);-webkit-backdrop-filter:blur(24px);border:1px solid var(--border);border-radius:32px;padding:48px 40px;width:90%;max-width:420px;box-shadow:0 25px 50px -12px rgba(0,0,0,0.5),inset 0 1px 0 rgba(255,255,255,0.1)}"
"h1{font-size:2.2rem;font-weight:700;text-align:center;margin-bottom:8px;background:linear-gradient(135deg,#fff 0%,#a1a1aa 100%);-webkit-background-clip:text;-webkit-text-fill-color:transparent;letter-spacing:-0.5px}"
"p.sub{font-size:0.95rem;font-weight:300;color:#9ca3af;text-align:center;margin-bottom:40px}"
".orb-container{position:relative;width:140px;height:140px;margin:0 auto 48px;display:flex;align-items:center;justify-content:center}"
".orb-glow{position:absolute;width:100%;height:100%;border-radius:50%;filter:blur(35px);opacity:0.5;transition:background-color 0.3s,opacity 0.3s}"
".orb{position:relative;width:120px;height:120px;border-radius:50%;background:#000;box-shadow:inset 0 -10px 20px rgba(0,0,0,0.5),inset 0 5px 15px rgba(255,255,255,0.2),0 10px 30px rgba(0,0,0,0.5);transition:background-color 0.3s;z-index:2;border:1px solid rgba(255,255,255,0.1)}"
".orb::after{content:'';position:absolute;top:8px;left:20px;width:40px;height:15px;background:rgba(255,255,255,0.4);border-radius:50%;filter:blur(3px);transform:rotate(-30deg)}"
".slider-group{margin-bottom:28px}"
".slider-header{display:flex;justify-content:space-between;align-items:center;margin-bottom:12px}"
".slider-label{font-size:0.85rem;font-weight:500;color:#d1d5db;text-transform:uppercase;letter-spacing:1.5px}"
".slider-val{font-variant-numeric:tabular-nums;font-weight:700;font-size:0.9rem;color:#fff;background:rgba(255,255,255,0.05);padding:4px 10px;border-radius:8px;min-width:48px;text-align:center;border:1px solid rgba(255,255,255,0.05)}"
"input[type=range]{-webkit-appearance:none;width:100%;height:6px;border-radius:3px;background:#374151;outline:none;transition:0.2s}"
"input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:24px;height:24px;border-radius:50%;background:#fff;cursor:pointer;box-shadow:0 4px 10px rgba(0,0,0,0.3);transition:transform 0.1s,box-shadow 0.1s}"
"input[type=range]::-webkit-slider-thumb:hover{transform:scale(1.15);box-shadow:0 6px 15px rgba(0,0,0,0.4)}"
"#rSlider::-webkit-slider-thumb{border:4px solid #ef4444}"
"#gSlider::-webkit-slider-thumb{border:4px solid #10b981}"
"#bSlider::-webkit-slider-thumb{border:4px solid #3b82f6}"
".btn-off{display:block;width:100%;margin-top:10px;padding:18px;border:none;border-radius:16px;background:linear-gradient(135deg,rgba(255,255,255,0.05) 0%,rgba(255,255,255,0.02) 100%);color:#e5e7eb;font-size:1.05rem;font-weight:500;letter-spacing:0.5px;cursor:pointer;transition:all 0.3s cubic-bezier(0.4,0,0.2,1);box-shadow:inset 0 1px 0 rgba(255,255,255,0.05),0 4px 6px rgba(0,0,0,0.1)}"
".btn-off:hover{background:linear-gradient(135deg,rgba(239,68,68,0.1) 0%,rgba(239,68,68,0.05) 100%);color:#ef4444;transform:translateY(-2px);box-shadow:inset 0 1px 0 rgba(239,68,68,0.2),0 8px 15px rgba(239,68,68,0.15)}"
".btn-off:active{transform:translateY(0)}"
"</style></head><body>"
"<div class='bg-mesh'></div>"
"<div class='card'>"
"<h1>Chroma Control</h1>"
"<p class='sub'>Studio RGB Lighting</p>"
"<div class='orb-container'>"
"<div id='orbGlow' class='orb-glow'></div>"
"<div id='orb' class='orb'></div>"
"</div>"
"<div class='slider-group'>"
"<div class='slider-header'><span class='slider-label'>Red</span><span class='slider-val' id='rv'>0</span></div>"
"<input type='range' id='rSlider' min='0' max='255' value='0'>"
"</div>"
"<div class='slider-group'>"
"<div class='slider-header'><span class='slider-label'>Green</span><span class='slider-val' id='gv'>0</span></div>"
"<input type='range' id='gSlider' min='0' max='255' value='0'>"
"</div>"
"<div class='slider-group'>"
"<div class='slider-header'><span class='slider-label'>Blue</span><span class='slider-val' id='bv'>0</span></div>"
"<input type='range' id='bSlider' min='0' max='255' value='0'>"
"</div>"
"<button class='btn-off' onclick='turnOff()'>Power Off</button>"
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
"  document.getElementById('orb').style.backgroundColor=col;"
"  document.getElementById('orbGlow').style.backgroundColor=col;"
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
        vTaskDelay(pdMS_TO_TICKS(20)); // ~50Hz update rate — snappy response while feeding watchdog
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
//  WI-FI & mDNS
// ===================================================================
#include "mdns.h"

static void start_mdns_service(void)
{
    ESP_ERROR_CHECK(mdns_init());
    ESP_ERROR_CHECK(mdns_hostname_set("chroma"));
    ESP_ERROR_CHECK(mdns_instance_name_set("ESP32 RGB Controller"));
    
    // Add mDNS service for HTTP
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS initialized. You can now access http://chroma.local");
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // Infinite retries for headless operation
        esp_wifi_connect();
        s_retry_num++;
        ESP_LOGI(TAG, "Retrying Wi-Fi... (attempt %d)", s_retry_num);
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
        start_mdns_service();
        start_webserver();
    } else {
        ESP_LOGE(TAG, "Wi-Fi failed. Check SSID/Password in menuconfig.");
    }
}
