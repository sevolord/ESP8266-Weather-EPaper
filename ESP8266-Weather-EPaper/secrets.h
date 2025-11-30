// ===== Файл с секретными данными (не синхронизируется с Git) =====
// Скопируйте этот файл из secrets.h.example и заполните своими значениями

#ifndef SECRETS_H
#define SECRETS_H

// SSID Wi-Fi сети
#define SECRET_WIFI_SSID "YourWIFI"

// Пароль Wi-Fi сети
#define SECRET_WIFI_PASSWORD "YourPassword"

// API-ключ для OpenWeatherMap
#define SECRET_WEATHER_API_KEY "YourAPIKey"

// Город для запроса погоды (формат: "City,CountryCode", например "Moscow,ru")
#define SECRET_WEATHER_CITY "Moscow,ru"

#endif

