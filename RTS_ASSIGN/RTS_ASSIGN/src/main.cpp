/* ======================================================
======================================================
ASSIGNMENT 5:Smart Agriculture – Greenhouse Control
    Group members:
    NURAFRINA IZZATI BINTI ROSLEE - B122310073
    ARIF HAIQAL BIN MOHD JAFFRI - B122310018
    JUSTEN A/L WILLIAM - B122310257
    RESHVIN GOVINDAN - B122310346
    UTAYA RAJ A/L RAVI - B122310576
======================================================
======================================================*/

#include <Arduino.h>
#include <DHTesp.h>
#include <LiquidCrystal_I2C.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ================= PIN CONFIG =================
#define PIN_SOIL 34
#define PIN_PUMP 26
#define PIN_DHT 15
#define PIN_LED 2

// ================= TIMING =================
#define SOIL_PERIOD   500
#define DHT_PERIOD    2000
#define MQTT_PERIOD   10000
#define LCD_PERIOD    1000

#define MQTT_EXEC_MS  1500
#define PUMP_ON_MS    500

// ================= PRIORITIES =================
#define PRI_SOIL  5
#define PRI_DHT   3
#define PRI_MQTT  1
#define PRI_LCD   2

// ================= OBJECTS =================
DHTesp dht;
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ================= SYNC =================
SemaphoreHandle_t mutexStats;
SemaphoreHandle_t mutexSerial;
SemaphoreHandle_t mqttQueueMutex;

// ================= DATA =================
struct Stats {
  int soil;
  float temp;
  float hum;
  int pumpCount;
  int mqttCount;
  int missed;
};

Stats stats = {};

// ================= SOIL TASK =================
void taskSoil(void *pv) {
  TickType_t last = xTaskGetTickCount();

  pinMode(PIN_SOIL, INPUT);
  pinMode(PIN_PUMP, OUTPUT);

  bool pumpState = false;

  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(SOIL_PERIOD));

    int raw = analogRead(PIN_SOIL);

    // FIX: correct mapping (Wokwi fix)
    int moisture = map(raw, 0, 4095, 100, 0);

    // simple smoothing fix (stability)
    moisture = constrain(moisture, 0, 100);

    xSemaphoreTake(mutexStats, portMAX_DELAY);
    stats.soil = moisture;
    xSemaphoreGive(mutexStats);

    xSemaphoreTake(mutexSerial, portMAX_DELAY);
    Serial.printf("[SOIL] Moisture=%d\n", moisture);
    xSemaphoreGive(mutexSerial);

    // HYSTERESIS FIX (prevents spam ON/OFF)
    if (moisture < 30 && !pumpState) {

      uint32_t start = micros();

      digitalWrite(PIN_PUMP, HIGH);
      pumpState = true;

      uint32_t latency = (micros() - start) / 1000;

      xSemaphoreTake(mutexStats, portMAX_DELAY);
      stats.pumpCount++;
      xSemaphoreGive(mutexStats);

      xSemaphoreTake(mutexSerial, portMAX_DELAY);
      Serial.printf("[PUMP ON] latency=%lu ms\n", latency);
      xSemaphoreGive(mutexSerial);

      vTaskDelay(pdMS_TO_TICKS(PUMP_ON_MS));

      digitalWrite(PIN_PUMP, LOW);
      pumpState = false;

      xSemaphoreTake(mutexSerial, portMAX_DELAY);
      Serial.println("[PUMP OFF]");
      xSemaphoreGive(mutexSerial);
    }
  }
}

// ================= DHT TASK =================
void taskDHT(void *pv) {
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(DHT_PERIOD));

    TempAndHumidity data = dht.getTempAndHumidity();

    xSemaphoreTake(mutexStats, portMAX_DELAY);
    stats.temp = data.temperature;
    stats.hum = data.humidity;
    xSemaphoreGive(mutexStats);

    xSemaphoreTake(mutexSerial, portMAX_DELAY);
    Serial.printf("[DHT] T=%.1f H=%.1f\n", data.temperature, data.humidity);
    xSemaphoreGive(mutexSerial);
  }
}

// ================= MQTT TASK (NON-BLOCKING FIX) =================
void taskMQTT(void *pv) {
  TickType_t last = xTaskGetTickCount();

  for (;;) {
    vTaskDelayUntil(&last, pdMS_TO_TICKS(MQTT_PERIOD));

    // simulate async queue (NOT blocking system anymore)
    if (xSemaphoreTake(mqttQueueMutex, pdMS_TO_TICKS(10)) == pdTRUE) {

      xSemaphoreTake(mutexStats, portMAX_DELAY);
      float t = stats.temp;
      float h = stats.hum;
      int m = stats.soil;
      xSemaphoreGive(mutexStats);

      xSemaphoreTake(mutexSerial, portMAX_DELAY);
      Serial.printf("[MQTT QUEUED] T=%.1f H=%.1f M=%d\n", t, h, m);
      xSemaphoreGive(mutexSerial);

      vTaskDelay(pdMS_TO_TICKS(MQTT_EXEC_MS)); // simulate slow network

      xSemaphoreTake(mutexStats, portMAX_DELAY);
      stats.mqttCount++;
      xSemaphoreGive(mutexStats);

      xSemaphoreGive(mqttQueueMutex);

      xSemaphoreTake(mutexSerial, portMAX_DELAY);
      Serial.println("[MQTT DONE]");
      xSemaphoreGive(mutexSerial);
    }
  }
}

// ================= LCD TASK =================
void taskLCD(void *pv) {
  lcd.init();
  lcd.backlight();

  for (;;) {
    xSemaphoreTake(mutexStats, portMAX_DELAY);

    lcd.setCursor(0, 0);
    lcd.printf("T:%.1f H:%.1f", stats.temp, stats.hum);

    lcd.setCursor(0, 1);
    lcd.printf("Soil:%d%%", stats.soil);

    xSemaphoreGive(mutexStats);

    vTaskDelay(pdMS_TO_TICKS(LCD_PERIOD));
  }
}

// ================= HEARTBEAT =================
void taskLED(void *pv) {
  pinMode(PIN_LED, OUTPUT);

  for (;;) {
    digitalWrite(PIN_LED, HIGH);
    vTaskDelay(pdMS_TO_TICKS(250));
    digitalWrite(PIN_LED, LOW);
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  dht.setup(PIN_DHT, DHTesp::DHT22);

  mutexStats = xSemaphoreCreateMutex();
  mutexSerial = xSemaphoreCreateMutex();
  mqttQueueMutex = xSemaphoreCreateMutex();

  xTaskCreate(taskSoil, "soil", 4096, NULL, PRI_SOIL, NULL);
  xTaskCreate(taskDHT, "dht", 4096, NULL, PRI_DHT, NULL);
  xTaskCreate(taskMQTT, "mqtt", 4096, NULL, PRI_MQTT, NULL);
  xTaskCreate(taskLCD, "lcd", 4096, NULL, PRI_LCD, NULL);
  xTaskCreate(taskLED, "led", 2048, NULL, 1, NULL);

  Serial.println("System Started");
}

void loop() {
  vTaskDelete(NULL);
}