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

// ==== Function Prototypes ====
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
//                           CLOCK MODE FUNCTIONS
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

  bool needsUpdate = forceUpdate || 
                    currentTime != lastRTCTime || 
                    currentDay != lastRTCDay;

  if (needsUpdate) {
    tft.fillRect(0, 70, 240, 80, TFT_BLACK);

    tft.setTextDatum(MC_DATUM);
    
    // Верхняя строка - время
    tft.setTextColor(TFT_CYAN);
    tft.setTextSize(2);
    tft.drawString(currentTime, 120, 90);

    // Нижняя строка - день недели
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    tft.drawString(currentDay, 120, 120);

    // Обновляем последние значения
    lastRTCTime = currentTime;
    lastRTCDay = currentDay;
  }
}

void drawLargeDigitalClock(bool forceUpdate) {
  String currentTime = getRTCTime();
  bool needsUpdate = forceUpdate || currentTime != lastRTCTime;

  if (needsUpdate) {
    tft.fillScreen(TFT_BLACK);
    
    // ИСПРАВЛЕНИЕ 1: Уменьшаем шрифт с 6 до 4 и показываем полное время с секундами
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN);
    tft.setTextSize(4); // УМЕНЬШЕН РАЗМЕР ШРИФТА ДО 4 (было 6)
    
    // Рисуем полное время с секундами "HH:MM:SS" по центру экрана
    tft.drawString(currentTime, 120, 120);
    
    lastRTCTime = currentTime;
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
  
  // УБРАНА НАДПИСЬ РЕЖИМА ПОД ЧАСАМИ
}

void updateAnalogClock() {
  if (!rtcAvailable) return;
  
  DateTime now = rtc.now();
  
  // Стираем старые стрелки
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_hx, last_hy, TFT_BLACK);
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_mx, last_my, TFT_BLACK);
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_sx, last_sy, TFT_BLACK);
  
  // Стираем старую секундную точку
  drawDotAtSecond((now.second() == 0) ? 59 : now.second() - 1, TFT_BLACK);
  
  // Часовая стрелка
  float hourAngle = (now.hour() % 12 + now.minute() / 60.0) * PI / 6;
  last_hx = CLOCK_CENTER_X + CLOCK_RADIUS * 0.5 * sin(hourAngle);
  last_hy = CLOCK_CENTER_Y - CLOCK_RADIUS * 0.5 * cos(hourAngle);
  
  // Минутная стрелка
  float minAngle = now.minute() * PI / 30;
  last_mx = CLOCK_CENTER_X + CLOCK_RADIUS * 0.7 * sin(minAngle);
  last_my = CLOCK_CENTER_Y - CLOCK_RADIUS * 0.7 * cos(minAngle);
  
  // Секундная стрелка
  float secAngle = now.second() * PI / 30;
  last_sx = CLOCK_CENTER_X + CLOCK_RADIUS * 0.8 * sin(secAngle);
  last_sy = CLOCK_CENTER_Y - CLOCK_RADIUS * 0.8 * cos(secAngle);
  
  // Рисуем новые стрелки
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_hx, last_hy, TFT_WHITE); // Часовая - белая
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_mx, last_my, TFT_CYAN);  // Минутная - голубая
  
  // ИСПРАВЛЕНИЕ 3: Сначала рисуем секундную точку, потом секундную стрелку
  // Рисуем текущую секундную точку ПЕРЕД секундной стрелкой
  drawDotAtSecond(now.second(), TFT_RED);
  
  // Теперь рисуем секундную стрелку ПОВЕРХ точки
  tft.drawLine(CLOCK_CENTER_X, CLOCK_CENTER_Y, last_sx, last_sy, TFT_RED);   // Секундная - красная
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
//                           TEST MODE FUNCTIONS
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

  bool needsUpdate = forceUpdate || 
                    currentWiFiSSID != lastWiFiSSID || 
                    currentIPAddress != lastIPAddress || 
                    currentNetworkTime != lastNetworkTime;

  if (needsUpdate) {
    tft.fillRect(0, 70, 240, 80, TFT_BLACK);

    tft.setTextDatum(MC_DATUM);
    
    // Верхняя строка - название WiFi сети
    tft.setTextColor(wifiConnected ? TFT_GREEN : TFT_RED);
    tft.setTextSize(2);
    tft.drawString(currentWiFiSSID, 120, 90);

    // Нижняя строка - IP адрес
    tft.setTextColor(wifiConnected ? TFT_CYAN : TFT_WHITE);
    tft.setTextSize(1);
    tft.drawString("IP: " + currentIPAddress, 120, 120);

    // Обновляем последние значения
    lastWiFiSSID = currentWiFiSSID;
    lastIPAddress = currentIPAddress;
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
//                            DRAW TRACK INFO
// ---------------------------------------------------------------------
void updateTrackInfo(bool forceUpdate) {
  if (menuVisible || currentMode != MODE_RADIO) return;

  if (forceUpdate || currentTrack != lastTrack || currentStation != lastStation) {
    tft.fillRect(0, 70, 240, 80, TFT_BLACK);

    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_CYAN);
    tft.setTextSize(2);
    tft.drawString(currentStation, 120, 90);

    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(1);
    
    String displayTrack = currentTrack;
    if (displayTrack.length() > 30) {
      displayTrack = displayTrack.substring(0, 30) + "...";
    }
    tft.drawString(displayTrack, 120, 120);

    lastTrack = currentTrack;
    lastStation = currentStation;
  }
}

