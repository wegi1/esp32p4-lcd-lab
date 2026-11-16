#include "wifi_ntp.hpp"
#include "rtc.hpp"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include <stdlib.h>
#include "esp_http_client.h"
#include "wifi_portal.hpp"

extern "C"
{
#include "rx8025t.h"
}

// Deklaracja Twojej istniejącej funkcji z main.cpp
extern void sync_internal_rtc_with_rx8025t(void);
extern volatile bool ntp_read;
extern volatile bool temp_read;

static const char *TAG = "WIFI_NTP";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static bool s_wifi_stack_initialized = false;
static volatile bool s_sync_in_progress = false;
static volatile bool s_weather_in_progress = false;

bool is_weather_fetch_running(void)
{
    return s_weather_in_progress;
}

static int s_retry_num = 0;
static const int MAX_RETRY = 3;

static void print_sram_debug(const char *etap)
{
    size_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    size_t min_free_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    printf("[SRAM - %s] Wolne teraz: %u KB | Minimum w historii: %u KB\n",
           etap, free_internal / 1024, min_free_internal / 1024);
}

bool is_ntp_sync_running(void)
{
    return s_sync_in_progress;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_num < MAX_RETRY)
        {
            s_retry_num++;
            ESP_LOGW(TAG, "Ponawiam polaczenie z Wi-Fi (%d/%d)...", s_retry_num, MAX_RETRY);
            esp_wifi_connect();
        }
        else
        {
            if (s_wifi_event_group != NULL)
            {
                xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
            }
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        uint8_t mac[6] = {0};
        esp_wifi_get_mac(WIFI_IF_STA, mac);

        ESP_LOGI(TAG, "=== DANE SIECIOWE ESP32-C6 ===");
        ESP_LOGI(TAG, "Adres MAC : %02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Adres IP  : " IPSTR, IP2STR(&event->ip_info.ip));
        ESP_LOGI(TAG, "Maska     : " IPSTR, IP2STR(&event->ip_info.netmask));
        ESP_LOGI(TAG, "Brama GW  : " IPSTR, IP2STR(&event->ip_info.gw));

        s_retry_num = 0;
        if (s_wifi_event_group != NULL)
        {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}
static volatile bool s_sntp_synced = false;
static void time_sync_notification_cb(struct timeval *tv)
{
    s_sntp_synced = true;
}

void vTaskNtpSyncWorker(void *pvParameters)
{
    print_sram_debug("START ZADANIA NTP");
    ESP_LOGI(TAG, "Start zadania synchronizacji NTP na rdzeniu %d", xPortGetCoreID());

    char ssid[64] = {0};
    char pass[64] = {0};

    // 1. Odczyt danych sieciowych z SPIFFS (Twoja funkcja z RTC.CPP)
    if (!load_wifi_credentials_from_SPIFFS(ssid, sizeof(ssid), pass, sizeof(pass)))
    {
        ESP_LOGE(TAG, "Brak pliku /spiffs/prefs.ini lub bledne dane! Przerywam bezpiecznie.");
        s_sync_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    if (!ensure_wifi_stack_initialized())
    {
        s_sync_in_progress = false;
        vTaskDelete(NULL);
        return;
    }

    s_wifi_event_group = xEventGroupCreate();
    s_retry_num = 0;

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {};
    strlcpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (esp_wifi_start() == ESP_OK)
    {
        print_sram_debug("PO ESP_WIFI_START");

        // Czekamy maksymalnie 15 sekund na połączenie lub błąd
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            pdMS_TO_TICKS(15000));

        if (bits & WIFI_CONNECTED_BIT)
        {
            ESP_LOGI(TAG, "Polaczono z Wi-Fi! Pobieram czas z NTP...");
            /********************************* */
            esp_wifi_set_ps(WIFI_PS_NONE);

            const char *ntp_servers[5] = {
                "tempus1.gum.gov.pl",
                "time.google.com",
                "time.cloudflare.com",
                "pool.ntp.org",
                "europe.pool.ntp.org"};

            s_sntp_synced = false;
            sntp_set_time_sync_notification_cb(time_sync_notification_cb);

            for (int attempt = 0; attempt < 5 && !s_sntp_synced; attempt++)
            {
                ESP_LOGI(TAG, "Proba NTP %d/5 -> serwer: %s", attempt + 1, ntp_servers[attempt]);

                if (esp_sntp_enabled())
                {
                    esp_sntp_stop();
                }

                esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
                esp_sntp_setservername(0, ntp_servers[attempt]);
                esp_sntp_init();

                int sntp_wait = 0;
                while (!s_sntp_synced && sntp_wait < 15)
                {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    sntp_wait++;
                }

                if (!s_sntp_synced)
                {
                    ESP_LOGW(TAG, "Brak odpowiedzi z %s (timeout 3s), przelaczam serwer...", ntp_servers[attempt]);
                }
            }

            if (esp_sntp_enabled())
            {
                esp_sntp_stop();
            }
            /****************************************************************** */
            if (s_sntp_synced)
            {

                time_t now = 0;
                struct tm timeinfo = {};
                time(&now);
                localtime_r(&now, &timeinfo);
                // Zapisujemy pobrany czas do fizycznego zegara RX8025T
                rx8025t_time_t rtc_time;
                rtc_time.second = timeinfo.tm_sec;
                rtc_time.minute = timeinfo.tm_min;
                rtc_time.hour = timeinfo.tm_hour;
                rtc_time.day = timeinfo.tm_mday;
                rtc_time.month = timeinfo.tm_mon + 1;
                rtc_time.year = timeinfo.tm_year + 1900;
                rtc_time.weekday = timeinfo.tm_wday;

                if (rx8025t_set_time(&rtc_time) == ESP_OK)
                {
                    sync_internal_rtc_with_rx8025t();
                    ntp_read = true;
                    ESP_LOGI(TAG, "SUKCES NTP! Zapisano do RX8025T: %04d-%02d-%02d %02d:%02d:%02d",
                             rtc_time.year, rtc_time.month, rtc_time.day,
                             rtc_time.hour, rtc_time.minute, rtc_time.second);
                }
                else
                {
                    ESP_LOGE(TAG, "Blad zapisu czasu NTP do RX8025T!");
                }
            }
            else
            {
                ESP_LOGW(TAG, "Timeout serwera NTP (brak odpowiedzi w 10s). Zegarek pracuje dalej na RX8025T.");
            }
        }
        else
        {
            ESP_LOGW(TAG, "Nie udalo sie polaczyc z Wi-Fi w 15s. Zamykam bezpiecznie bez crasha.");
        }
    }

    // Bezpieczne sprzątanie przed zakończeniem zadania:
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);

    esp_wifi_disconnect();
    esp_wifi_stop();

    if (s_wifi_event_group != NULL)
    {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    print_sram_debug("PO ZAMKNIECIU WIFI");
    ESP_LOGI(TAG, "Zadanie NTP zakonczone czysto. Radio Wi-Fi wylaczone.");
    s_sync_in_progress = false;
    vTaskDelete(NULL);
}

void start_ntp_sync_task_async(void)
{
    if (s_sync_in_progress || s_weather_in_progress || is_wifi_portal_active())
    {
        ESP_LOGW(TAG, "Radio Wi-Fi zajete (NTP / Pogoda / Portal) - pomijam NTP.");
        return;
    }

    if (s_sync_in_progress)
    {
        ESP_LOGW(TAG, "Synchronizacja NTP juz trwa - ignoruje kolejne wywolanie.");
        return;
    }
    ntp_read = false;
    s_sync_in_progress = true;
    BaseType_t res = xTaskCreatePinnedToCore(
        vTaskNtpSyncWorker,
        "NtpSyncTask",
        6144,
        NULL,
        3,
        NULL,
        1);

    if (res != pdPASS)
    {
        ESP_LOGE(TAG, "Nie udalo sie utworzyc zadania NtpSyncTask!");
        s_sync_in_progress = false;
    }
}

bool ensure_wifi_stack_initialized(void)
{
    // strcpy(ssid, "BAD_SSID");
    // 2. Jednorazowa inicjalizacja stosu sieciowego (nigdy nie powtarzana przy kolejnych próbach!)
    if (!s_wifi_stack_initialized)
    {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "Blad esp_netif_init: %s", esp_err_to_name(err));
            return false;
        }

        err = esp_event_loop_create_default();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE)
        {
            ESP_LOGE(TAG, "Blad event_loop: %s", esp_err_to_name(err));
            return false;
        }

        esp_netif_create_default_wifi_sta();

        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
        err = esp_wifi_init(&cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Blad esp_wifi_init: %s", esp_err_to_name(err));
            return false;
        }

        s_wifi_stack_initialized = true;
    }
    return true;
}

