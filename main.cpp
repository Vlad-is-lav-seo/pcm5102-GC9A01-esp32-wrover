#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <BluetoothA2DPSink.h>
#include "esp_bt.h"
#include <WiFi.h>

// ===== Display =====
TFT_eSPI tft = TFT_eSPI();

// ===== Bluetooth A2DP =====
BluetoothA2DPSink a2dp_sink;

// ===== I2S Pins =====
#define I2S_BCLK  26
#define I2S_LRC   25
#define I2S_DOUT  22

// ===== Button Pins =====
#define VOL_UP_PIN     32
#define MUTE_PIN       33
#define VOL_DOWN_PIN   27

// ===== WiFi Settings =====
const char* ssid = "Covid";
const char* password = "pass";

// ===== State =====
bool bluetoothConnected = false;
bool isMuted = false;
bool isSleeping = false;
bool wifiConnected = false;
float currentVolume = 0.3;
float savedVolume = 0.3;
volatile bool displayUpdateNeeded = false;
int currentMode = 1; // 0=RADIO, 1=BLUETOOTH, 2=AP MODE, 3=TEST MODE, 4=SLEEP

// ===== Menu System =====
bool menuVisible = false;
int currentMenuSelection = 0;
const char* menuItems[] = {
    "RADIO",
    "BLUETOOTH", 
    "AP MODE",
    "TEST MODE",
    "SLEEP"
};
const int menuSize = 5;

// Цвета фона для разных режимов
uint16_t modeColors[] = {
    TFT_RED,        // RADIO
    TFT_BLUE,       // BLUETOOTH
    TFT_GREEN,      // AP MODE
    TFT_YELLOW,     // TEST MODE
    TFT_PURPLE      // SLEEP
};

String connectionStatus = "Disconnected";
String currentTrackName = "No Track";
String btDeviceName = "No Device";

const unsigned long debounceDelay = 250;

// ===== Mutexes and Semaphores =====
SemaphoreHandle_t xDisplayMutex;
SemaphoreHandle_t xAudioMutex;
SemaphoreHandle_t xDisplaySemaphore;

// ===== Forward Declarations =====
void enterSleepMode();
void drawMenu();
void toggleMenu();
void navigateMenu(int direction);
void executeMenuItem();
void updateDisplay();
void setupBluetooth();
void stopBluetooth();
void setupWiFi();
void stopWiFi();

// ===== Callback for Bluetooth Connection State =====
void connection_state_changed(esp_a2d_connection_state_t state, void *param) {
  if (isSleeping || currentMode != 1) return; // Только в режиме Bluetooth
  
  if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
    Serial.printf("connection_state_changed: state=%d\n", state);
    if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
      bluetoothConnected = true;
      connectionStatus = "Connected";
      btDeviceName = "Unknown Device";
      Serial.printf("Bluetooth connected to: %s\n", btDeviceName.c_str());
    } else if (state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
      bluetoothConnected = false;
      connectionStatus = "Connecting";
      Serial.println("Bluetooth connecting");
    } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
      bluetoothConnected = false;
      connectionStatus = "Disconnected";
      currentTrackName = "No Track";
      btDeviceName = "No Device";
      Serial.println("Bluetooth disconnected");
    }
    if (!menuVisible) {
      displayUpdateNeeded = true;
      xSemaphoreGive(xDisplaySemaphore);
    }
    xSemaphoreGive(xDisplayMutex);
  }
}

// ===== Callback for AVRC Metadata =====
void avrc_metadata_callback(uint8_t attribute_id, const uint8_t *attribute_text) {
  if (isSleeping || currentMode != 1) return; // Только в режиме Bluetooth
  
  if (attribute_text != nullptr) {
    if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
      switch (attribute_id) {
        case ESP_AVRC_MD_ATTR_TITLE:
          currentTrackName = String((char*)attribute_text);
          Serial.printf("Track name received: %s\n", currentTrackName.c_str());
          if (!menuVisible) {
            displayUpdateNeeded = true;
            xSemaphoreGive(xDisplaySemaphore);
          }
          break;
        case ESP_AVRC_MD_ATTR_ARTIST:
          Serial.printf("Artist received: %s\n", (char*)attribute_text);
          break;
      }
      xSemaphoreGive(xDisplayMutex);
    }
  }
}

