#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <DHT.h>
#include <LiquidCrystal_I2C.h>

// ==================== PIN DEFINITIONS ====================
#define SOIL_MOISTURE_PIN   34
#define DHT_PIN             15
#define PUMP_PIN            5
#define DHT_TYPE            DHT22

// ==================== CONSTANTS ====================
// Timing constants (in milliseconds)
const TickType_t SOIL_INTERVAL_MS = pdMS_TO_TICKS(500);     // Every 500ms
const TickType_t DHT_INTERVAL_MS = pdMS_TO_TICKS(2000);     // Every 2 seconds
const TickType_t LCD_INTERVAL_MS = pdMS_TO_TICKS(500);      // Every 500ms (faster update)
const TickType_t MQTT_INTERVAL_MS = pdMS_TO_TICKS(10000);   // Every 10 seconds

// DHT22 read time is 200ms (simulated)
const int DHT_READ_DURATION_MS = 200;

// MQTT send takes 1500ms (simulated blocking)
const int MQTT_SEND_DURATION_MS = 1500;

// Soil moisture threshold (0 = dry, 1 = wet)
const float DRY_THRESHOLD = 0.30;  // 30% - Pump activates below this

// Pump must activate within 100ms of dry reading
const int PUMP_ACTIVATION_DEADLINE_MS = 100;

// Pump stays on for this many milliseconds
const int PUMP_DURATION_MS = 3000;  // 3 seconds

// ==================== GLOBAL VARIABLES ====================
// Sensor readings (shared data)
volatile float soilMoisture = 0.5;
volatile float temperature = 0;
volatile float humidity = 0;
volatile bool pumpActive = false;
volatile unsigned long lastPumpTriggerTime = 0;
volatile bool soilDryDetected = false;
volatile unsigned long pumpOffTime = 0;

// Synchronization primitives
SemaphoreHandle_t soilMutex;
SemaphoreHandle_t dhtMutex;
SemaphoreHandle_t lcdMutex;

// Task handles
TaskHandle_t soilTaskHandle = NULL;
TaskHandle_t dhtTaskHandle = NULL;
TaskHandle_t pumpTaskHandle = NULL;
TaskHandle_t lcdTaskHandle = NULL;
TaskHandle_t mqttTaskHandle = NULL;

// DHT sensor
DHT dht(DHT_PIN, DHT_TYPE);

// LCD (I2C address 0x27, 16x2)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// MQTT counter
int mqttCounter = 0;

// ==================== FORMATTED OUTPUT FUNCTIONS ====================

void printSeparator(char symbol, int length = 65) {
  for (int i = 0; i < length; i++) {
    Serial.print(symbol);
  }
  Serial.println();
}

void printDoubleSeparator() {
  printSeparator('=', 65);
  printSeparator('=', 65);
}

void printHeader(const char* title) {
  Serial.println();
  printSeparator('=', 65);
  int len = strlen(title);
  int padding = (65 - len) / 2;
  for (int i = 0; i < padding; i++) Serial.print(" ");
  Serial.println(title);
  printSeparator('=', 65);
}

void printSubHeader(const char* title) {
  printSeparator('-', 65);
  Serial.println(title);
  printSeparator('-', 65);
}

void printAligned(const char* label, const char* value, unsigned long time) {
  char buffer[100];
  snprintf(buffer, sizeof(buffer), "%-20s : %-30s @ %6lu ms", label, value, time);
  Serial.println(buffer);
}

void printTaskOutput(const char* task, const char* status, unsigned long time, const char* details = "") {
  char buffer[120];
  snprintf(buffer, sizeof(buffer), "[%-10s] %-30s %6lu ms   %s", task, status, time, details);
  Serial.println(buffer);
}

void printMetric(const char* name, int value, const char* unit, unsigned long time) {
  char buffer[100];
  snprintf(buffer, sizeof(buffer), "%-15s : %3d %-10s @ %6lu ms", name, value, unit, time);
  Serial.println(buffer);
}