static char s_weather_rx_buf[1024];
static int s_weather_rx_len = 0;

static void load_weather_coords_from_spiffs(float *lat, float *lon)
{
    *lat = 53.12f; // Domyslny fallback: Bydgoszcz
    *lon = 18.01f;

    FILE *f = fopen("/spiffs/weather.ini", "r");
    if (f == NULL)
    {
        ESP_LOGW(TAG, "Brak /spiffs/weather.ini - uzywam Bydgoszczy (53.12, 18.01)");
        return;
    }

    char line[64];
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (strncmp(line, "lat=", 4) == 0)
        {
            *lat = strtof(line + 4, NULL);
        }
        else if (strncmp(line, "lon=", 4) == 0)
        {
            *lon = strtof(line + 4, NULL);
        }
    }
    fclose(f);
    ESP_LOGI(TAG, "Wspolrzedne pogody: lat=%.4f, lon=%.4f", *lat, *lon);
}

static esp_err_t weather_http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0)
    {
        int space_left = (int)sizeof(s_weather_rx_buf) - 1 - s_weather_rx_len;
        if (space_left > 0)
        {
            int to_copy = (evt->data_len < space_left) ? evt->data_len : space_left;
            memcpy(s_weather_rx_buf + s_weather_rx_len, evt->data, to_copy);
            s_weather_rx_len += to_copy;
            s_weather_rx_buf[s_weather_rx_len] = '\0';
        }
    }
    return ESP_OK;
}
// Tutaj zastąp HOST_API nazwą serwera Open-Meteo (api . open-meteo . com bez spacji)
static const char *WEATHER_HOST = "api.open-meteo.com";

