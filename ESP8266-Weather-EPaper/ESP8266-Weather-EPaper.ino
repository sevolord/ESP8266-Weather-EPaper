#include <Arduino.h>                      // Базовые функции и типы данных Arduino
#include <ESP8266WiFi.h>                  // Библиотека для работы с WiFi на ESP8266
#include <time.h>                         // Функции работы со временем (например, для синхронизации по NTP)
#include <GxEPD2_3C.h>                    // Библиотека для работы с трёхцветным e-paper дисплеем (например, SSD1680)
#include <U8g2_for_Adafruit_GFX.h>        // Библиотека для удобной работы с графикой и текстом через Adafruit GFX (в т.ч. содержит русские шрифты)
#include <SPI.h>                          // Библиотека для работы с интерфейсом SPI (используется дисплеем)
#include <ESP8266HTTPClient.h>            // Библиотека для выполнения HTTP-запросов
#include <ArduinoJson.h>                  // Библиотека для парсинга и генерации JSON
#include <ESP8266WebServer.h>             // Библиотека для создания веб-сервера (режим точки доступа для настройки)
#include <EEPROM.h>                       // Библиотека для работы с энергонезависимой памятью (EEPROM)
#include "secrets.h"                      // Файл с секретными данными (SSID, пароль, API-ключ, город)

// Примечание: Код оптимизирован для ESP8266, 
// но для больших JSON-ответов (>16KB) рекомендуется использовать ESP32 из-за большего объёма SRAM.

// ===== Константы =====
#define DEBUG_ENABLED false          // Флаг включения отладочного вывода через Serial (true – вывод включён)
#define EEPROM_SIZE 256              // Размер EEPROM (в байтах) для хранения настроек
#define SETTINGS_MAGIC 0xDEADBEEF    // Магическая константа для проверки корректности данных в EEPROM
#define NTP_TIMEOUT_MS 30000         // Тайм-аут (в мс) для запроса времени по NTP
#define RETRY_DELAY_MS 10000         // Задержка (в мс) между повторными попытками запросов (например, NTP или API)
#define MIN_HEAP_FOR_JSON 15000      // Минимальный размер свободной памяти (в байтах) для корректной обработки JSON
#define HTTP_PREFIX "http://"        // Префикс для формирования HTTP-запросов

// ===== Дефолтные значения настроек =====
// Значения берутся из файла secrets.h (не синхронизируется с Git)
const String DEFAULT_SSID = SECRET_WIFI_SSID;           // Дефолтный SSID Wi-Fi сети
const String DEFAULT_PASS = SECRET_WIFI_PASSWORD;       // Дефолтный пароль Wi-Fi
const String DEFAULT_API_KEY = SECRET_WEATHER_API_KEY;  // Дефолтный API-ключ для OpenWeatherMap
const String DEFAULT_CITY = SECRET_WEATHER_CITY;        // Дефолтный город для запроса погоды

// ===== Структура настроек для EEPROM =====
struct Settings {
  char ssid[32];         // SSID Wi-Fi (максимум 31 символ + терминатор '\0')
  char password[64];     // Пароль Wi-Fi (максимум 63 символа + терминатор '\0')
  char apiKey[64];       // API-ключ OpenWeatherMap (максимум 63 символа + терминатор '\0')
  char city[32];         // Город для запроса погоды (максимум 31 символ + терминатор '\0')
  uint32_t magic;        // Магическая переменная для проверки корректности сохранённых настроек
};

// ===== Глобальные переменные =====
Settings settings;              // Объект для хранения настроек, загружаемых/сохраняемых в EEPROM
bool debug = DEBUG_ENABLED;     // Флаг отладки: если true – сообщения выводятся в Serial

// Текущие значения настроек, которые могут быть изменены через веб-интерфейс или сохранены в EEPROM
String WIFI_SSID = DEFAULT_SSID;
String WIFI_PASS = DEFAULT_PASS;
String API_KEY = DEFAULT_API_KEY;
String city = DEFAULT_CITY;     // Город для запроса погоды
String units = "metric";      // Единицы измерения: "metric" – метрическая система (°C)

