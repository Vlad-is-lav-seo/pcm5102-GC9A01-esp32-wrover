#include <Arduino.h>
#include <WiFi.h>
#include "Audio.h"
#include <TFT_eSPI.h>
#include <Preferences.h>
#include <Wire.h>
#include "RTClib.h"

// ==== WiFi ====
const char* ssid = "Co-vid";
const char* password = "zadolbala";

// ==== I2S Pins ====
#define I2S_BCLK 26
#define I2S_LRC  25
#define I2S_DOUT 22

// ==== Buttons ====
#define VOL_UP_PIN     32
#define MUTE_PIN       33
#define VOL_DOWN_PIN   27
#define BUTTON_4_PIN   14  // Новая кнопка 4
#define BUTTON_5_PIN   13  // Новая кнопка 5

// ==== I2C Pins for DS1307 RTC ====
#define I2C_SDA        21  // Свободный пин для SDA
#define I2C_SCL        19  // Свободный пин для SCL

TFT_eSPI tft = TFT_eSPI();
Audio audio;
Preferences preferences;
RTC_DS1307 rtc;  // RTC объект

// ==== System Mode ====
enum SystemMode {
  MODE_RADIO,
  MODE_BLUETOOTH,
  MODE_CLOCK,
  MODE_AP,
  MODE_TEST
};
SystemMode currentMode = MODE_RADIO;

// ==== Clock Display Modes ====
enum ClockDisplayMode {
  CLOCK_NORMAL,      // Текущий режим (время + день недели)
  CLOCK_LARGE_DIGITAL, // Крупные цифровые часы
  CLOCK_ANALOG       // Аналоговые стрелочные часы
};
ClockDisplayMode currentClockMode = CLOCK_NORMAL;
ClockDisplayMode lastClockMode = CLOCK_NORMAL;

// ==== Menu System ====
bool menuVisible = false;
int currentMenuSelection = 0;
const char* menuItems[] = {
  "RADIO",
  "BLUETOOTH", 
  "CLOCK",
  "AP MODE",
  "TEST",
  "EXIT"
};
const int menuSize = 6;

// ==== Globals ====
int volume = 4;
bool muted = false;
int lastVolume = -1;
bool lastMute = false;
bool lastMenuVisible = false;
SystemMode lastMode = MODE_RADIO;
String lastTrack = "";
String lastStation = "";
bool wifiConnected = false;
bool modeChangeRequested = false;
SystemMode requestedMode = MODE_RADIO;
bool rtcAvailable = false;  // Флаг наличия RTC модуля

// ==== Track info ====
String currentStation = "RMF FM";
String currentTrack = "";

// ==== TEST Mode Variables ====
String lastWiFiSSID = "";
String lastIPAddress = "";
String lastNetworkTime = "";

// ==== CLOCK Mode Variables ====
String lastRTCTime = "";
String lastRTCDay = "";
String lastRTCDate = "";
unsigned long lastClockUpdate = 0;

// ==== Analog Clock Variables ====
#define CLOCK_CENTER_X 120
#define CLOCK_CENTER_Y 120
#define CLOCK_RADIUS 110  // Увеличен радиус до краев экрана
#define DOT_RADIUS 2
#define TRACK_RADIUS 118  // Увеличено соответственно
int16_t last_hx = 0, last_hy = 0, last_mx = 0, last_my = 0, last_sx = 0, last_sy = 0;

// ==== Tasks ====
TaskHandle_t audioTaskHandle;
TaskHandle_t uiTaskHandle;

// ==== Stream ====
const char* streamURL = "http://31.192.216.10:8000/rmf_fm";

// ==== NTP for TEST mode ====
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 10800; // UTC+3
const int   daylightOffset_sec = 0;

// ==== УНИФИЦИРОВАННЫЕ КООРДИНАТЫ СТРОК ====
#define LINE1_Y 40    // Иконка
#define LINE2_Y 90    // Основная информация (крупный шрифт, зеленый)
#define LINE3_Y 120   // Дополнительная информация (обычный шрифт, белый) 
#define LINE4_Y 180   // Статус/громкость (внизу)

// ==== Глобальные переменные для оптимизации ====
String lastLine2 = "";
String lastLine3 = ""; 
String lastLine4 = "";

// ==== Function Prototypes ====
void drawUnifiedScreen(const char* line2, const char* line3, const char* line4, uint16_t line4Color = TFT_YELLOW, bool showIcon = true, bool forceUpdate = false);
void updateLine4Only(const char* line4, uint16_t color = TFT_YELLOW);
void drawCurrentModeScreen(bool forceUpdate = false);
void updateTrackInfo(bool forceUpdate = false);
void updateVolumeDisplay(bool forceUpdate = false);
void setSystemMode(SystemMode newMode);
void showSplashScreen(String modeName = "ESP32 RADIO");
void showModeChangeScreen(SystemMode newMode);
void drawMenu();
void toggleMenu();
void navigateMenu(int direction);
void selectMenuItem();
void drawWiFiIcon(int x, int y, int bars, bool isAPMode = false);
void drawBluetoothIcon(int x, int y);
void drawClockIcon(int x, int y);
void drawAPModeIcon(int x, int y);
void drawTestIcon(int x, int y);
int rssiToBars(int rssi);
uint16_t interpolateColor(uint16_t color1, uint16_t color2, float t);
void saveCurrentMode();
SystemMode loadCurrentMode();
String getModeName(SystemMode mode);
int getCurrentModeIndex();
bool connectToWiFi();
void disconnectFromWiFi();
void stopRadio();
void performModeChange();
void initializeAudioForCurrentMode();
void initializeTestMode();
String getNetworkTime();
String getIPAddress();
String getWiFiSSID();
void updateTestInfo(bool forceUpdate = false);
bool initializeRTC();
String getRTCTime();
String getRTCDate();
String getRTCDayOfWeek();
bool syncRTCFromNTP();
void updateClockDisplay(bool forceUpdate = false);
void drawNormalClock(bool forceUpdate);
void drawLargeDigitalClock(bool forceUpdate);
void drawAnalogClock(bool forceUpdate);
void drawClockFace();
void updateAnalogClock();
void drawDotAtSecond(uint8_t sec, uint16_t color);

