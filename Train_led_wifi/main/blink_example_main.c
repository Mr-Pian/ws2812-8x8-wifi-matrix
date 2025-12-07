/* ESP32-C3 LED Matrix Web Server (v3.1)
 * 功能：开机动画 -> 联网 (带超时处理) -> 成功(显示IP+打钩) / 失败(显示TIMEOUT+打叉) -> 启动画板
 */

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h" 
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "cJSON.h"
#include "led_strip.h"
#include "driver/gpio.h"
#include "font8x8.h" 
#include "math.h"

static const char *TAG = "matrix_main";

// --- 用户配置区域 ---
#define WIFI_SSID      "auto_kx_D710"
#define WIFI_PASS      "31130100"
#define LED_STRIP_GPIO 3
#define MATRIX_WIDTH   8
#define MATRIX_HEIGHT  8
#define WIFI_TIMEOUT_MS 10000 // 连接超时时间 (10秒)

// 全局变量
static led_strip_handle_t led_strip;
static char s_ip_addr_str[16] = "0.0.0.0"; 

// WiFi 连接事件组
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* ================== LED 驱动与图形函数 ================== */

void matrix_set_pixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return;

    uint32_t index;
    // 蛇形走线 
    if (y % 2 == 0) {
        index = y * MATRIX_WIDTH + x;
    } else {
        index = y * MATRIX_WIDTH + (MATRIX_WIDTH - 1 - x);
    }
    led_strip_set_pixel(led_strip, index, r, g, b);
}

// 辅助函数：根据线性索引点亮 (支持亮度)
void set_pixel_by_index(int index, int color_val, int brightness_percent) {
    int x = index % 8;
    int y = index / 8;
    
    uint8_t r = (color_val >> 16) & 0xFF;
    uint8_t g = (color_val >> 8) & 0xFF;
    uint8_t b = color_val & 0xFF;

    if (brightness_percent < 0) brightness_percent = 0;
    if (brightness_percent > 100) brightness_percent = 100;

    r = (r * brightness_percent) / 100;
    g = (g * brightness_percent) / 100;
    b = (b * brightness_percent) / 100;

    matrix_set_pixel(7-x, y, r, g, b); 
}

// 滚动显示文字函数
void scroll_text(const char *text, int speed_ms, uint8_t r, uint8_t g, uint8_t b)
{
    int len = strlen(text);
    int total_columns = len * 8 + 8; 

    for (int offset = 0; offset < total_columns; offset++) {
        led_strip_clear(led_strip);

        for (int x = 0; x < 8; x++) {
            int current_msg_col = offset + x - 8; 

            if (current_msg_col >= 0 && current_msg_col < len * 8) {
                int char_idx = current_msg_col / 8;
                int col_in_char = current_msg_col % 8;
                
                int char_code = (int)text[char_idx];
                if (char_code < 0 || char_code > 127) char_code = '?';

                uint8_t col_data = font8x8[char_code][col_in_char];

                for (int y = 0; y < 8; y++) {
                    if (col_data & (1 << y)) {
                        // 镜像修正：根据你的硬件情况，这里可能需要改 7-x 或 7-y
                        // 之前代码里用的是 7-x，这里保持一致
                        matrix_set_pixel(7-x, y, r, g, b);
                    }
                }
            }
        }
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(speed_ms));
    }
}

static void configure_led(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = 64, 
        .led_model = LED_MODEL_WS2812,
        .flags.invert_out = false,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, 
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
}

/* ================== HTTP Server ================== */