// ===== Настройка дисплея 2.9" SSD1680 (3-цветного) =====
// Инициализация объекта дисплея с указанием пинов: CS=15, DC=0, RST=2, BUSY=4
GxEPD2_3C<GxEPD2_290_C90c, GxEPD2_290_C90c::HEIGHT> display(
  GxEPD2_290_C90c(/*CS=*/15, /*DC=*/0, /*RST=*/2, /*BUSY=*/4));

// ===== U8g2 для работы с дисплеем =====
U8G2_FOR_ADAFRUIT_GFX u8g2;  // Объект для работы с текстом и графикой на дисплее через Adafruit GFX

// ===== Дни недели и месяцы =====
// Массивы строк для отображения дней недели и названий месяцев (на русском языке)
const char* wdayName[7] = { "Воскресенье", "Понедельник", "Вторник", "Среда", "Четверг", "Пятница", "Суббота" };    
const char* monthName[12] = { "января", "февраля", "марта", "апреля", "мая", "июня", "июля", "августа", "сентября", "октября", "ноября", "декабря" };

// ===== Данные погоды =====
// Переменные для хранения текущих данных о погоде
char weatherDesc[32] = "";              // Описание текущей погоды (например, "ясно", "облачно")
float weatherTemp = 0.0;                // Текущая температура (в °C)
float windSpeed = 0.0;                  // Скорость ветра (в м/с)
float feelsLike = 0.0;                  // Значение "ощущается как" (в °C)
time_t sunrise = 0;                     // Время рассвета (Unix timestamp)
time_t sunset = 0;                      // Время заката (Unix timestamp)
int pressure = 0;                       // Атмосферное давление (в гПа/мбар)

// Переменная для хранения времени последнего обновления данных
unsigned long lastUpdateTime = 0;

// ===== Веб-сервер и режим точки доступа =====
ESP8266WebServer server(80);            // Создание веб-сервера, работающего на порту 80
volatile bool configReceived = false;   // Флаг, сигнализирующий о получении настроек через веб-интерфейс в режиме AP

// ===== Функция для логирования (отладочный вывод) =====
void debugPrint(const String& msg) {
  if (debug) Serial.println(msg);       // Если режим отладки включён, выводим сообщение в Serial
}

// ===== Инициализация настроек дисплея =====
void initDisplay()
{
  display.init(115200, true, 50, false); // Инициализация дисплея с указанной скоростью передачи и параметрами
  display.setRotation(1);               // Установка ориентации дисплея (поворот на 90°) альбомная
  display.setFullWindow();              // Режим полного обновления экрана
  u8g2.begin(display);                  // Инициализация U8g2 для работы с дисплеем
  u8g2.setFont(u8g2_font_10x20_t_cyrillic); // Установка шрифта с поддержкой кириллицы
  u8g2.setBackgroundColor(GxEPD_WHITE); // Установка белого фона для отрисовки
}

// ===== Сохранение настроек в EEPROM =====
void saveSettings() {
  settings.magic = SETTINGS_MAGIC;      // Записываем магическую константу для проверки валидности настроек
  // Копируем строки настроек в структуру с учетом размера буфера
  strncpy(settings.ssid, WIFI_SSID.c_str(), sizeof(settings.ssid) - 1);
  settings.ssid[sizeof(settings.ssid) - 1] = '\0'; // Гарантируем корректное завершение строки
  strncpy(settings.password, WIFI_PASS.c_str(), sizeof(settings.password) - 1);
  settings.password[sizeof(settings.password) - 1] = '\0';
  strncpy(settings.apiKey, API_KEY.c_str(), sizeof(settings.apiKey) - 1);
  settings.apiKey[sizeof(settings.apiKey) - 1] = '\0';
  strncpy(settings.city, city.c_str(), sizeof(settings.city) - 1);
  settings.city[sizeof(settings.city) - 1] = '\0';
  EEPROM.put(0, settings);              // Сохраняем всю структуру настроек в EEPROM, начиная с адреса 0
  EEPROM.commit();                      // Фиксируем изменения в EEPROM
  debugPrint("Настройки сохранены в EEPROM.");
}