// WiFi event handler
void wifi_event_handler(WiFiEvent_t event) {
  switch(event) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      Serial.println("WiFi Connected");
      wifiConnected = true;
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.println("WiFi Disconnected");
      wifiConnected = false;
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.println("WiFi Got IP: " + WiFi.localIP().toString());
      wifiConnected = true;
      break;
    default:
      break;
  }
  
  // Notify UI task to redraw RSSI
  if (uiTaskHandle != NULL) {
    xTaskNotify(uiTaskHandle, 0, eNoAction);
  }
}

// ---------------------------------------------------------------------
//                    ОПТИМИЗИРОВАННАЯ ФУНКЦИЯ ОТРИСОВКИ
// ---------------------------------------------------------------------
void drawUnifiedScreen(const char* line2, const char* line3, const char* line4, 
                      uint16_t line4Color, bool showIcon, bool forceUpdate) {
  
  bool needsUpdate = forceUpdate;
  
  // Проверяем изменения для каждой строки отдельно
  if (line2 != NULL && String(line2) != lastLine2) {
    needsUpdate = true;
    lastLine2 = String(line2);
  }
  if (line3 != NULL && String(line3) != lastLine3) {
    needsUpdate = true; 
    lastLine3 = String(line3);
  }
  if (line4 != NULL && String(line4) != lastLine4) {
    needsUpdate = true;
    lastLine4 = String(line4);
  }
  
  if (!needsUpdate) return;
  
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  // Строка 1: Иконка (если нужно)
  if (showIcon) {
    switch(currentMode) {
      case MODE_RADIO:
        if (wifiConnected) {
          int rssi = WiFi.RSSI();
          int bars = rssiToBars(rssi);
          drawWiFiIcon(120, LINE1_Y, bars, false);
        } else {
          drawWiFiIcon(120, LINE1_Y, 0, false);
        }
        break;
      case MODE_BLUETOOTH: drawBluetoothIcon(120, LINE1_Y); break;
      case MODE_CLOCK: drawClockIcon(120, LINE1_Y); break;
      case MODE_AP: drawAPModeIcon(120, LINE1_Y); break;
      case MODE_TEST: drawTestIcon(120, LINE1_Y); break;
    }
  }

  // Строка 2: Основная информация (зеленая, крупный шрифт)
  if (line2 != NULL) {
    tft.setTextColor(TFT_GREEN);
    tft.setTextSize(2);
    tft.drawString(line2, 120, LINE2_Y);
  }

  // Строка 3: Дополнительная информация (белая, обычный шрифт)
  if (line3 != NULL) {
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.drawString(line3, 120, LINE3_Y);
  }

  // Строка 4: Статус/громкость (цвет зависит от контекста)
  if (line4 != NULL) {
    tft.setTextColor(line4Color);
    tft.setTextSize(2);
    tft.drawString(line4, 120, LINE4_Y);
  }
}

// ---------------------------------------------------------------------
//                    ФУНКЦИЯ ЧАСТИЧНОГО ОБНОВЛЕНИЯ
// ---------------------------------------------------------------------
void updateLine4Only(const char* line4, uint16_t color) {
  if (String(line4) != lastLine4) {
    tft.fillRect(0, LINE4_Y - 10, 240, 20, TFT_BLACK);
    tft.setTextColor(color);
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(line4, 120, LINE4_Y);
    lastLine4 = String(line4);
  }
}

