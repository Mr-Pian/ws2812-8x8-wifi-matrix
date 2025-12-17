/* ESP32-C3 LED Matrix Web Server (v4.0)
 * * 核心功能：
 * 1. 启动流程：开机动画 -> 滚动文字 -> 连接WiFi -> 状态指示(IP/超时)
 * 2. Web控制：支持亮度调节和像素数组下发
 * 3. 物理按键 (GPIO 10)：
 * - 单击开关屏幕
 * - 关灯模式：仅熄灭LED，显存(Buffer)数据保留
 * - 开灯模式：从显存恢复之前的画面
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

// --- 配置参数 ---
#define WIFI_SSID       "auto_kx_D710"
#define WIFI_PASS       "31130100"
#define LED_STRIP_GPIO  3
#define MATRIX_WIDTH    8
#define MATRIX_HEIGHT   8
#define WIFI_TIMEOUT_MS 10000
#define GPIO_INPUT_PIN  10    // 物理按键

// --- 全局变量 ---
static led_strip_handle_t led_strip;
static char s_ip_addr_str[16] = "0.0.0.0";

// 像素结构
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} pixel_color_t;

// 显存缓冲区 (Shadow Buffer) - 用于关灯时保存状态
static pixel_color_t s_screen_buffer[64];

// 屏幕开关状态标志
volatile bool g_display_enable = false;

// WiFi事件组
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

/* ================== LED 驱动层 ================== */

// 刷新LED：只有在开启状态下才推数据
void matrix_refresh() {
    if (g_display_enable) {
        led_strip_refresh(led_strip);
    }
}

// 彻底清屏：同时清除显存和物理LED状态
void matrix_clear_all(void)
{
    // 如果屏幕亮着，先灭物理灯
    if (g_display_enable) {
        led_strip_clear(led_strip);
    }
    // 同步清空显存，防止开灯瞬间闪烁旧数据
    memset(s_screen_buffer, 0, sizeof(s_screen_buffer));
}

uint32_t pos_to_index(uint8_t x, uint8_t y) {
    uint32_t index=0;

    // TODO: 根据实际走线（如S型/Z型）计算物理index

    return index;
}

// 写像素核心函数
void matrix_set_pixel(uint8_t x, uint8_t y, uint8_t r, uint8_t g, uint8_t b)
{
    if (x >= MATRIX_WIDTH || y >= MATRIX_HEIGHT) return;

    // 1. 计算物理位置
    uint32_t index;
    index = pos_to_index(x, y);

    // 2. 始终更新显存 (保证后台数据同步)
    s_screen_buffer[index].r = r;
    s_screen_buffer[index].g = g;
    s_screen_buffer[index].b = b;

    // 3. 若屏幕开启，同步写入硬件
    if (g_display_enable) {
        led_strip_set_pixel(led_strip, index, r, g, b);
    }
}

// 辅助：按线性索引设置像素 (带亮度处理)
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

    // 注意：这里做了x轴翻转处理 (7-x)，视具体硬件摆放调整
    matrix_set_pixel(7-x, y, r, g, b);
}

/* ================== 绘图与动画 ================== */

void scroll_text(const char *text, int speed_ms, uint8_t r, uint8_t g, uint8_t b)
{
    int len = strlen(text);
    int total_columns = len * 8 + 8;

    for (int offset = 0; offset < total_columns; offset++) {
        // 帧清空，防止残影
        matrix_clear_all();

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
                        matrix_set_pixel(7-x, y, r, g, b);
                    }
                }
            }
        }
        matrix_refresh();
        vTaskDelay(pdMS_TO_TICKS(speed_ms));
    }
}

void play_startup_animation(void)
{
    float center_x = 3.5;
    float center_y = 3.5;
    float max_radius = 6.0;

    // 扩散圆环动画
    for (float r = 0; r < max_radius; r += 0.5) {
        matrix_clear_all(); // 帧重置

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

                    matrix_set_pixel(x, y, (red * brightness)/100, (green * brightness)/100, (blue * brightness)/100);
                }
            }
        }
        matrix_refresh();
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    // 闪白光特效
    for(int i=0; i<64; i++) {
        int x = i % 8; int y = i / 8;
        matrix_set_pixel(x, y, 30, 30, 30);
    }
    matrix_refresh();
    vTaskDelay(pdMS_TO_TICKS(100));

    matrix_clear_all();
    matrix_refresh();
    vTaskDelay(pdMS_TO_TICKS(500));
}

void draw_success_icon(void)
{
    matrix_clear_all(); // 清除之前的文字残留

    uint8_t R = 0, G = 15, B = 0;

    // 绘制外框
    for(int x=2; x<=5; x++) { matrix_set_pixel(x, 0, R, G, B); matrix_set_pixel(x, 7, R, G, B); }
    for(int y=2; y<=5; y++) { matrix_set_pixel(0, y, R, G, B); matrix_set_pixel(7, y, R, G, B); }
    matrix_set_pixel(1, 1, R, G, B); matrix_set_pixel(6, 1, R, G, B);
    matrix_set_pixel(1, 6, R, G, B); matrix_set_pixel(6, 6, R, G, B);

    // 绘制对勾
    uint8_t G_tick = 30;
    matrix_set_pixel(7-2, 4, 0, G_tick, 0);
    matrix_set_pixel(7-3, 5, 0, G_tick, 0);
    matrix_set_pixel(7-4, 4, 0, G_tick, 0);
    matrix_set_pixel(7-5, 3, 0, G_tick, 0);

    matrix_refresh();
}