// ===== Загрузка настроек из EEPROM =====
void loadSettings() {
  EEPROM.begin(EEPROM_SIZE);            // Инициализация работы с EEPROM заданного размера
  EEPROM.get(0, settings);              // Чтение сохранённых настроек из EEPROM
  // Проверка: если магическая константа совпадает, настройки валидны
  if (settings.magic == SETTINGS_MAGIC) {
    WIFI_SSID = String(settings.ssid);  // Загружаем SSID из EEPROM
    WIFI_PASS = String(settings.password); // Загружаем пароль
    API_KEY = String(settings.apiKey);   // Загружаем API-ключ
    city = String(settings.city);  // Загружаем город из EEPROM

    debugPrint("Загружены настройки из EEPROM:");
    debugPrint("SSID: " + WIFI_SSID);
    debugPrint("Password: " + WIFI_PASS);
    debugPrint("API Key: " + API_KEY);
    debugPrint("City: " + city);
  } else {
    // Если настройки отсутствуют или повреждены, используем дефолтные значения и сохраняем их
    debugPrint("В EEPROM нет валидных настроек. Используем дефолтные.");
    WIFI_SSID = DEFAULT_SSID;
    WIFI_PASS = DEFAULT_PASS;
    API_KEY = DEFAULT_API_KEY;
    city = DEFAULT_CITY;
    saveSettings();
  }
}

// ===== Отображение ошибки на дисплее =====
void drawError(const String& errMsg) {
  display.firstPage();                  // Начинаем обновление экрана
  do {
    display.fillScreen(GxEPD_WHITE);     // Заливаем экран белым цветом
    u8g2.begin(display);                 // Инициализируем u8g2 для работы с дисплеем
    u8g2.setFont(u8g2_font_10x20_t_cyrillic); // Устанавливаем шрифт для вывода сообщения
    u8g2.setCursor(5, 20);               // Устанавливаем начальную позицию курсора
    u8g2.setForegroundColor(GxEPD_RED);  // Устанавливаем красный цвет для выделения ошибки
    u8g2.print("Ошибка:");              // Выводим заголовок "Ошибка:"
    u8g2.setCursor(5, 40);               // Сдвигаем курсор для вывода сообщения ошибки
    u8g2.print(errMsg);                  // Выводим текст ошибки
  } while (display.nextPage());         // Завершаем обновление экрана
}

// ===== Отображение инструкций в режиме AP =====
void displayAPInstructions() {
  do {
    display.fillScreen(GxEPD_WHITE);     // Очищаем экран, заливая белым цветом
    u8g2.setCursor(5, 20);               // Устанавливаем позицию курсора
    u8g2.setForegroundColor(GxEPD_RED);  // Красный цвет для выделения ошибки подключения
    u8g2.print("Ошибка подключения");
    u8g2.setCursor(5, 40);
    u8g2.setForegroundColor(GxEPD_RED);
    u8g2.print("к WiFi!");               // Сообщаем об ошибке подключения к WiFi
    u8g2.setCursor(5, 60);
    u8g2.setForegroundColor(GxEPD_BLACK); // Черный цвет для инструкций
    u8g2.print("Подключитесь к сети:");
    u8g2.setCursor(5, 80);
    u8g2.setForegroundColor(GxEPD_RED);
    u8g2.print("ESP_Config");           // Имя создаваемой точки доступа
    u8g2.setCursor(5, 100);
    u8g2.setForegroundColor(GxEPD_BLACK);
    u8g2.print("Откройте:");
    u8g2.setCursor(5, 120);
    u8g2.setForegroundColor(GxEPD_RED);
    u8g2.print("http://192.168.4.1");   // Адрес для доступа к веб-интерфейсу настройки
  } while (display.nextPage());         // Повторяем вывод, пока обновление экрана не завершится
}