// ---------------------------------------------------------------------
//                           RTC INITIALIZATION
// ---------------------------------------------------------------------
bool initializeRTC() {
  Serial.println("Initializing RTC DS1307...");
  
  // Инициализируем I2C с правильными пинами
  Wire.begin(I2C_SDA, I2C_SCL);
  delay(100);
  
  if (!rtc.begin()) {
    Serial.println("Couldn't find RTC DS1307");
    rtcAvailable = false;
    return false;
  }
  
  if (!rtc.isrunning()) {
    Serial.println("RTC is NOT running, setting the time!");
    // Устанавливаем время компиляции если RTC не работает
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  
  Serial.println("RTC DS1307 initialized successfully");
  rtcAvailable = true;
  return true;
}

String getRTCTime() {
  if (!rtcAvailable) {
    return "00:00:00";
  }
  
  DateTime now = rtc.now();
  char timeString[9];
  sprintf(timeString, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
  return String(timeString);
}

String getRTCDate() {
  if (!rtcAvailable) {
    return "DD-MM-YYYY";
  }
  
  DateTime now = rtc.now();
  char dateString[11];
  sprintf(dateString, "%02d-%02d-%04d", now.day(), now.month(), now.year());
  return String(dateString);
}

String getRTCDayOfWeek() {
  if (!rtcAvailable) {
    return "RTC ERROR";
  }
  
  DateTime now = rtc.now();
  const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
  return String(days[now.dayOfTheWeek()]);
}

bool syncRTCFromNTP() {
  if (!rtcAvailable || !wifiConnected) {
    return false;
  }
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain NTP time");
    return false;
  }
  
  DateTime ntpTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                   timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  
  rtc.adjust(ntpTime);
  Serial.println("RTC synced with NTP");
  return true;
}

// ---------------------------------------------------------------------
//                    ОПТИМИЗИРОВАННЫЕ CLOCK ФУНКЦИИ
// ---------------------------------------------------------------------
void updateClockDisplay(bool forceUpdate) {
  if (menuVisible || currentMode != MODE_CLOCK) return;

  // Перерисовываем если сменился режим отображения
  if (currentClockMode != lastClockMode) {
    forceUpdate = true;
    lastClockMode = currentClockMode;
    tft.fillScreen(TFT_BLACK); // Полная очистка при смене режима
  }

  switch(currentClockMode) {
    case CLOCK_NORMAL:
      drawNormalClock(forceUpdate);
      break;
    case CLOCK_LARGE_DIGITAL:
      drawLargeDigitalClock(forceUpdate);
      break;
    case CLOCK_ANALOG:
      drawAnalogClock(forceUpdate);
      break;
  }
}

void drawNormalClock(bool forceUpdate) {
  String currentTime = getRTCTime();
  String currentDay = getRTCDayOfWeek();
  String currentDate = getRTCDate();

  // Обновляем только при реальных изменениях
  bool timeChanged = (currentTime != lastRTCTime);
  bool dayChanged = (currentDay != lastRTCDay);
  
  if (forceUpdate || timeChanged || dayChanged) {
    drawUnifiedScreen(
      currentTime.c_str(),    // Строка 2: время
      currentDay.c_str(),     // Строка 3: день недели  
      currentDate.c_str(),    // Строка 4: дата
      TFT_YELLOW,
      true,
      forceUpdate
    );

    lastRTCTime = currentTime;
    lastRTCDay = currentDay;
    lastRTCDate = currentDate;
  } else {
    // Обновляем только дату если она изменилась
    if (currentDate != lastRTCDate) {
      updateLine4Only(currentDate.c_str(), TFT_YELLOW);
      lastRTCDate = currentDate;
    }
  }
}

void drawLargeDigitalClock(bool forceUpdate) {
  String currentTime = getRTCTime();
  String currentDate = getRTCDate();
  
  bool timeChanged = (currentTime != lastRTCTime);
  bool dateChanged = (currentDate != lastRTCDate);
  
  if (forceUpdate || timeChanged) {
    tft.fillScreen(TFT_BLACK);
    
    // Крупные цифровые часы
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN);
    tft.setTextSize(4);
    tft.drawString(currentTime, 120, 120);
    
    lastRTCTime = currentTime;
  }
  
  // Дата обновляется отдельно
  if (forceUpdate || dateChanged) {
    updateLine4Only(currentDate.c_str(), TFT_YELLOW);
    lastRTCDate = currentDate;
  }
}

void drawAnalogClock(bool forceUpdate) {
  if (forceUpdate) {
    drawClockFace();
  }
  updateAnalogClock();
}

void drawClockFace() {
  tft.fillScreen(TFT_BLACK);
  
  // Рисуем внешний круг часов (увеличенный радиус до краев экрана)
  tft.drawCircle(CLOCK_CENTER_X, CLOCK_CENTER_Y, CLOCK_RADIUS, TFT_WHITE);
  
  // Рисуем центральную точку
  tft.fillCircle(CLOCK_CENTER_X, CLOCK_CENTER_Y, 3, TFT_WHITE);
  
  // Рисуем часовые метки
  for (int i = 0; i < 12; i++) {
    float angle = i * PI / 6;
    int innerX = CLOCK_CENTER_X + (CLOCK_RADIUS - 10) * sin(angle);
    int innerY = CLOCK_CENTER_Y - (CLOCK_RADIUS - 10) * cos(angle);
    int outerX = CLOCK_CENTER_X + CLOCK_RADIUS * sin(angle);
    int outerY = CLOCK_CENTER_Y - CLOCK_RADIUS * cos(angle);
    tft.drawLine(innerX, innerY, outerX, outerY, TFT_WHITE);
  }
  
  // Рисуем минутные метки
  for (int i = 0; i < 60; i++) {
    if (i % 5 != 0) { // Пропускаем часовые метки
      float angle = i * PI / 30;
      int innerX = CLOCK_CENTER_X + (CLOCK_RADIUS - 5) * sin(angle);
      int innerY = CLOCK_CENTER_Y - (CLOCK_RADIUS - 5) * cos(angle);
      int outerX = CLOCK_CENTER_X + CLOCK_RADIUS * sin(angle);
      int outerY = CLOCK_CENTER_Y - CLOCK_RADIUS * cos(angle);
      tft.drawLine(innerX, innerY, outerX, outerY, TFT_DARKGREY);
    }
  }
}