void printMetricFloat(const char* name, float value, const char* unit, unsigned long time) {
  char buffer[100];
  snprintf(buffer, sizeof(buffer), "%-15s : %5.1f %-10s @ %6lu ms", name, value, unit, time);
  Serial.println(buffer);
}

// ==================== HELPER FUNCTIONS ====================

void readSoilMoisture() {
  int adcValue = analogRead(SOIL_MOISTURE_PIN);
  // Convert ADC (0-4095) to moisture (0-1)
  // Lower ADC = drier soil (potentiometer at 0)
  float moisture = 1.0 - (adcValue / 4095.0);
  
  // Clamp values between 0 and 1
  if (moisture < 0) moisture = 0;
  if (moisture > 1) moisture = 1;
  
  if (xSemaphoreTake(soilMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    soilMoisture = moisture;
    xSemaphoreGive(soilMutex);
  }
  
  // Check if soil is dry AND pump is not already active
  if (moisture < DRY_THRESHOLD && !pumpActive && !soilDryDetected) {
    soilDryDetected = true;
    lastPumpTriggerTime = millis();
    
    // Immediate alert
    printSeparator('!', 65);
    printAligned("🚨 ALERT", "SOIL DRY DETECTED!", millis());
    printMetric("Moisture", (int)(moisture * 100), "%", millis());
    printMetric("Threshold", (int)(DRY_THRESHOLD * 100), "%", millis());
    printSeparator('!', 65);
  }
}

void activatePump(bool activate) {
  // Update actual LED pin
  digitalWrite(PUMP_PIN, activate ? HIGH : LOW);
  
  // Update pumpActive variable with mutex protection
  if (xSemaphoreTake(soilMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    pumpActive = activate;
    xSemaphoreGive(soilMutex);
  }
  
  if (activate) {
    printSeparator('~', 65);
    printAligned("💧 PUMP", "ACTIVATED - Watering in progress", millis());
    printSeparator('~', 65);
  } else {
    printAligned("💧 PUMP", "DEACTIVATED - Watering stopped", millis());
  }
}

void readDHT22() {
  // Simulate the 200ms read time
  vTaskDelay(pdMS_TO_TICKS(DHT_READ_DURATION_MS));
  
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();
  
  if (isnan(temp) || isnan(hum)) {
    printTaskOutput("DHT22", "❌ READ FAILED", millis(), "Sensor error");
    return;
  }
  
  if (xSemaphoreTake(dhtMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    temperature = temp;
    humidity = hum;
    xSemaphoreGive(dhtMutex);
  }
  
  printTaskOutput("DHT22", "✅ READ SUCCESS", millis());
  printMetricFloat("Temperature", temp, "°C", millis());
  printMetricFloat("Humidity", hum, "%", millis());
}

void updateLCD() {
  if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    lcd.clear();
    
    // Row 0: Soil moisture
    lcd.setCursor(0, 0);
    lcd.print("S:");
    
    float moisture;
    bool pumpState;
    if (xSemaphoreTake(soilMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      moisture = soilMoisture;
      pumpState = pumpActive;
      xSemaphoreGive(soilMutex);
    }
    
    int percent = (int)(moisture * 100);
    lcd.print(percent);
    lcd.print("%");
    
    // Show threshold indicator
    if (moisture < DRY_THRESHOLD) {
      lcd.print(" DRY! ");
    } else {
      lcd.print(" WET  ");
    }
    
    // Row 0 continued: Pump status
    if (pumpState) {
      lcd.print("P:ON");
    } else {
      lcd.print("P:OFF");
    }
    
    // Row 1: Temperature and Humidity
    lcd.setCursor(0, 1);
    lcd.print("T:");
    
    float temp, hum;
    if (xSemaphoreTake(dhtMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      temp = temperature;
      hum = humidity;
      xSemaphoreGive(dhtMutex);
    }
    
    lcd.print(temp, 1);
    lcd.print("C H:");
    lcd.print(hum, 0);
    lcd.print("%");
    
    xSemaphoreGive(lcdMutex);
  }
}

void mqttSimulatedSend() {
  mqttCounter++;
  
  printHeader("☁️  CLOUD LOGGING (MQTT) ☁️");
  printAligned("Transmission #", String(mqttCounter).c_str(), millis());
  printAligned("Status", "STARTING transmission...", millis());
  printAligned("Block time", String(MQTT_SEND_DURATION_MS).c_str(), millis());
  
  // Get current sensor data
  float temp, hum, moisture;
  if (xSemaphoreTake(dhtMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    temp = temperature;
    hum = humidity;
    xSemaphoreGive(dhtMutex);
  }
  if (xSemaphoreTake(soilMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    moisture = soilMoisture;
    xSemaphoreGive(soilMutex);
  }
  
  // Simulate the 1.5 second blocking MQTT send operation
  unsigned long startBlock = millis();
  vTaskDelay(pdMS_TO_TICKS(MQTT_SEND_DURATION_MS));
  unsigned long endBlock = millis();
  
  printAligned("Status", "TRANSMITTING data...", startBlock);
  
  // Print the data being sent
  printSubHeader("📤 DATA SENT TO CLOUD");
  printMetric("Soil Moisture", (int)(moisture * 100), "%", endBlock);
  printMetricFloat("Temperature", temp, "°C", endBlock);
  printMetricFloat("Humidity", hum, "%", endBlock);
  
  printSubHeader("📊 TRANSMISSION STATS");
  printAligned("Actual block time", String(endBlock - startBlock).c_str(), endBlock);
  printAligned("Status", "COMPLETED successfully", endBlock);
  
  printSeparator('=', 65);
}

// ==================== FREERTOS TASKS ====================

// HIGH PRIORITY TASK (Priority 3)
// Reads soil moisture every 500ms
void soilMoistureTask(void *pvParameters) {
  TickType_t lastWakeTime = xTaskGetTickCount();
  const TickType_t interval = SOIL_INTERVAL_MS;
  
  while(1) {
    readSoilMoisture();
    
    float moisture;
    if (xSemaphoreTake(soilMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      moisture = soilMoisture;
      xSemaphoreGive(soilMutex);
    }
    
    int percent = (int)(moisture * 100);
    char details[50];
    
    if (moisture < DRY_THRESHOLD) {
      snprintf(details, sizeof(details), "⚠️  DRY: %3d%% (below %d%%)", percent, (int)(DRY_THRESHOLD * 100));
      printTaskOutput("SOIL", "⚠️  DRY DETECTED", millis(), details);
    } else {
      snprintf(details, sizeof(details), "💧 WET: %3d%% moisture", percent);
      printTaskOutput("SOIL", "✅ READING OK", millis(), details);
    }
    
    vTaskDelayUntil(&lastWakeTime, interval);
  }
}

// HIGH PRIORITY TASK (Priority 3)
// Monitors soil dry condition and activates pump within 100ms
void pumpControlTask(void *pvParameters) {
  while(1) {
    if (soilDryDetected && !pumpActive) {
      unsigned long triggerTime = lastPumpTriggerTime;
      unsigned long currentTime = millis();
      unsigned long responseTime = currentTime - triggerTime;
      
      // Activate pump immediately
      activatePump(true);
      
      // Set when to turn off pump
      pumpOffTime = currentTime + PUMP_DURATION_MS;
      
      // Check if deadline was met (must be within 100ms)
      printHeader("🎯 PUMP RESPONSE VERIFICATION 🎯");
      printAligned("Dry soil detected at", String(triggerTime).c_str(), triggerTime);
      printAligned("Pump activated at", String(currentTime).c_str(), currentTime);
      printAligned("Response time", String(responseTime).c_str(), currentTime);
      
      if (responseTime <= PUMP_ACTIVATION_DEADLINE_MS) {
        printAligned("✅ DEADLINE STATUS", "MET! (≤ 100ms)", currentTime);
      } else {
        printAligned("❌ DEADLINE STATUS", "MISSED! (> 100ms)", currentTime);
      }
      printSeparator('=', 65);
      
      soilDryDetected = false;
    }
    
    // Check if pump should be turned off
    if (pumpActive && millis() >= pumpOffTime) {
      activatePump(false);
    }
    
    // Check every 10ms for immediate response
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// MEDIUM PRIORITY TASK (Priority 2)
// Reads DHT22 every 2 seconds, takes 200ms to read
void dhtReadTask(void *pvParameters) {
  TickType_t lastWakeTime = xTaskGetTickCount();
  const TickType_t interval = DHT_INTERVAL_MS;
  
  while(1) {
    printHeader("🌡️  DHT22 SENSOR READING 🌡️");
    unsigned long startTime = millis();
    readDHT22();
    unsigned long duration = millis() - startTime;
    printAligned("Read duration", String(duration).c_str(), millis());
    printSeparator('=', 65);
    
    vTaskDelayUntil(&lastWakeTime, interval);
  }
}

// LOW PRIORITY TASK (Priority 1)
// Updates LCD display every 500ms
void lcdDisplayTask(void *pvParameters) {
  TickType_t lastWakeTime = xTaskGetTickCount();
  const TickType_t interval = LCD_INTERVAL_MS;
  
  while(1) {
    updateLCD();
    vTaskDelayUntil(&lastWakeTime, interval);
  }
}

// LOWEST PRIORITY TASK (Priority 0)
// Simulates MQTT cloud logging every 10 seconds
void mqttLoggingTask(void *pvParameters) {
  TickType_t lastWakeTime = xTaskGetTickCount();
  const TickType_t interval = MQTT_INTERVAL_MS;
  
  while(1) {
    mqttSimulatedSend();
    vTaskDelayUntil(&lastWakeTime, interval);
  }
}

// ==================== TASK STATISTICS ====================

void printTaskStats() {
  printHeader("📊 TASK STATISTICS 📊");
  
  char buffer[100];
  
  snprintf(buffer, sizeof(buffer), "%-20s | %-10s | %-20s", "Task Name", "Priority", "Stack High Water Mark");
  Serial.println(buffer);
  printSeparator('-', 65);
  
  if (soilTaskHandle) {
    UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(soilTaskHandle);
    snprintf(buffer, sizeof(buffer), "%-20s | %-10d | %-20u", "Soil Moisture", 3, highWaterMark);
    Serial.println(buffer);
  }
  
  if (pumpTaskHandle) {
    UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(pumpTaskHandle);
    snprintf(buffer, sizeof(buffer), "%-20s | %-10d | %-20u", "Pump Control", 3, highWaterMark);
    Serial.println(buffer);
  }
  
  if (dhtTaskHandle) {
    UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(dhtTaskHandle);
    snprintf(buffer, sizeof(buffer), "%-20s | %-10d | %-20u", "DHT22 Read", 2, highWaterMark);
    Serial.println(buffer);
  }
  
  if (lcdTaskHandle) {
    UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(lcdTaskHandle);
    snprintf(buffer, sizeof(buffer), "%-20s | %-10d | %-20u", "LCD Display", 1, highWaterMark);
    Serial.println(buffer);
  }
  
  if (mqttTaskHandle) {
    UBaseType_t highWaterMark = uxTaskGetStackHighWaterMark(mqttTaskHandle);
    snprintf(buffer, sizeof(buffer), "%-20s | %-10d | %-20u", "MQTT Logging", 0, highWaterMark);
    Serial.println(buffer);
  }
  
  printSeparator('=', 65);
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  printDoubleSeparator();
  printHeader("🏭 GREENHOUSE CONTROL SYSTEM 🏭");
  printHeader("Problem #5: Smart Agriculture");
  printHeader("FreeRTOS Real-Time Scheduling");
  printDoubleSeparator();
  
  // Initialize pins
  pinMode(SOIL_MOISTURE_PIN, INPUT);
  pinMode(PUMP_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);  // Ensure pump starts OFF
  
  // Initialize DHT22
  dht.begin();
  
  // Initialize LCD
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Greenhouse Ctrl");
  lcd.setCursor(0, 1);
  lcd.print("System Ready");
  
  // Create mutexes for shared resources
  soilMutex = xSemaphoreCreateMutex();
  dhtMutex = xSemaphoreCreateMutex();
  lcdMutex = xSemaphoreCreateMutex();
  
  printSubHeader("📌 SYSTEM CONFIGURATION");
  char buffer[50];
  snprintf(buffer, sizeof(buffer), "Every 500ms (Priority 3) - Threshold: %d%%", (int)(DRY_THRESHOLD * 100));
  printAligned("Soil moisture reading", buffer, millis());
  snprintf(buffer, sizeof(buffer), "Within %dms (Priority 3)", PUMP_ACTIVATION_DEADLINE_MS);
  printAligned("Pump activation deadline", buffer, millis());
  printAligned("DHT22 reading", "Every 2 sec, takes 200ms (Priority 2)", millis());
  printAligned("LCD update", "Every 500ms (Priority 1)", millis());
  printAligned("MQTT logging", "Every 10 sec, blocks 1.5 sec (Priority 0)", millis());
  
  printSubHeader("🔧 SCHEDULING SOLUTION");
  printAligned("1.", "MQTT at LOWEST priority (0) - cannot block critical tasks", millis());
  printAligned("2.", "Pump control at HIGHEST priority (3) - immediate response", millis());
  printAligned("3.", "Proper mutex usage prevents priority inversion", millis());
  printAligned("4.", "Soil dry detection triggers pump within 100ms", millis());
  printAligned("5.", "LCD and LED now synchronized properly", millis());
  
  // Create FreeRTOS tasks with different priorities
  // Priority 3 (Highest) - Soil moisture reading (critical)
  xTaskCreatePinnedToCore(
    soilMoistureTask,
    "SoilMoisture",
    4096,
    NULL,
    3,
    &soilTaskHandle,
    0
  );
  
  // Priority 3 (Highest) - Pump control (must meet 100ms deadline)
  xTaskCreatePinnedToCore(
    pumpControlTask,
    "PumpControl",
    4096,
    NULL,
    3,
    &pumpTaskHandle,
    0
  );
  
  // Priority 2 (Medium) - DHT22 reading
  xTaskCreatePinnedToCore(
    dhtReadTask,
    "DHT22Read",
    4096,
    NULL,
    2,
    &dhtTaskHandle,
    1
  );
  
  // Priority 1 (Low) - LCD display
  xTaskCreatePinnedToCore(
    lcdDisplayTask,
    "LCDDisplay",
    4096,
    NULL,
    1,
    &lcdTaskHandle,
    1
  );
  
  // Priority 0 (Lowest/Idle) - MQTT logging
  xTaskCreatePinnedToCore(
    mqttLoggingTask,
    "MQTTLogging",
    8192,
    NULL,
    0,
    &mqttTaskHandle,
    1
  );
  
  printSubHeader("✅ TASKS CREATED SUCCESSFULLY");
  printAligned("Soil Moisture Task", "Priority 3 - Core 0", millis());
  printAligned("Pump Control Task", "Priority 3 - Core 0", millis());
  printAligned("DHT22 Read Task", "Priority 2 - Core 1", millis());
  printAligned("LCD Display Task", "Priority 1 - Core 1", millis());
  printAligned("MQTT Logging Task", "Priority 0 - Core 1", millis());
  
  printHeader("🚀 SYSTEM READY - MONITORING STARTED 🚀");
  Serial.println();
  Serial.println("NOTE: Pump activates when soil moisture drops below 30%");
  Serial.println("      Pump stays ON for 3 seconds, then automatically OFF");
  Serial.println();
}

// ==================== LOOP ====================
void loop() {
  // Print task statistics every 30 seconds
  static unsigned long lastStatsTime = 0;
  unsigned long currentTime = millis();
  
  if (currentTime - lastStatsTime >= 30000) {
    printTaskStats();
    lastStatsTime = currentTime;
  }
  
  // Yield to let FreeRTOS scheduler run
  vTaskDelay(pdMS_TO_TICKS(1000));
}