// ===== Обработчик корневой страницы веб-сервера =====
void handleRoot() {
  // Формирование HTML-страницы для настройки WiFi и API
  String html = "<html><head><meta charset='UTF-8'><title>Настройка устройства</title>";
  // Добавляем стили для улучшения внешнего вида страницы
  html += "<style>body{font-family:Arial;margin:20px;}h1{color:#333;}input{margin:5px;padding:5px;width:200px;}</style>";
  html += "</head><body>";
  html += "<h1>Настройка WiFi, API и города</h1>";
  html += "<form method='POST' action='/save'>";
  html += "WiFi SSID: <input type='text' name='ssid' maxlength='31' value='" + WIFI_SSID + "' required><br>";
  html += "WiFi Password: <input type='password' name='password' maxlength='63' value='" + WIFI_PASS + "' required><br>";
  html += "API Key: <input type='text' name='apikey' maxlength='63' value='" + API_KEY + "' placeholder='Оставьте пустым для дефолтного'><br>";
  html += "Город: <input type='text' name='city' maxlength='31' value='" + city + "' required><br>";
  html += "<input type='submit' value='Сохранить' style='padding:10px;'>";
  html += "</form></body></html>";
  // Отправляем сформированную HTML-страницу с кодом 200 (OK)
  server.send(200, "text/html", html);
}

// ===== Обработчик сохранения настроек =====
void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("password") && server.hasArg("apikey") && server.hasArg("city")) {
    String newSSID = server.arg("ssid");
    String newPass = server.arg("password");
    String newAPIKey = server.arg("apikey");
    String newCity = server.arg("city");

    // Валидация: проверяем, что длина введённых данных соответствует ограничениям
    if (newSSID.length() > 0 && newSSID.length() <= 31 &&
        newPass.length() > 0 && newPass.length() <= 63 &&
        newAPIKey.length() <= 63 &&
        newCity.length() > 0 && newCity.length() <= 31) {
      WIFI_SSID = newSSID;
      WIFI_PASS = newPass;
      API_KEY = (newAPIKey.length() > 0) ? newAPIKey : DEFAULT_API_KEY;
      city = newCity;

      debugPrint("Получены новые настройки:");
      debugPrint("SSID: " + WIFI_SSID);
      debugPrint("Password: " + WIFI_PASS);
      debugPrint("API Key: " + API_KEY);
      debugPrint("City: " + city);
      saveSettings();                  // Сохраняем обновлённые настройки в EEPROM
      // Отправляем клиенту сообщение об успешном сохранении и перезагрузке
      server.send(200, "text/html", "<html><body><h1>Настройки сохранены. Перезагрузка...</h1><script>setTimeout(() => location.href='/', 2000);</script></body></html>");
      configReceived = true;           // Устанавливаем флаг, что настройки получены
    } else {
      // Если длина одного из полей превышает допустимую, отправляем ошибку
      server.send(400, "text/html", "<html><body><h1>Ошибка: длина данных превышает допустимую</h1></body></html>");
    }
  } else {
    // Если отсутствуют обязательные поля, отправляем сообщение об ошибке
    server.send(400, "text/html", "<html><body><h1>Ошибка: все поля обязательны (кроме API Key)</h1></body></html>");
  }
}

// ===== Подключение к WiFi  =====
bool connectToWiFi() {
  WiFi.mode(WIFI_STA);                // Устанавливаем ESP8266 в режим станции
  WiFi.begin(WIFI_SSID.c_str(), WIFI_PASS.c_str()); // Начинаем подключение к WiFi с использованием заданных настроек
  unsigned long startAttemptTime = millis(); // Запоминаем время начала попытки подключения
  debugPrint("Подключение к WiFi...");
  // Цикл ожидания подключения (до 30 секунд)
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 30000) {
    delay(500);                       // Задержка 500 мс между проверками
    debugPrint(".");                  // Выводим точку для индикации процесса подключения
  }
  // Проверяем, удалось ли подключиться к WiFi
  if (WiFi.status() == WL_CONNECTED) {
    debugPrint("\nWiFi подключен, IP: " + WiFi.localIP().toString());
    return true;
  } else {
    debugPrint("\nНе удалось подключиться к WiFi.");
    return false;
  }
}