void updateAnalogClock() {
  if (!rtcAvailable) return;
  
  DateTime now = rtc.now();
  
  // Обновляем только если секунда изменилась
  static uint8_t lastSecond = 255;
  if (now.second() == lastSecond) return;
  lastSecond = now.second();
  
  // Стираем старые стрелки
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_hx, last_hy, TFT_BLACK);
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_mx, last_my, TFT_BLACK); 
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_sx, last_sy, TFT_BLACK);
  
  // Стираем старую секундную точку
  drawDotAtSecond((now.second() == 0) ? 59 : now.second() - 1, TFT_BLACK);
  
  // Расчет новых позиций стрелок
  float hourAngle = (now.hour() % 12 + now.minute() / 60.0) * PI / 6;
  last_hx = CLOCK_CENTER_X + CLOCK_RADIUS * 0.5 * sin(hourAngle);
  last_hy = CLOCK_CENTER_Y - CLOCK_RADIUS * 0.5 * cos(hourAngle);
  
  float minAngle = now.minute() * PI / 30;
  last_mx = CLOCK_CENTER_X + CLOCK_RADIUS * 0.7 * sin(minAngle);
  last_my = CLOCK_CENTER_Y - CLOCK_RADIUS * 0.7 * cos(minAngle);
  
  float secAngle = now.second() * PI / 30;
  last_sx = CLOCK_CENTER_X + CLOCK_RADIUS * 0.8 * sin(secAngle);
  last_sy = CLOCK_CENTER_Y - CLOCK_RADIUS * 0.8 * cos(secAngle);
  
  // Рисуем новые стрелки
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_hx, last_hy, TFT_WHITE);
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_mx, last_my, TFT_CYAN);
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_sx, last_sy, TFT_RED);

  // Обновляем дату только если она изменилась
  String currentDate = getRTCDate();
  if (currentDate != lastRTCDate) {
    updateLine4Only(currentDate.c_str(), TFT_YELLOW);
    lastRTCDate = currentDate;
  }
}

void drawDotAtSecond(uint8_t sec, uint16_t color) {
  float angle = sec * PI / 30;
  int x = CLOCK_CENTER_X + TRACK_RADIUS * sin(angle);
  int y = CLOCK_CENTER_Y - TRACK_RADIUS * cos(angle);
  tft.fillCircle(x, y, DOT_RADIUS, color);
}

// ---------------------------------------------------------------------
//                           WiFi MANAGEMENT
// ---------------------------------------------------------------------
bool connectToWiFi() {
  Serial.println("Connecting to WiFi: " + String(ssid));

  WiFi.begin(ssid, password);
  unsigned long startTime = millis();
  
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());
    return true;
  } else {
    wifiConnected = false;
    Serial.println("\nWiFi connection failed");
    return false;
  }
}

void disconnectFromWiFi() {
  if (wifiConnected) {
    Serial.println("Disconnecting from WiFi...");
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiConnected = false;
    delay(100);
    Serial.println("WiFi disconnected");
  }
}

void stopRadio() {
  Serial.println("Stopping radio...");
  audio.stopSong();
  delay(100);
  Serial.println("Radio stopped");
}

// ---------------------------------------------------------------------
//                           MODE MANAGEMENT
// ---------------------------------------------------------------------
void saveCurrentMode() {
  preferences.begin("system", false);
  preferences.putUChar("currentMode", (uint8_t)currentMode);
  preferences.end();
}

SystemMode loadCurrentMode() {
  preferences.begin("system", true);
  uint8_t mode = preferences.getUChar("currentMode", MODE_RADIO);
  preferences.end();
  return (SystemMode)mode;
}

String getModeName(SystemMode mode) {
  switch(mode) {
    case MODE_RADIO: return "RADIO";
    case MODE_BLUETOOTH: return "BLUETOOTH";
    case MODE_CLOCK: return "CLOCK";
    case MODE_AP: return "AP MODE";
    case MODE_TEST: return "TEST";
    default: return "RADIO";
  }
}

int getCurrentModeIndex() {
  switch(currentMode) {
    case MODE_RADIO: return 0;
    case MODE_BLUETOOTH: return 1;
    case MODE_CLOCK: return 2;
    case MODE_AP: return 3;
    case MODE_TEST: return 4;
    default: return 0;
  }
}

void setSystemMode(SystemMode newMode) {
  if (currentMode == newMode) {
    menuVisible = false;
    drawCurrentModeScreen(true);
    return;
  }
  
  // Запрашиваем смену режима с перезагрузкой
  modeChangeRequested = true;
  requestedMode = newMode;
  showModeChangeScreen(newMode);
}

// ---------------------------------------------------------------------
//                           MODE CHANGE WITH REBOOT
// ---------------------------------------------------------------------
void showModeChangeScreen(SystemMode newMode) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(3);
  tft.drawString("CHANGE MODE", 120, 120);
  
  Serial.println("Mode change requested to: " + getModeName(newMode));
  delay(1500);
  
  // Сохраняем новый режим и перезагружаемся
  preferences.begin("system", false);
  preferences.putUChar("currentMode", (uint8_t)newMode);
  preferences.end();
  
  Serial.println("Rebooting ESP32...");
  tft.fillScreen(TFT_BLACK);
  delay(100);
  ESP.restart();
}