// ---------------------------------------------------------------------
//                           UPDATE VOLUME DISPLAY
// ---------------------------------------------------------------------
void updateVolumeDisplay(bool forceUpdate) {
  if (menuVisible) return;
  
  if (currentMode == MODE_TEST) {
    // В TEST режиме проверяем изменения времени
    String currentTime = getNetworkTime();
    bool timeChanged = (currentTime != lastNetworkTime);
    
    if (forceUpdate || timeChanged) {
      tft.fillRect(0, 165, 240, 25, TFT_BLACK);
      
      tft.setTextDatum(MC_DATUM);
      tft.setTextSize(2);
      tft.setTextColor(wifiConnected ? TFT_GREEN : TFT_ORANGE);
      tft.drawString(currentTime, 120, 180);
      
      lastNetworkTime = currentTime;
    }
  }
  else if (currentMode == MODE_CLOCK) {
    // В CLOCK режиме показываем дату вместо громкости
    String currentDate = getRTCDate();
    bool dateChanged = (currentDate != lastRTCDate);
    
    if (forceUpdate || dateChanged) {
      tft.fillRect(0, 165, 240, 25, TFT_BLACK);
      
      tft.setTextDatum(MC_DATUM);
      tft.setTextSize(2);
      tft.setTextColor(TFT_YELLOW);
      tft.drawString(currentDate, 120, 180);
      
      lastRTCDate = currentDate;
    }
  }
  else {
    // В других режимах проверяем изменения громкости
    if (forceUpdate || volume != lastVolume || muted != lastMute) {
      tft.fillRect(0, 165, 240, 25, TFT_BLACK);
      
      tft.setTextDatum(MC_DATUM);
      tft.setTextSize(2);
      
      if (muted) {
        tft.setTextColor(TFT_RED);
      } else {
        tft.setTextColor(TFT_YELLOW);
      }
      tft.drawString("VOL: " + String(volume), 120, 180);

      lastVolume = volume;
      lastMute = muted;
    }
  }
}

// ---------------------------------------------------------------------
//                            MODE MANAGEMENT
// ---------------------------------------------------------------------
void drawCurrentModeScreen(bool forceUpdate) {
  if (!forceUpdate && currentMode == lastMode && menuVisible == lastMenuVisible) return;
  
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);

  int iconY = 40;
  
  switch(currentMode) {
    case MODE_RADIO:
      if (wifiConnected) {
        int rssi = WiFi.RSSI();
        int bars = rssiToBars(rssi);
        drawWiFiIcon(120, iconY, bars, false);
      } else {
        drawWiFiIcon(120, iconY, 0, false);
      }
      updateTrackInfo(true);
      break;
      
    case MODE_BLUETOOTH:
      drawBluetoothIcon(120, iconY);
      tft.setTextColor(TFT_WHITE);
      tft.setTextSize(2);
      tft.drawString("BLUETOOTH", 120, 90);
      tft.setTextColor(TFT_GREEN);
      tft.setTextSize(1);
      tft.drawString("Audio Ready", 120, 120);
      break;
      
    case MODE_CLOCK:
      drawClockIcon(120, iconY);
      tft.setTextColor(TFT_GREEN);
      tft.setTextSize(3);
      tft.drawString("CLOCK", 120, 90);
      
      // Сбрасываем режим отображения при входе в CLOCK режим
      currentClockMode = CLOCK_NORMAL;
      lastClockMode = CLOCK_NORMAL;
      
      updateClockDisplay(true);
      break;
      
    case MODE_AP:
      drawAPModeIcon(120, iconY);
      tft.setTextColor(TFT_WHITE);
      tft.setTextSize(2);
      tft.drawString("AP MODE", 120, 90);
      tft.setTextColor(TFT_GREEN);
      tft.setTextSize(1);
      tft.drawString("Access Point Mode", 120, 120);
      break;
      
    case MODE_TEST:
      drawTestIcon(120, iconY);
      tft.setTextColor(TFT_WHITE);
      tft.setTextSize(2);
      tft.drawString("TEST", 120, 90);
      updateTestInfo(true);
      break;
  }

  updateVolumeDisplay(true);

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
    int yPos = 40 + i * 40;
    
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
        updateVolumeDisplay(false); // Обновляем дату в строке громкости
        lastClockRefresh = millis();
      }
    }

    // Обновление TEST режима
    if (currentMode == MODE_TEST && !menuVisible) {
      updateTestInfo(false);
      updateVolumeDisplay(false);
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