// ===== Запуск режима точки доступа и веб-сервера =====
void startAPMode() {
  debugPrint("Запуск режима точки доступа...");
  WiFi.mode(WIFI_AP);                 // Переключаем ESP8266 в режим точки доступа (AP)
  WiFi.softAP("ESP_Config");          // Создаем точку доступа с именем "ESP_Config"
  IPAddress apIP = WiFi.softAPIP();    // Получаем IP-адрес созданной точки доступа
  debugPrint("AP Mode IP: " + apIP.toString());

  displayAPInstructions();           // Отображаем инструкции для подключения к точке доступа на дисплее

  // Назначаем обработчики для веб-сервера
  server.on("/", handleRoot);         // Обработчик корневой страницы (GET-запрос)
  server.on("/save", HTTP_POST, handleSave); // Обработчик для сохранения настроек (POST-запрос)
  server.begin();                     // Запускаем веб-сервер
  debugPrint("HTTP сервер запущен");

  // Ожидаем, пока пользователь не введёт настройки через веб-интерфейс
  while (!configReceived) {
    server.handleClient();            // Обрабатываем входящие запросы
    delay(10);                        // Небольшая задержка для стабильности работы
  }

  server.stop();                      // Останавливаем веб-сервер после получения настроек
  debugPrint("Настройки получены, выходим из режима AP");
}

// ===== Синхронизация времени через NTP  =====
bool getNTPtime() {
  const int maxAttempts = 5;          // Максимальное количество попыток получения времени
  // Массив адресов NTP-серверов для резервирования
  const char* ntpServers[] = { "pool.ntp.org", "time.nist.gov", "time.google.com", "time.windows.com" };
  int attempts = 0;
  // Повторяем попытки, пока не достигнем maxAttempts
  while (attempts < maxAttempts) {
    for (int i = 0; i < 4; i++) {
      // Устанавливаем временную зону (смещение 5 часов) и NTP-сервер
      configTime(5 * 3600, 0, ntpServers[i]);
      debugPrint("Попытка NTP с сервера: " + String(ntpServers[i]));
      struct tm timeinfo;
      // Функция getLocalTime пытается получить локальное время в течение заданного тайм-аута
      if (getLocalTime(&timeinfo, NTP_TIMEOUT_MS)) {
        debugPrint("Время успешно получено с " + String(ntpServers[i]));
        return true;                // Если время получено, завершаем функцию
      } else {
        debugPrint("Не удалось получить время с " + String(ntpServers[i]));
      }
    }
    attempts++;                     // Увеличиваем счетчик попыток
    debugPrint("Повтор попытки NTP через " + String(RETRY_DELAY_MS / 1000) + " секунд. Попытка: " + String(attempts));
    delay(RETRY_DELAY_MS);          // Задержка перед следующей попыткой
  }
  drawError("Ошибка NTP");          // Если время не получено, выводим сообщение об ошибке
  return false;
}