// ---------------------------------------------------------------------
//                           SPLASH SCREEN
// ---------------------------------------------------------------------
void showSplashScreen(String modeName) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(3);
  tft.drawString(modeName, 120, 120);

  const int steps = 40;
  const int stepDelay = 2500 / steps;
  const int centerX = 120;
  const int centerY = 120;
  const int ringWidth = 6;
  const int outerRadius = 120;
  const int innerRadius = outerRadius - ringWidth;

  for (int i = 0; i <= steps; i++) {
    uint8_t g = (i * 200 / steps);
    uint8_t b = (i * 255 / steps);
    uint16_t color = tft.color565(0, g, b);

    for (int r = innerRadius; r <= outerRadius; r++) {
      tft.drawCircle(centerX, centerY, r, color);
    }

    vTaskDelay(stepDelay / portTICK_PERIOD_MS);
  }
  
  vTaskDelay(500 / portTICK_PERIOD_MS);
  tft.fillScreen(TFT_BLACK);
}

// ---------------------------------------------------------------------
//                    ОПТИМИЗИРОВАННЫЙ TEST РЕЖИМ
// ---------------------------------------------------------------------
void initializeTestMode() {
  // Подключаемся к WiFi для теста
  if (connectToWiFi()) {
    // Настраиваем NTP для получения времени
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("NTP configured for TEST mode");
    // Синхронизируем RTC если доступен
    syncRTCFromNTP();
  }
  // Сбрасываем предыдущие значения для принудительного обновления
  lastWiFiSSID = "";
  lastIPAddress = "";
  lastNetworkTime = "";
}

String getNetworkTime() {
  if (!wifiConnected) {
    return "NO TIME";
  }
  
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "TIME ERROR";
  }
  
  char timeString[9];
  strftime(timeString, sizeof(timeString), "%H:%M:%S", &timeinfo);
  return String(timeString);
}

String getIPAddress() {
  if (!wifiConnected) {
    return "not connected";
  }
  return WiFi.localIP().toString();
}

String getWiFiSSID() {
  if (!wifiConnected) {
    return "ERROR";
  }
  return WiFi.SSID();
}

void updateTestInfo(bool forceUpdate) {
  if (menuVisible || currentMode != MODE_TEST) return;

  String currentWiFiSSID = getWiFiSSID();
  String currentIPAddress = getIPAddress();
  String currentNetworkTime = getNetworkTime();

  bool ssidChanged = (currentWiFiSSID != lastWiFiSSID);
  bool ipChanged = (currentIPAddress != lastIPAddress);
  bool timeChanged = (currentNetworkTime != lastNetworkTime);

  if (forceUpdate || ssidChanged || ipChanged) {
    // Перерисовываем только если изменились SSID или IP
    drawUnifiedScreen(
      currentWiFiSSID.c_str(),
      ("IP: " + currentIPAddress).c_str(), 
      currentNetworkTime.c_str(),
      wifiConnected ? TFT_GREEN : TFT_ORANGE,
      true,
      forceUpdate
    );

    lastWiFiSSID = currentWiFiSSID;
    lastIPAddress = currentIPAddress;
    lastNetworkTime = currentNetworkTime;
  } else if (timeChanged) {
    // Если изменилось только время - обновляем только строку 4
    updateLine4Only(currentNetworkTime.c_str(), wifiConnected ? TFT_GREEN : TFT_ORANGE);
    lastNetworkTime = currentNetworkTime;
  }
}

// ---------------------------------------------------------------------
//                           AUDIO INITIALIZATION
// ---------------------------------------------------------------------
void initializeAudioForCurrentMode() {
  // Всегда инициализируем аудио пины один раз
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(muted ? 0 : volume);
  
  // Запускаем радио только если в RADIO режиме
  if (currentMode == MODE_RADIO) {
    if (connectToWiFi()) {
      audio.connecttohost(streamURL);
      Serial.println("Radio started after reboot");
    }
  }
  // Для TEST режима инициализируем WiFi
  else if (currentMode == MODE_TEST) {
    initializeTestMode();
  }
}

// ---------------------------------------------------------------------
//                    ОПТИМИЗИРОВАННЫЙ RADIO РЕЖИМ
// ---------------------------------------------------------------------
void updateTrackInfo(bool forceUpdate) {
  if (menuVisible || currentMode != MODE_RADIO) return;

  bool stationChanged = (currentStation != lastStation);
  bool trackChanged = (currentTrack != lastTrack);
  
  if (forceUpdate || stationChanged || trackChanged) {
    String displayTrack = currentTrack;
    if (displayTrack.length() > 30) {
      displayTrack = displayTrack.substring(0, 30) + "...";
    }
    
    drawUnifiedScreen(
      currentStation.c_str(),
      displayTrack.c_str(),
      muted ? "MUTED" : ("VOL: " + String(volume)).c_str(),
      muted ? TFT_RED : TFT_YELLOW,
      true,
      forceUpdate
    );

    lastStation = currentStation;
    lastTrack = currentTrack;
  }
}