bool fetch_weather_temperature(float *out_temp)
{
    temp_read = false;
    if (out_temp == NULL)
        return false;

    if (s_sync_in_progress || s_weather_in_progress || is_wifi_portal_active())
    {
        ESP_LOGW(TAG, "Radio Wi-Fi zajete - pomijam pobieranie pogody.");
        return false;
    }

    char ssid[33] = {0};
    char pass[65] = {0};
    if (!load_wifi_credentials_from_SPIFFS(ssid, sizeof(ssid), pass, sizeof(pass)))
    {
        ESP_LOGW(TAG, "Brak danych Wi-Fi w SPIFFS dla pogody.");
        return false;
    }

    s_weather_in_progress = true;
    bool ok = false;

    float lat = 53.12f;
    float lon = 18.01f;
    load_weather_coords_from_spiffs(&lat, &lon);

    if (!ensure_wifi_stack_initialized())
    {
        s_weather_in_progress = false;
        return false;
    }

    if (s_wifi_event_group != NULL)
    {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }
    s_wifi_event_group = xEventGroupCreate();
    s_retry_num = 0;

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = (strlen(pass) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE,
        pdFALSE,
        pdMS_TO_TICKS(15000));

    if (bits & WIFI_CONNECTED_BIT)
    {
        char path_buf[160];
        snprintf(path_buf, sizeof(path_buf),
                 "/v1/forecast?latitude=%.4f&longitude=%.4f&current_weather=true",
                 lat, lon);

        s_weather_rx_len = 0;
        s_weather_rx_buf[0] = '\0';

        esp_http_client_config_t cfg = {};
        cfg.host = WEATHER_HOST;
        cfg.path = path_buf;
        cfg.transport_type = HTTP_TRANSPORT_OVER_TCP;
        cfg.timeout_ms = 8000;
        cfg.event_handler = weather_http_event_handler;

        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client != NULL)
        {
            if (esp_http_client_perform(client) == ESP_OK &&
                esp_http_client_get_status_code(client) == 200)
            {
                const char *cw = strstr(s_weather_rx_buf, "\"current_weather\"");
                if (cw != NULL)
                {
                    const char *tp = strstr(cw, "\"temperature\":");
                    if (tp != NULL)
                    {
                        char *end_ptr = NULL;
                        float parsed_val = strtof(tp + 14, &end_ptr);

                        if (end_ptr != (tp + 14) && parsed_val > -60.0f && parsed_val < 65.0f)
                        {
                            *out_temp = parsed_val;
                            temp_read = true;
                            ok = true;
                            ESP_LOGI(TAG, "Pobrano poprawna temperature: %.1f C", *out_temp);
                        }
                    }
                }
            }

            esp_http_client_cleanup(client);
        }
    }

    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id);
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip);
    esp_wifi_disconnect();
    esp_wifi_stop();

    if (s_wifi_event_group != NULL)
    {
        vEventGroupDelete(s_wifi_event_group);
        s_wifi_event_group = NULL;
    }

    s_weather_in_progress = false;
    return ok;
}