// ===== GAP Callback =====
void app_gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
  switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
      if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
        ESP_LOGI(BT_AV_TAG, "authentication success: %s", param->auth_cmpl.device_name);
        esp_log_buffer_hex(BT_AV_TAG, param->auth_cmpl.bda, ESP_BD_ADDR_LEN);
      } else {
        ESP_LOGE(BT_AV_TAG, "authentication failed, status:%d", param->auth_cmpl.stat);
      }
      break;
    }
    default: {
      break;
    }
  }
}

// ===== Show Splash Screen =====
void showSplashScreen() {
  if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextDatum(CC_DATUM);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(3);
    tft.drawString("O-SPEAKER", 120, 120);

    // Draw animated ring
    const int steps = 20;
    const int stepDelay = 2000 / steps;
    const int centerX = 120;
    const int centerY = 120;
    const int outerRadius = 120;
    const int innerRadius = outerRadius - 5;

    for (int i = 0; i <= steps; i++) {
      uint8_t r = 255 - (i * 255 / steps);
      uint8_t g = 255 - (i * 255 / steps);
      uint8_t b = 255;
      uint16_t color = tft.color565(r, g, b);

      for (int radius = innerRadius; radius <= outerRadius; radius++) {
        tft.drawCircle(centerX, centerY, radius, color);
      }

      xSemaphoreGive(xDisplayMutex);
      vTaskDelay(stepDelay / portTICK_PERIOD_MS);
      xSemaphoreTake(xDisplayMutex, portMAX_DELAY);
    }

    xSemaphoreGive(xDisplayMutex);
  }
}

// ===== Setup WiFi =====
void setupWiFi() {
  Serial.println("Connecting to WiFi...");
  
  if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
    tft.fillScreen(TFT_RED);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.setTextDatum(CC_DATUM);
    tft.drawString("WiFi Connecting", 120, 100);
    tft.setTextSize(1);
    tft.drawString(ssid, 120, 130);
    xSemaphoreGive(xDisplayMutex);
  }
  
  WiFi.begin(ssid, password);
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
    Serial.print(".");
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
      tft.fillScreen(TFT_RED);
      tft.setTextColor(TFT_WHITE);
      tft.setTextSize(2);
      tft.setTextDatum(CC_DATUM);
      tft.drawString("WiFi Connected", 120, 100);
      tft.setTextSize(1);
      tft.drawString(WiFi.localIP().toString().c_str(), 120, 130);
      xSemaphoreGive(xDisplayMutex);
    }
    
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  } else {
    Serial.println("\nWiFi connection failed!");
    wifiConnected = false;
    
    if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
      tft.fillScreen(TFT_RED);
      tft.setTextColor(TFT_WHITE);
      tft.setTextSize(2);
      tft.setTextDatum(CC_DATUM);
      tft.drawString("WiFi Failed", 120, 100);
      xSemaphoreGive(xDisplayMutex);
    }
    
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

// ===== Stop WiFi =====
void stopWiFi() {
  if (wifiConnected) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiConnected = false;
    Serial.println("WiFi disconnected");
  }
}

// ===== Setup Bluetooth =====
void setupBluetooth() {
  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
  };
  a2dp_sink.set_pin_config(pin_config);

  // Register Callbacks
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);
  a2dp_sink.set_on_connection_state_changed(connection_state_changed);
  a2dp_sink.set_volume(int(currentVolume * 127));
  a2dp_sink.start("ESP32-Speaker");

  // Register GAP callback for remote device name
  esp_bt_gap_register_callback(app_gap_callback);
  
  bluetoothConnected = false;
  connectionStatus = "Disconnected";
  btDeviceName = "No Device";
  currentTrackName = "No Track";
  
  Serial.println("Bluetooth started");
}

// ===== Stop Bluetooth =====
void stopBluetooth() {
  a2dp_sink.end();
  esp_bluedroid_disable();
  esp_bt_controller_disable();
  
  bluetoothConnected = false;
  connectionStatus = "Bluetooth Off";
  btDeviceName = "No Device";
  currentTrackName = "No Track";
  
  Serial.println("Bluetooth stopped");
}