// ---------------------------------------------------------------------
//                    ОПТИМИЗИРОВАННОЕ ОБНОВЛЕНИЕ ГРОМКОСТИ
// ---------------------------------------------------------------------
void updateVolumeDisplay(bool forceUpdate) {
  if (menuVisible) return;
  
  if (forceUpdate || volume != lastVolume || muted != lastMute) {
    String volumeText = muted ? "MUTED" : ("VOL: " + String(volume));
    
    if (currentMode == MODE_RADIO || currentMode == MODE_BLUETOOTH || currentMode == MODE_AP) {
      // Для этих режимов обновляем только строку 4
      updateLine4Only(volumeText.c_str(), muted ? TFT_RED : TFT_YELLOW);
    } else if (currentMode == MODE_TEST) {
      // TEST режим уже обновляет время в своей функции
      // Ничего не делаем здесь
    }
    // CLOCK режим обновляет дату в своих функциях
    
    lastVolume = volume;
    lastMute = muted;
  }
}

// ---------------------------------------------------------------------
//                            MODE MANAGEMENT
// ---------------------------------------------------------------------
void drawCurrentModeScreen(bool forceUpdate) {
  if (!forceUpdate && currentMode == lastMode && menuVisible == lastMenuVisible) return;
  
  switch(currentMode) {
    case MODE_RADIO:
      if (wifiConnected) {
        updateTrackInfo(true);
      } else {
        drawUnifiedScreen(
          "RADIO",                      // Строка 2: режим
          "Connecting to WiFi...",      // Строка 3: статус
          muted ? "MUTED" : ("VOL: " + String(volume)).c_str(), // Строка 4: громкость
          muted ? TFT_RED : TFT_YELLOW,
          true,
          true
        );
      }
      break;
      
    case MODE_BLUETOOTH:
      drawUnifiedScreen(
        "BLUETOOTH",                   // Строка 2: режим
        "Audio Ready",                 // Строка 3: статус
        muted ? "MUTED" : ("VOL: " + String(volume)).c_str(), // Строка 4: громкость
        muted ? TFT_RED : TFT_YELLOW,
        true,
        true
      );
      break;
      
    case MODE_CLOCK:
      // Сбрасываем режим отображения при входе в CLOCK режим
      currentClockMode = CLOCK_NORMAL;
      lastClockMode = CLOCK_NORMAL;
      updateClockDisplay(true);
      break;
      
    case MODE_AP:
      drawUnifiedScreen(
        "AP MODE",                     // Строка 2: режим
        "Access Point Mode",           // Строка 3: описание
        "192.168.4.1",                 // Строка 4: IP
        TFT_CYAN,
        true,
        true
      );
      break;
      
    case MODE_TEST:
      updateTestInfo(true);
      break;
  }

  lastMode = currentMode;
  lastMenuVisible = menuVisible;
}

// ---------------------------------------------------------------------
//                           ICON FUNCTIONS
// ---------------------------------------------------------------------
void drawWiFiIcon(int x, int y, int bars, bool isAPMode) {
  uint16_t fillColor = isAPMode ? TFT_BLUE : TFT_GREEN;
  uint16_t borderColor = isAPMode ? TFT_NAVY : TFT_DARKGREEN;
  
  tft.fillRect(x - 20, y - 20, 40, 25, TFT_BLACK);
  
  int w = 6, h = 6, gap = 3;
  
  for (int i = 0; i < 4; i++) {
    int barHeight = (i + 1) * h;
    int barX = x - 15 + i * (w + gap);
    int barY = y - barHeight;
    
    if (i < bars) {
      tft.fillRect(barX, barY, w, barHeight, fillColor);
      tft.drawRect(barX, barY, w, barHeight, borderColor);
    } else {
      tft.drawRect(barX, barY, w, barHeight, TFT_DARKGREY);
    }
  }
}

void drawBluetoothIcon(int x, int y) {
  tft.fillRect(x - 12, y - 12, 25, 25, TFT_BLACK);
  
  tft.fillTriangle(x-5, y-8, x+5, y, x-5, y+8, TFT_BLUE);
  tft.drawFastVLine(x, y-8, 16, TFT_BLUE);
  tft.drawFastHLine(x-5, y-8, 5, TFT_BLUE);
  tft.drawFastHLine(x-5, y+8, 5, TFT_BLUE);
}

void drawClockIcon(int x, int y) {
  tft.fillCircle(x, y, 18, TFT_BLACK);
  tft.drawCircle(x, y, 15, TFT_GREEN);
  tft.fillCircle(x, y, 2, TFT_GREEN);
  tft.drawLine(x, y, x, y-10, TFT_GREEN);
  tft.drawLine(x, y, x+8, y, TFT_GREEN);
}

void drawAPModeIcon(int x, int y) {
  tft.fillRect(x - 20, y - 20, 40, 25, TFT_BLACK);
  
  int w = 6, h = 6, gap = 3;
  
  for (int i = 0; i < 4; i++) {
    int barHeight = (i + 1) * h;
    int barX = x - 15 + i * (w + gap);
    int barY = y - barHeight;
    
    tft.fillRect(barX, barY, w, barHeight, TFT_BLUE);
    tft.drawRect(barX, barY, w, barHeight, TFT_NAVY);
  }
}