static esp_err_t matrix_options_handler(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t matrix_post_handler(httpd_req_t *req)
{
    char content[1024];
    size_t recv_size = MIN(req->content_len, sizeof(content));

    int ret = httpd_req_recv(req, content, recv_size);
    if (ret <= 0) {  
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) httpd_resp_send_408(req);
        return ESP_FAIL;
    }
    content[ret] = '\0'; 

    cJSON *root = cJSON_Parse(content);
    if (root) {
        int brightness = 20;
        cJSON *bri_item = cJSON_GetObjectItem(root, "brightness");
        if (bri_item) brightness = bri_item->valueint;

        cJSON *data_array = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsArray(data_array)) {
            led_strip_clear(led_strip); 
            int array_size = cJSON_GetArraySize(data_array);
            for (int i = 0; i < array_size && i < 64; i++) {
                cJSON *item = cJSON_GetArrayItem(data_array, i);
                if (item) set_pixel_by_index(i, item->valueint, brightness);
            }
            led_strip_refresh(led_strip);
        }
        cJSON_Delete(root);
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, "{\"status\":\"ok\"}", -1);
    return ESP_OK;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_open_sockets = 5; 
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_opt = { .uri = "/api/matrix", .method = HTTP_OPTIONS, .handler = matrix_options_handler };
        httpd_register_uri_handler(server, &uri_opt);
        httpd_uri_t uri_post = { .uri = "/api/matrix", .method = HTTP_POST, .handler = matrix_post_handler };
        httpd_register_uri_handler(server, &uri_post);
        return server;
    }
    return NULL;
}

/* ================== WiFi ================== */

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        // ESP_LOGI(TAG, "Retry connecting to WiFi..."); // 减少日志刷屏
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        sprintf(s_ip_addr_str, IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Got IP: %s", s_ip_addr_str);
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        start_webserver();
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate(); 

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA_PSK,
            .pmf_cfg = { .capable = true, .required = false },
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    
    esp_wifi_set_ps(WIFI_PS_NONE);
    esp_wifi_set_max_tx_power(52);  //限制wifi功率
}

/* ================== 图形动画函数 ================== */

// 1. 炫酷开机动画
void play_startup_animation(void)
{
    float center_x = 3.5;
    float center_y = 3.5;
    float max_radius = 6.0;

    for (float r = 0; r < max_radius; r += 0.5) {
        led_strip_clear(led_strip);
        
        for (int x = 0; x < 8; x++) {
            for (int y = 0; y < 8; y++) {
                float dx = x - center_x;
                float dy = y - center_y;
                float dist = sqrt(dx*dx + dy*dy);

                if (dist <= r) {
                    int brightness = 15; 
                    uint8_t red = (dist < 1.5) ? 100 : 0;   
                    uint8_t green = 255 - (dist * 30);      
                    if (green > 255) green = 0;             
                    uint8_t blue = 200;                     

                    matrix_set_pixel(x, y, 
                                     (red * brightness)/100, 
                                     (green * brightness)/100, 
                                     (blue * brightness)/100);
                }
            }
        }
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(80)); 
    }

    for(int i=0; i<64; i++) {
        led_strip_set_pixel(led_strip, i, 30, 30, 30); 
    }
    led_strip_refresh(led_strip);
    vTaskDelay(pdMS_TO_TICKS(100));

    led_strip_clear(led_strip);
    led_strip_refresh(led_strip);
    vTaskDelay(pdMS_TO_TICKS(500));
}

// 2. 绘制待机图标：绿色圆圈 + 对勾
void draw_success_icon(void)
{
    led_strip_clear(led_strip);
    uint8_t R = 0;
    uint8_t G = 15; 
    uint8_t B = 0;

    // 画圆圈
    for(int x=2; x<=5; x++) { matrix_set_pixel(x, 0, R, G, B); matrix_set_pixel(x, 7, R, G, B); }
    for(int y=2; y<=5; y++) { matrix_set_pixel(0, y, R, G, B); matrix_set_pixel(7, y, R, G, B); }
    matrix_set_pixel(1, 1, R, G, B); matrix_set_pixel(6, 1, R, G, B);
    matrix_set_pixel(1, 6, R, G, B); matrix_set_pixel(6, 6, R, G, B);

    // 画对勾
    uint8_t G_tick = 30;
    matrix_set_pixel(7-2, 4, 0, G_tick, 0); // 注意：这里坐标我保留了之前的镜像修正逻辑
    matrix_set_pixel(7-3, 5, 0, G_tick, 0);
    matrix_set_pixel(7-4, 4, 0, G_tick, 0);
    matrix_set_pixel(7-5, 3, 0, G_tick, 0);

    led_strip_refresh(led_strip);
}