// ===== Enter Sleep Mode =====
void enterSleepMode() {
  Serial.println("=== ATTEMPTING DEEP SLEEP ===");
  
  // 1. Показываем сообщение
  if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_ORANGE);
    tft.setTextSize(2);
    tft.setTextDatum(CC_DATUM);
    tft.drawString("GOING TO SLEEP", 120, 100);
    tft.setTextSize(1);
    tft.drawString("Press MUTE to wake up", 120, 130);
    xSemaphoreGive(xDisplayMutex);
  }
  
  // 2. Ждем 2 секунды
  Serial.println("1. Sleep message shown, waiting 2 seconds...");
  vTaskDelay(2000 / portTICK_PERIOD_MS);
  
  // 3. Выключаем всё
  Serial.println("2. Stopping all services...");
  stopWiFi();
  stopBluetooth();
  
  // 4. Выключаем дисплей
  Serial.println("3. Turning off display...");
  tft.writecommand(TFT_DISPOFF);
  tft.writecommand(TFT_SLPIN);
  delay(100);
  
  // 5. Пробуем альтернативный подход - используем light sleep
  Serial.println("4. Configuring wakeup...");
  
  // Отключаем WiFi и Bluetooth полностью
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  btStop();
  
  // Настраиваем пробуждение
  esp_sleep_enable_ext0_wakeup((gpio_num_t)MUTE_PIN, 0);
  
  // Даем время на завершение операций
  delay(500);
  Serial.println("5. Entering light sleep...");
  Serial.flush();
  
  // Используем light sleep вместо deep sleep
  esp_light_sleep_start();
  
  // Если дошли сюда - проснулись от light sleep
  Serial.println("6. Woke up from light sleep - rebooting");
  
  // После light sleep делаем полную перезагрузку
  ESP.restart();
}

// ===== Draw Menu =====
void drawMenu() {
  if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
    tft.fillScreen(TFT_BLACK);
    
    // Пункты меню - на всю ширину экрана
    tft.setTextSize(2);
    tft.setTextDatum(CC_DATUM);
    
    for (int i = 0; i < menuSize; i++) {
      if (i == currentMenuSelection) {
        tft.fillRect(0, 40 + i * 35, 240, 30, TFT_NAVY);
        tft.setTextColor(TFT_WHITE);
      } else {
        tft.setTextColor(TFT_SILVER);
      }
      tft.drawString(menuItems[i], 120, 55 + i * 35);
    }
    
    xSemaphoreGive(xDisplayMutex);
  }
}

// Асинхронное подключение к WiFi во время заставки
void connectWiFiDuringSplash() {
  wifiConnected = false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 4000) {
    Serial.print(".");
    vTaskDelay(250 / portTICK_PERIOD_MS);
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\nWiFi connected during splash!");
  } else {
    wifiConnected = false;
    Serial.println("\nWiFi connect timeout.");
  }
}

