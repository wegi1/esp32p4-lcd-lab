#pragma once

#include <stdbool.h>

// Uruchamia jednorazowe zadanie synchronizacji czasu NTP w tle na rdzeniu 1.
// Nie blokuje ekranu i nie zawiesza procesora, nawet gdy brak sieci Wi-Fi.
void start_ntp_sync_task_async(void);

// Zwraca true, jeśli aktualnie trwa łączenie z Wi-Fi / NTP
bool is_ntp_sync_running(void);
bool ensure_wifi_stack_initialized(void);

// Nowe funkcje dla pogody Open-Meteo:
bool fetch_weather_temperature(float *out_temp);
bool is_weather_fetch_running(void);