/**
 * @brief 【修改】绘制失败图标：红色圆圈 + 红色叉
 */
void draw_failure_icon(void)
{
    led_strip_clear(led_strip);
    
    // 设置颜色：暗红色 (R=20, G=0, B=0)
    // 保持低亮度，避免刺眼
    uint8_t R = 20; 
    uint8_t G = 0;
    uint8_t B = 0;

    // --- 1. 画圆圈 (外围轮廓) ---
    // 上下边框 (避开角)
    for(int x=2; x<=5; x++) { 
        matrix_set_pixel(x, 0, R, G, B); 
        matrix_set_pixel(x, 7, R, G, B); 
    }
    // 左右边框 (避开角)
    for(int y=2; y<=5; y++) { 
        matrix_set_pixel(0, y, R, G, B); 
        matrix_set_pixel(7, y, R, G, B); 
    }
    // 四个圆角
    matrix_set_pixel(1, 1, R, G, B); matrix_set_pixel(6, 1, R, G, B);
    matrix_set_pixel(1, 6, R, G, B); matrix_set_pixel(6, 6, R, G, B);

    // --- 2. 画叉 (内部对角线) ---
    // 只画中间部分 (从索引 2 到 5)，这样叉就在圆圈内部了
    for (int i = 2; i <= 5; i++) {
        matrix_set_pixel(i, i, R, G, B);     // 左上到右下
        matrix_set_pixel(i, 7-i, R, G, B);   // 左下到右上
    }

    led_strip_refresh(led_strip);
}

/* ================== 主程序逻辑 ================== */
void app_main(void)
{
    // 1. Init NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Init LED
    configure_led();

    // 3. 播放开机动画
    ESP_LOGI(TAG, "Startup Animation...");
    play_startup_animation(); 

    vTaskDelay(pdMS_TO_TICKS(500));

    scroll_text("Center4Maker by Mao", 60, 15, 15, 15);

    // 4. 开始连 WiFi
    ESP_LOGI(TAG, "Connecting WiFi...");
    wifi_init_sta(); 

    // 5. 等待连接动画 (带超时处理)
    int frame = 0;
    bool is_connected = false;
    // 50ms 一帧，30000ms / 50ms = 600 帧
    int max_frames = WIFI_TIMEOUT_MS / 50; 

    while (frame < max_frames) {
        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            is_connected = true;
            break;
        }

        // 呼吸灯动画
        led_strip_clear(led_strip);
        int brightness = (frame % 40); 
        if (brightness > 20) brightness = 40 - brightness; 
        
        // 黄色呼吸灯
        matrix_set_pixel(3, 3, brightness, brightness, 0); 
        matrix_set_pixel(3, 4, brightness, brightness, 0);
        matrix_set_pixel(4, 3, brightness, brightness, 0);
        matrix_set_pixel(4, 4, brightness, brightness, 0);
        
        led_strip_refresh(led_strip);
        vTaskDelay(pdMS_TO_TICKS(50));
        frame++;
    }

    // 6. 结果判定分支
    if (is_connected) {
        // --- 成功分支 ---
        ESP_LOGI(TAG, "WiFi Connected!");
        
        // 瞬间绿屏
        led_strip_clear(led_strip);
        vTaskDelay(pdMS_TO_TICKS(200));
        
        // 滚动显示 IP (暗青色)
        scroll_text(s_ip_addr_str, 60, 0, 15, 15);

        // 显示对勾
        ESP_LOGI(TAG, "System Ready.");
        draw_success_icon(); 

        // 正常工作，等待网页指令
        while(1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

    } else {
        // --- 失败分支 ---
        ESP_LOGE(TAG, "WiFi Connection Timeout!");
        
        // 滚动显示报错信息 (暗红色)
        scroll_text("TIMEOUT", 100, 20, 0, 0);

        // 显示红叉
        draw_failure_icon();
        
        // 如果失败了，就一直停在这里显示红叉，或者你想让它重启
        while(1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            // 如果你想让它过一会重启重试，可以取消下面这行的注释：
            esp_restart(); 
        }
    }
}