// ===== Запрос текущей погоды  =====
bool fetchCurrentWeather() {
  const int maxAttempts = 5;          // Максимальное количество попыток запроса
  // Массив адресов серверов для запроса текущей погоды
  const char* weatherHosts[] = { "api.openweathermap.org", "eu-api.openweathermap.org" };
  int attempts = 0;
  while (attempts < maxAttempts) {
    for (int i = 0; i < 2; i++) {
      String host = weatherHosts[i];
      WiFiClient client;            // Создаем клиент для работы с сетью
      HTTPClient http;              // Объект для выполнения HTTP-запроса
      // Формируем URL запроса текущей погоды с параметрами города, единиц измерения, API-ключа и языка (русский)
      String url = HTTP_PREFIX + host + "/data/2.5/weather?q=" + city + "&units=" + units + "&appid=" + API_KEY + "&lang=ru";
      debugPrint("Запрос текущей погоды: " + url);
      if (!http.begin(client, url)) {
        debugPrint("Ошибка инициализации HTTP с " + host);
        continue;
      }
      int httpCode = http.GET();  // Отправляем GET-запрос
      if (httpCode != HTTP_CODE_OK) {
        debugPrint("Ошибка HTTP: " + String(httpCode) + " с " + host);
        http.end();
        continue;
      }
      String payload = http.getString();  // Получаем ответ в виде строки
      debugPrint("Получен ответ: " + payload.substring(0, 100) + "...");
      
      // Проверяем, достаточно ли памяти для обработки JSON
      if (ESP.getFreeHeap() < MIN_HEAP_FOR_JSON) {
        debugPrint("Недостаточно памяти для обработки JSON!");
        http.end();
        continue;
      }
      // Парсинг JSON-ответа
      StaticJsonDocument<2048> doc;
      DeserializationError error = deserializeJson(doc, payload);
      if (error) {
        debugPrint("Ошибка парсинга JSON: " + String(error.c_str()));
        http.end();
        continue;
      }
      // Извлекаем необходимые данные о погоде из JSON
      strncpy(weatherDesc, (doc["weather"][0]["description"] | "Без описания"), sizeof(weatherDesc) - 1);
      weatherDesc[sizeof(weatherDesc) - 1] = '\0';
      weatherTemp = doc["main"]["temp"] | 0.0;
      windSpeed = doc["wind"]["speed"] | 0.0;
      feelsLike = doc["main"]["feels_like"] | 0.0;
      sunrise = (time_t)(doc["sys"]["sunrise"] | 0);
      sunset = (time_t)(doc["sys"]["sunset"] | 0);
      pressure = doc["main"]["pressure"] | 0;
      debugPrint("Текущая погода: " + String(weatherDesc) + ", " + String(weatherTemp) + ", ветер: " + String(windSpeed));
      http.end();
      return true;               // Данные успешно получены
    }
    attempts++;
    debugPrint("Повтор запроса текущей погоды через " + String(RETRY_DELAY_MS / 1000) + " секунд. Попытка: " + String(attempts));
    delay(RETRY_DELAY_MS);         // Задержка перед повторной попыткой
  }
  drawError("Ошибка погоды");      // Если данные не получены, выводим сообщение об ошибке
  return false;
}

// ===== Функция для определения характеристики давления =====
const char* getPressureDescription(int pressure) {
  if (pressure < 1000) {
    return "низкое";
  } else if (pressure < 1025) {
    return "нормальное";
  } else if (pressure < 1035) {
    return "повышенное";
  } else {
    return "высокое";
  }
}