void drawTestIcon(int x, int y) {
  tft.fillRect(x - 15, y - 15, 30, 30, TFT_BLACK);
  tft.drawRect(x-10, y-10, 20, 20, TFT_MAGENTA);
  tft.drawLine(x-8, y-2, x-3, y+3, TFT_MAGENTA);
  tft.drawLine(x-3, y+3, x+8, y-8, TFT_MAGENTA);
}

int rssiToBars(int rssi) {
  if (rssi > -55) return 4;
  else if (rssi > -65) return 3;
  else if (rssi > -75) return 2;
  else if (rssi > -85) return 1;
  else return 0;
}

// ---------------------------------------------------------------------
//                           MENU FUNCTIONS
// ---------------------------------------------------------------------
void drawMenu() {
  tft.fillScreen(TFT_BLACK);
  
  int currentModeIndex = getCurrentModeIndex();
  
  for (int i = 0; i < menuSize; i++) {
    int yPos = 20 + i * 40; // ПОДНЯТО НА 10 ПИКСЕЛЕЙ ВВЕРХ (было 40 + i * 40)
    
    if (i == currentMenuSelection) {
      tft.fillRect(0, yPos - 18, 240, 36, TFT_NAVY);
      tft.setTextColor(TFT_WHITE);
    } else if (i == currentModeIndex) {
      tft.fillRect(0, yPos - 18, 240, 36, TFT_DARKGREEN);
      tft.setTextColor(TFT_WHITE);
    } else {
      tft.setTextColor(TFT_SILVER);
    }
    
    tft.setTextSize(2);
    tft.setTextDatum(MC_DATUM);
    tft.drawString(menuItems[i], 120, yPos);
  }
}

void toggleMenu() {
  menuVisible = !menuVisible;
  if (menuVisible) {
    currentMenuSelection = getCurrentModeIndex();
    drawMenu();
  } else {
    drawCurrentModeScreen(true);
  }
}

void navigateMenu(int direction) {
  if (!menuVisible) return;
  currentMenuSelection += direction;
  if (currentMenuSelection < 0) currentMenuSelection = menuSize - 1;
  else if (currentMenuSelection >= menuSize) currentMenuSelection = 0;
  drawMenu();
}

void selectMenuItem() {
  if (!menuVisible) return;
  
  if (currentMenuSelection == menuSize - 1) {  // EXIT
    menuVisible = false;
    drawCurrentModeScreen(true);
    return;
  }
  
  SystemMode selectedMode;
  switch(currentMenuSelection) {
    case 0: selectedMode = MODE_RADIO; break;
    case 1: selectedMode = MODE_BLUETOOTH; break;
    case 2: selectedMode = MODE_CLOCK; break;
    case 3: selectedMode = MODE_AP; break;
    case 4: selectedMode = MODE_TEST; break;
    default: selectedMode = MODE_RADIO;
  }
  
  setSystemMode(selectedMode);
}

// ---------------------------------------------------------------------
//                           UTILITY FUNCTIONS
// ---------------------------------------------------------------------
uint16_t interpolateColor(uint16_t color1, uint16_t color2, float t) {
  uint8_t r1 = (color1 >> 11) & 0x1F;
  uint8_t g1 = (color1 >> 5) & 0x3F;
  uint8_t b1 = color1 & 0x1F;
  
  uint8_t r2 = (color2 >> 11) & 0x1F;
  uint8_t g2 = (color2 >> 5) & 0x3F;
  uint8_t b2 = color2 & 0x1F;
  
  uint8_t r = r1 + (r2 - r1) * t;
  uint8_t g = g1 + (g2 - g1) * t;
  uint8_t b = b1 + (b2 - b1) * t;
  
  return (r << 11) | (g << 5) | b;
}

// ---------------------------------------------------------------------
//                           AUDIO CALLBACKS
// ---------------------------------------------------------------------
void audio_info(const char *info) {
  Serial.printf("info: %s\n", info);
}

void audio_showstreamtitle(const char *info) {
  Serial.print("streamtitle: ");
  Serial.println(info);
  currentTrack = String(info);
  updateTrackInfo(false);
}

void audio_showstation(const char *info) {
  Serial.print("station: ");
  Serial.println(info);
  currentStation = String(info);
  updateTrackInfo(false);
}

// ---------------------------------------------------------------------
//                           AUDIO TASK
// ---------------------------------------------------------------------
void audioTask(void* parameter) {
  unsigned long lastWiFiReconnect = 0; // Добавляем таймер для WiFi
  
  // Заменяем poll на WiFi.onEvent для обработки WiFi событий
  WiFi.onEvent(wifi_event_handler);
  
  for (;;) {
    if (currentMode == MODE_RADIO) {
      // ИСПРАВЛЕНИЕ 2: Проверяем WiFi только раз в 10 секунд вместо постоянных проверок
      if (!WiFi.isConnected()) {
        if (millis() - lastWiFiReconnect > 10000) { // Только раз в 10 секунд
          connectToWiFi();
          lastWiFiReconnect = millis();
        }
      }
      if (wifiConnected) {
        audio.loop();
      }
    }
    vTaskDelay(1); // ВОЗВРАЩАЕМ ОБРАТНО 1 мс для плавного воспроизведения
  }
}