// ===== Execute Menu Item =====
void executeMenuItem() {
  // Проверяем, не пытаемся ли выбрать текущий режим
  if (currentMenuSelection == currentMode) {
    // Просто скрываем меню без изменений
    menuVisible = false;
    displayUpdateNeeded = true;
    xSemaphoreGive(xDisplaySemaphore);
    return;
  }
  
  int previousMode = currentMode;
  currentMode = currentMenuSelection;
  
  // Управление подключениями при смене режима
  if (previousMode == 0 && currentMode != 0) stopWiFi();
  if (previousMode == 1 && currentMode != 1) stopBluetooth();

  // ===== Показываем заставку выбора режима =====
  if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
    tft.fillScreen(modeColors[currentMode]);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.setTextDatum(CC_DATUM);
    tft.drawString("MODE: " + String(menuItems[currentMode]), 120, 100);
    xSemaphoreGive(xDisplayMutex);
  }

  // Если это RADIO — подключаем WiFi во время заставки
  if (currentMode == 0) {
    Serial.println(">>> Connecting WiFi during splash...");
    connectWiFiDuringSplash();
  } else if (currentMode == 1) {
    setupBluetooth();
  }

  // Ждём 2 секунды (заставка)
  vTaskDelay(2000 / portTICK_PERIOD_MS);

  // === После заставки показываем результат подключения ===
  if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
    tft.fillScreen(modeColors[currentMode]);
    tft.setTextColor(TFT_WHITE);
    tft.setTextSize(2);
    tft.setTextDatum(CC_DATUM);

    if (currentMode == 0) {
      tft.drawString("INTERNET RADIO", 120, 80);
      if (wifiConnected) {
        tft.setTextSize(1);
        tft.drawString("WiFi Connected", 120, 110);
        tft.drawString(WiFi.localIP().toString().c_str(), 120, 130);
      } else {
        tft.setTextSize(1);
        tft.drawString("WiFi Failed", 120, 110);
      }
    } else if (currentMode == 1) {
      tft.drawString("BLUETOOTH MODE", 120, 100);
      tft.setTextSize(1);
      tft.drawString("Waiting for device...", 120, 130);
    } else if (currentMode == 2) {
      tft.drawString("AP MODE ACTIVE", 120, 100);
    } else if (currentMode == 3) {
      tft.drawString("TEST MODE", 120, 100);
    } else if (currentMode == 4) {
      enterSleepMode();
      return;
    }

    xSemaphoreGive(xDisplayMutex);
  }

  // После показа статуса 1 секунда паузы
  vTaskDelay(1000 / portTICK_PERIOD_MS);

  // Скрываем меню и обновляем экран
  menuVisible = false;
  displayUpdateNeeded = true;
  xSemaphoreGive(xDisplaySemaphore);
}

// ===== Toggle Menu Visibility =====
void toggleMenu() {
  if (isSleeping) return;
  
  menuVisible = !menuVisible;
  if (menuVisible) {
    drawMenu();
  } else {
    displayUpdateNeeded = true;
    xSemaphoreGive(xDisplaySemaphore);
  }
}

// ===== Navigate Menu =====
void navigateMenu(int direction) {
  if (!menuVisible || isSleeping) return;
  
  currentMenuSelection += direction;
  if (currentMenuSelection < 0) {
    currentMenuSelection = menuSize - 1;
  } else if (currentMenuSelection >= menuSize) {
    currentMenuSelection = 0;
  }
  
  drawMenu();
}

// ===== Set Volume =====
void setAudioVolume(float volume) {
  if (isSleeping) return;
  
  if (xSemaphoreTake(xAudioMutex, portMAX_DELAY) == pdTRUE) {
    currentVolume = volume;
    if (currentMode == 1) {
      a2dp_sink.set_volume((int)(volume * 127));
    }
    xSemaphoreGive(xAudioMutex);
    Serial.printf("Volume set: %.0f%%\n", volume * 100);
    if (!menuVisible) {
      displayUpdateNeeded = true;
      xSemaphoreGive(xDisplaySemaphore);
    }
  }
}

// ===== Handle Buttons =====
void handleButtons() {
  static unsigned long lastButtonTime = 0;
  unsigned long currentTime = millis();
  if (currentTime - lastButtonTime < debounceDelay) return;

  if (digitalRead(VOL_UP_PIN) == LOW) {
    if (menuVisible) {
      navigateMenu(-1);
    } else {
      if (isMuted) {
        isMuted = false;
        setAudioVolume(savedVolume);
      } else {
        setAudioVolume(min(currentVolume + 0.05, 1.0));
      }
    }
    lastButtonTime = currentTime;
  }

  if (digitalRead(VOL_DOWN_PIN) == LOW) {
    if (menuVisible) {
      navigateMenu(1);
    } else {
      if (isMuted) {
        isMuted = false;
        setAudioVolume(savedVolume);
      } else {
        setAudioVolume(max(currentVolume - 0.05, 0.0));
      }
    }
    lastButtonTime = currentTime;
  }

  if (digitalRead(MUTE_PIN) == LOW) {
    unsigned long pressStartTime = millis();
    
    while(digitalRead(MUTE_PIN) == LOW) {
      vTaskDelay(50 / portTICK_PERIOD_MS);
    }
    
    unsigned long pressDuration = millis() - pressStartTime;
    
    if (menuVisible) {
      executeMenuItem();
    } else {
      if (pressDuration < 1000) {
        isMuted = !isMuted;
        if (isMuted) {
          savedVolume = currentVolume;
          setAudioVolume(0.0);
        } else {
          setAudioVolume(savedVolume);
        }
      } else {
        menuVisible = true;
        drawMenu();
      }
    }
    lastButtonTime = currentTime;
  }
}