// ===== Отрисовка содержимого на дисплее =====
void drawAllContent() {
  time_t now = time(nullptr);            // Получаем текущее время
  struct tm* t = localtime(&now);          // Преобразуем время в структуру tm
  bool hasTime = (t->tm_year + 1900 >= 2022); // Проверяем, синхронизировано ли время (год >= 2022)

  String dateStr, weekdayStr;
  if (hasTime) {
    int year = t->tm_year + 1900;
    int month = t->tm_mon;
    int day = t->tm_mday;
    // Формируем строку с датой (например, "15 марта 2025")
    dateStr = String(day) + " " + String(monthName[month]) + " " + String(year);
    // Получаем название дня недели
    weekdayStr = String(wdayName[t->tm_wday]);
  } else {
    dateStr = "Время не sync!"; // Сообщение, если время не синхронизировано
    weekdayStr = "";
  }

  display.firstPage();             // Начинаем обновление экрана
  do {
    display.fillScreen(GxEPD_WHITE); // Очищаем экран, заливая белым цветом

    // Строка 1: Отображение даты и дня недели
    u8g2.setCursor(5, 20);
    u8g2.setForegroundColor(GxEPD_BLACK);
    u8g2.print(dateStr + ", ");
    u8g2.setForegroundColor(GxEPD_RED);
    u8g2.print(weekdayStr);

    // Строка 2: Текущая погода (описание, температура)
    u8g2.setCursor(5, 40);
    u8g2.setForegroundColor(GxEPD_BLACK);
    if (strlen(weatherDesc) == 0 && weatherTemp == 0.0) {
      u8g2.print("Погода не получена!");
    } else {
      u8g2.print(weatherDesc);
      u8g2.print(", ");
      u8g2.setForegroundColor(GxEPD_RED);
      u8g2.print(String(weatherTemp));
    }

    // Строка 3: Скорость ветра
    u8g2.setCursor(5, 60);
    u8g2.setForegroundColor(GxEPD_BLACK);
    u8g2.print("Ветер: ");
    u8g2.setForegroundColor(GxEPD_RED);
    u8g2.print(String(windSpeed));

    // Строка 4: Значение "Ощущается как"
    u8g2.setCursor(5, 80);
    u8g2.setForegroundColor(GxEPD_BLACK);
    u8g2.print("Ощущается как: ");
    u8g2.setForegroundColor(GxEPD_RED);
    u8g2.print(String(feelsLike));

    // Строка 5: Рассвет и закат
    u8g2.setCursor(5, 100);
    u8g2.setForegroundColor(GxEPD_BLACK);
    u8g2.print("Рассвет: ");
    if (sunrise > 0) {
      struct tm* sunriseTm = localtime(&sunrise);
      char sunriseStr[6];
      sprintf(sunriseStr, "%02d:%02d", sunriseTm->tm_hour, sunriseTm->tm_min);
      u8g2.setForegroundColor(GxEPD_RED);
      u8g2.print(sunriseStr);
    } else {
      u8g2.setForegroundColor(GxEPD_RED);
      u8g2.print("--:--");
    }
    u8g2.setForegroundColor(GxEPD_BLACK);
    u8g2.print(", Закат: ");
    if (sunset > 0) {
      struct tm* sunsetTm = localtime(&sunset);
      char sunsetStr[6];
      sprintf(sunsetStr, "%02d:%02d", sunsetTm->tm_hour, sunsetTm->tm_min);
      u8g2.setForegroundColor(GxEPD_RED);
      u8g2.print(sunsetStr);
    } else {
      u8g2.setForegroundColor(GxEPD_RED);
      u8g2.print("--:--");
    }

    // Строка 6: Давление
    u8g2.setCursor(5, 120);
    u8g2.setForegroundColor(GxEPD_BLACK);
    u8g2.print("Давление: ");
    if (pressure > 0) {
      u8g2.setForegroundColor(GxEPD_RED);
      u8g2.print(String(pressure));
      u8g2.setForegroundColor(GxEPD_BLACK);
      u8g2.print(" (");
      u8g2.print(getPressureDescription(pressure));
      u8g2.print(")");
    } else {
      u8g2.setForegroundColor(GxEPD_RED);
      u8g2.print("--");
    }

  } while (display.nextPage());    // Завершаем обновление экрана
}

// ===== Инициализация устройства =====
void setup() {
  Serial.begin(115200);             // Инициализируем последовательный порт для отладки
  debugPrint("Запуск...");
  initDisplay();                    // Инициализируем дисплей
  
  loadSettings();                   // Загружаем сохранённые настройки из EEPROM

  // Пытаемся подключиться к WiFi в режиме станции (STA)
  if (!connectToWiFi()) {
    // Если не удалось подключиться, запускаем режим точки доступа (AP) для настройки
    startAPMode();
    // После получения настроек повторяем попытку подключения к WiFi
    if (!connectToWiFi()) {
      drawError("Не удалось подключиться к WiFi"); // Если повторная попытка неуспешна, выводим ошибку
      return;
    }
  }

  // Синхронизируем время через NTP, запрашиваем текущую погоду, затем отображаем данные на дисплее
  getNTPtime();
  fetchCurrentWeather();

  drawAllContent();                 // Отрисовываем полученные данные на дисплее
  lastUpdateTime = millis();        // Запоминаем время последнего обновления данных
}

// ===== Основной цикл =====
void loop() {
  // Каждые 3600000 мс (1 час) обновляем данные (NTP, погоду и отображение)
  if (millis() - lastUpdateTime >= 3600000UL) {
    debugPrint("Обновление данных...");
    getNTPtime();
    fetchCurrentWeather();
    drawAllContent();
    lastUpdateTime = millis();
  }
  delay(1000);                    // Задержка 1 секунда между итерациями основного цикла
}