// ---------------------------------------------------------------------
//                           UI TASK
// ---------------------------------------------------------------------
void uiTask(void* parameter) {
  pinMode(VOL_UP_PIN, INPUT_PULLUP);
  pinMode(VOL_DOWN_PIN, INPUT_PULLUP);
  pinMode(MUTE_PIN, INPUT_PULLUP);
  pinMode(BUTTON_4_PIN, INPUT_PULLUP);
  pinMode(BUTTON_5_PIN, INPUT_PULLUP);

  unsigned long mutePressStart = 0;
  bool mutePressed = false;
  unsigned long lastRTCSync = 0;
  unsigned long lastClockRefresh = 0;

  for (;;) {
    bool changed = false;

    // Обработка уведомлений от WiFi событий для перерисовки RSSI
    uint32_t notificationValue;
    if (xTaskNotifyWait(0x00, ULONG_MAX, &notificationValue, 0) == pdTRUE) {
      // WiFi статус изменился - перерисовываем экран если в RADIO режиме
      if (currentMode == MODE_RADIO && !menuVisible) {
        drawCurrentModeScreen(true);
      }
    }

    // Обработка кнопок
    if (digitalRead(VOL_UP_PIN) == LOW) {
      if (menuVisible) {
        navigateMenu(-1);
        vTaskDelay(pdMS_TO_TICKS(200));
      } else if (currentMode == MODE_RADIO && volume < 21) { 
        volume++; 
        audio.setVolume(muted ? 0 : volume); 
        changed = true;
        vTaskDelay(pdMS_TO_TICKS(200));
      } else if (currentMode == MODE_CLOCK && !menuVisible) {
        // Переключаем режимы отображения часов вперед
        currentClockMode = (ClockDisplayMode)((currentClockMode + 1) % 3);
        updateClockDisplay(true);
        Serial.println("Clock mode changed to: " + String(currentClockMode));
        vTaskDelay(pdMS_TO_TICKS(200));
      }
    }

    if (digitalRead(VOL_DOWN_PIN) == LOW) {
      if (menuVisible) {
        navigateMenu(1);
        vTaskDelay(pdMS_TO_TICKS(200));
      } else if (currentMode == MODE_RADIO && volume > 0) { 
        volume--; 
        audio.setVolume(muted ? 0 : volume); 
        changed = true;
        vTaskDelay(pdMS_TO_TICKS(200));
      } else if (currentMode == MODE_CLOCK && !menuVisible) {
        // Переключаем режимы отображения часов назад
        currentClockMode = (ClockDisplayMode)((currentClockMode + 2) % 3);
        updateClockDisplay(true);
        Serial.println("Clock mode changed to: " + String(currentClockMode));
        vTaskDelay(pdMS_TO_TICKS(200));
      }
    }

    if (digitalRead(MUTE_PIN) == LOW && !mutePressed) {
      mutePressed = true; 
      mutePressStart = millis();
    }

    if (digitalRead(MUTE_PIN) == HIGH && mutePressed) {
      unsigned long pressDuration = millis() - mutePressStart;
      mutePressed = false;
      
      if (pressDuration < 1000) {
        if (menuVisible) {
          selectMenuItem();
        } else if (currentMode == MODE_RADIO) {
          muted = !muted; 
          audio.setVolume(muted ? 0 : volume); 
          changed = true;
        }
      } else {
        toggleMenu();
      }
    }

    // Обработка новых кнопок
    if (digitalRead(BUTTON_4_PIN) == LOW) {
      Serial.println("Button 4 pressed");
      vTaskDelay(pdMS_TO_TICKS(200));
    }

    if (digitalRead(BUTTON_5_PIN) == LOW) {
      Serial.println("Button 5 pressed");
      vTaskDelay(pdMS_TO_TICKS(200));
    }

    // Синхронизация RTC в TEST режиме
    if (currentMode == MODE_TEST && wifiConnected && (millis() - lastRTCSync > 30000)) {
      syncRTCFromNTP();
      lastRTCSync = millis();
    }

    // Обновление времени в CLOCK режиме
    if (currentMode == MODE_CLOCK && !menuVisible) {
      if (millis() - lastClockRefresh >= 1000) {
        updateClockDisplay(false);
        lastClockRefresh = millis();
      }
    }

    // Обновление TEST режима
    if (currentMode == MODE_TEST && !menuVisible) {
      updateTestInfo(false);
    }
    // Обновление RADIO режима
    else if (currentMode == MODE_RADIO && !menuVisible && (changed || lastVolume != volume || lastMute != muted)) {
      updateVolumeDisplay(false);
      lastVolume = volume; 
      lastMute = muted;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ---------------------------------------------------------------------
//                           SETUP
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  
  // Инициализируем RTC (не критично если модуль не подключен)
  initializeRTC();
  
  // Загружаем сохраненный режим
  currentMode = loadCurrentMode();
  
  // Инициализируем дисплей
  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);
  
  // Показываем сплеш-скрин с анимацией
  showSplashScreen(getModeName(currentMode));
  
  // Инициализируем аудио для текущего режима
  initializeAudioForCurrentMode();
  
  // Рисуем основной экран
  drawCurrentModeScreen(true);

  // Запускаем задачи
  xTaskCreatePinnedToCore(audioTask, "AudioTask", 8192, NULL, 2, &audioTaskHandle, 0);
  xTaskCreatePinnedToCore(uiTask, "UITask", 4096, NULL, 1, &uiTaskHandle, 1);
}

void loop() { 
  vTaskDelay(1); 
}