// ===== Update Display =====
void updateDisplay() {
  if (xSemaphoreTake(xDisplayMutex, portMAX_DELAY) == pdTRUE) {
    tft.fillScreen(modeColors[currentMode]);
    tft.setTextDatum(CC_DATUM);
    
    if (currentMode == 3) {
      tft.setTextColor(TFT_BLACK);
    } else {
      tft.setTextColor(TFT_WHITE);
    }
    
    tft.setTextSize(2);

    // Информация в зависимости от режима
    if (currentMode == 0) { // RADIO
      tft.drawString("INTERNET RADIO", 120, 40);
      if (wifiConnected) {
        tft.drawString("WiFi: Connected", 120, 70);
        tft.drawString("Ready to play", 120, 100);
      } else {
        tft.drawString("WiFi: Disconnected", 120, 70);
        tft.drawString("Check connection", 120, 100);
      }
    } else if (currentMode == 1) { // BLUETOOTH
      tft.drawString(btDeviceName, 120, 40);
      tft.drawString(connectionStatus, 120, 70);
      tft.drawString(currentTrackName, 120, 100);
    } else {
      tft.drawString(menuItems[currentMode], 120, 50);
      tft.drawString("Mode Active", 120, 80);
    }
    
    // Отображаем Volume
    int volPercent = int(currentVolume * 100);
    String volumeText = "Volume: " + String(volPercent) + "%";
    if (isMuted) volumeText = "Volume: MUTED";
    tft.drawString(volumeText, 120, 140);

    // Полоска объема
    int barWidth = 200;
    int barHeight = 15;
    int x0 = 20;
    int y0 = 170;
    
    uint16_t barColor = (currentMode == 3) ? TFT_BLACK : TFT_WHITE;
    tft.drawRect(x0, y0, barWidth, barHeight, barColor);
    int filled = int(currentVolume * barWidth);
    tft.fillRect(x0, y0, filled, barHeight, barColor);

    // Подсказка про меню
    tft.setTextColor((currentMode == 3) ? TFT_BLACK : TFT_YELLOW);
    tft.setTextSize(1);
    tft.drawString("Hold MUTE for Menu", 120, 200);

    xSemaphoreGive(xDisplayMutex);
  }
}

// ===== Button Task =====
void buttonTask(void *pvParameters) {
  pinMode(VOL_UP_PIN, INPUT_PULLUP);
  pinMode(MUTE_PIN, INPUT_PULLUP);
  pinMode(VOL_DOWN_PIN, INPUT_PULLUP);

  while (true) {
    handleButtons();
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

// ===== Display Task =====
void displayTask(void *pvParameters) {
  while (true) {
    if (xSemaphoreTake(xDisplaySemaphore, portMAX_DELAY) == pdTRUE) {
      if (displayUpdateNeeded && !menuVisible && !isSleeping) {
        updateDisplay();
        displayUpdateNeeded = false;
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  
  // Проверяем пробуждение от сна
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  if (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("=== WOKE UP FROM SLEEP ===");
    delay(1000);
  } else {
    Serial.println("=== NORMAL BOOT ===");
  }
  
  xDisplayMutex = xSemaphoreCreateMutex();
  xAudioMutex = xSemaphoreCreateMutex();
  xDisplaySemaphore = xSemaphoreCreateBinary();

  // Initialize TFT
  tft.init();
  tft.setRotation(2);

  // Show splash screen
  showSplashScreen();
  vTaskDelay(100 / portTICK_PERIOD_MS);

  // Запускаем Bluetooth только если начальный режим Bluetooth
  if (currentMode == 1) {
    setupBluetooth();
  }

  // Initial display update
  displayUpdateNeeded = true;
  xSemaphoreGive(xDisplaySemaphore);

  // Запускаем задачи на разных ядрах
  xTaskCreatePinnedToCore(buttonTask, "ButtonTask", 2048, NULL, 3, NULL, 0);
  xTaskCreatePinnedToCore(displayTask, "DisplayTask", 8192, NULL, 1, NULL, 1);
}

void loop() {
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}