void draw_failure_icon(void)
{
    matrix_clear_all();

    uint8_t R = 20, G = 0, B = 0;

    // 外框
    for(int x=2; x<=5; x++) { matrix_set_pixel(x, 0, R, G, B); matrix_set_pixel(x, 7, R, G, B); }
    for(int y=2; y<=5; y++) { matrix_set_pixel(0, y, R, G, B); matrix_set_pixel(7, y, R, G, B); }
    matrix_set_pixel(1, 1, R, G, B); matrix_set_pixel(6, 1, R, G, B);
    matrix_set_pixel(1, 6, R, G, B); matrix_set_pixel(6, 6, R, G, B);

    // 叉号
    for (int i = 2; i <= 5; i++) {
        matrix_set_pixel(i, i, R, G, B);
        matrix_set_pixel(i, 7-i, R, G, B);
    }

    matrix_refresh();
}

/* ================== 硬件初始化 ================== */

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

    matrix_clear_all();
    matrix_refresh();
}

/* ================== 按键监控任务 ================== */

void turn_on_and_off_led(void *pvParameters)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = (1ULL << GPIO_INPUT_PIN);
    io_conf.mode = GPIO_MODE_INPUT;
    // 启用内部上拉，低电平触发
    io_conf.pull_up_en = 1;
    io_conf.pull_down_en = 0;
    gpio_config(&io_conf);

    ESP_LOGI(TAG, "Button Monitor Task Started");

    while (1) {
        if (gpio_get_level(GPIO_INPUT_PIN) == 0) {

            // 简单消抖
            vTaskDelay(pdMS_TO_TICKS(50));
            if (gpio_get_level(GPIO_INPUT_PIN) == 0) {

                // 翻转显示状态
                g_display_enable = !g_display_enable;

                if (g_display_enable) {
                    // 开灯：将Shadow Buffer的数据刷回灯珠
                    ESP_LOGI(TAG, "Display ON: Restoring buffer...");
                    for (int i = 0; i < 64; i++) {
                        led_strip_set_pixel(led_strip, i,
                                            s_screen_buffer[i].r,
                                            s_screen_buffer[i].g,
                                            s_screen_buffer[i].b);
                    }
                    led_strip_refresh(led_strip);
                }
                else {
                    // 关灯：仅清空物理灯珠，不清除Buffer
                    ESP_LOGI(TAG, "Display OFF: Saving power...");
                    led_strip_clear(led_strip);
                    led_strip_refresh(led_strip);
                }

                // 等待释放
                while (gpio_get_level(GPIO_INPUT_PIN) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
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

            // 收到新画面前先清屏，避免叠加
            matrix_clear_all();

            int array_size = cJSON_GetArraySize(data_array);
            for (int i = 0; i < array_size && i < 64; i++) {
                cJSON *item = cJSON_GetArrayItem(data_array, i);
                if (item) set_pixel_by_index(i, item->valueint, brightness);
            }
            matrix_refresh();
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

/* ================== WiFi Logic ================== */

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
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
    esp_wifi_set_max_tx_power(52);
}

/* ================== Main ================== */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 1. 硬件初始化 (Buffer置0)
    configure_led();

    // 2. 启动按键扫描任务
    xTaskCreate(turn_on_and_off_led, "btn_task", 2048, NULL, 5, NULL);

    // 3. 播放开机动画
    ESP_LOGI(TAG, "Startup Animation...");
    play_startup_animation();

    scroll_text("Center4Maker by Mao", 60, 15, 15, 15);

    // 4. WiFi连接
    ESP_LOGI(TAG, "Connecting WiFi...");
    wifi_init_sta();

    // 5. 等待连接 (带超时动画)
    int frame = 0;
    bool is_connected = false;
    int max_frames = WIFI_TIMEOUT_MS / 50;

    while (frame < max_frames) {
        EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
        if (bits & WIFI_CONNECTED_BIT) {
            is_connected = true;
            break;
        }

        // 连接过程中的呼吸灯效果
        matrix_clear_all();
        int brightness = (frame % 40);
        if (brightness > 20) brightness = 40 - brightness;

        // 中心黄色呼吸
        matrix_set_pixel(3, 3, brightness, brightness, 0);
        matrix_set_pixel(3, 4, brightness, brightness, 0);
        matrix_set_pixel(4, 3, brightness, brightness, 0);
        matrix_set_pixel(4, 4, brightness, brightness, 0);

        matrix_refresh();
        vTaskDelay(pdMS_TO_TICKS(50));
        frame++;
    }

    // 6. 结果判定
    if (is_connected) {
        ESP_LOGI(TAG, "WiFi Connected!");

        // 绿屏提示
        matrix_clear_all();
        for(int i=0; i<64; i++) {
            int x=i%8, y=i/8;
            matrix_set_pixel(x,y, 0, 10, 0);
        }
        matrix_refresh();
        vTaskDelay(pdMS_TO_TICKS(700));

        // 滚动IP地址
        scroll_text(s_ip_addr_str, 60, 0, 15, 15);

        // 显示常驻对勾，系统就绪
        ESP_LOGI(TAG, "System Ready.");
        draw_success_icon();

        while(1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }

    } else {
        ESP_LOGE(TAG, "WiFi Connection Timeout!");

        scroll_text("TIMEOUT", 100, 20, 0, 0);
        draw_failure_icon();

        while(1) {
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
    }
}
