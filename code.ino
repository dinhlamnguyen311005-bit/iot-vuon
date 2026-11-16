#include <WiFi.h>
#include <PubSubClient.h>
#include <DHT.h>

// Cấu hình chân phần cứng khớp với diagram.json
#define DHTPIN 4
#define DHTTYPE DHT22
#define PUMP_PIN 18
#define FAN_PIN 19
#define MIST_PIN 21

// Thông tin Wi-Fi và MQTT Broker
const char* ssid = "Wokwi-GUEST";
const char* password = "";
const char* mqtt_server = "broker.hivemq.com";

WiFiClient espClient;
PubSubClient client(espClient);
DHT dht(DHTPIN, DHTTYPE);

// Cấu trúc dữ liệu gửi qua hàng đợi dataQueue
struct SensorData {
  float temperature;
  float humidity;
};

// Khai báo FreeRTOS Queues theo yêu cầu đề bài
QueueHandle_t dataQueue;
QueueHandle_t alertQueue;

// Biến trạng thái điều khiển thủ công (Manual) cho Máy bơm và Quạt
bool pumpState = false;
bool fanState = false;

// Hàm callback nhận lệnh điều khiển từ Web/App qua MQTT
void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  if (String(topic) == "garden/pump/control") {
    pumpState = (message == "ON");
    digitalWrite(PUMP_PIN, pumpState ? HIGH : LOW);
    Serial.printf("[MQTT] Máy bơm: %s\n", pumpState ? "BẬT" : "TẮT");
  } 
  else if (String(topic) == "garden/fan/control") {
    fanState = (message == "ON");
    digitalWrite(FAN_PIN, fanState ? HIGH : LOW);
    Serial.printf("[MQTT] Quạt gió: %s\n", fanState ? "BẬT" : "TẮT");
  }
}

// =================================================================
// 1. "Task truyền data": Kết nối Wi-Fi/MQTT, đọc data Queue, gửi lên Broker (Core 0)
// =================================================================
void taskTransmission(void *pvParameters) {
  // Kết nối Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
  }
  Serial.println("[Core 0] Đã kết nối Wi-Fi thành công!");

  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);

  SensorData receivedData;

  while (true) {
    // Duy trì kết nối MQTT Broker
    if (!client.connected()) {
      while (!client.connected()) {
        String clientId = "ESP32GardenClient-" + String(random(0xffff), HEX);
        if (client.connect(clientId.c_str())) {
          client.subscribe("garden/pump/control");
          client.subscribe("garden/fan/control");
          Serial.println("[Core 0] Đã kết nối MQTT Broker!");
        } else {
          vTaskDelay(2000 / portTICK_PERIOD_MS);
        }
      }
    }
    client.loop();

    // Lấy dữ liệu từ hàng đợi "data Queue" và publish lên MQTT Broker
    if (xQueueReceive(dataQueue, &receivedData, portMAX_DELAY) == pdPASS) {
      String payload = "{\"temperature\": " + String(receivedData.temperature) + 
                       ", \"humidity\": " + String(receivedData.humidity) + "}";
      client.publish("garden/sensor/data", payload.c_str());
    }
  }
}

// =================================================================
// 2. "Task đọc data": Đọc cảm biến DHT22, in serial, gửi vào dataQueue & dataAlert (Core 1)
// =================================================================
void taskReadSensor(void *pvParameters) {
  dht.begin();
  SensorData data;
  float alertHumidity;

  while (true) {
    data.humidity = dht.readHumidity();
    data.temperature = dht.readTemperature();

    // Kiểm tra dữ liệu hợp lệ từ DHT22
    if (!isnan(data.humidity) && !isnan(data.temperature)) {
      // Hiển thị dữ liệu qua giao tiếp serial
      Serial.printf("[Core 1 - Sensor] Nhiệt độ: %.1f°C | Độ ẩm: %.1f%%\n", data.temperature, data.humidity);

      // Gửi dữ liệu đầy đủ vào hàng đợi "data Queue"
      xQueueSend(dataQueue, &data, portMAX_DELAY);

      // Gửi dữ liệu độ ẩm vào hàng đợi "data Alert" cho task cảnh báo
      alertHumidity = data.humidity;
      xQueueSend(alertQueue, &alertHumidity, portMAX_DELAY);
    } else {
      Serial.println("[Core 1 - Sensor] Lỗi đọc cảm biến DHT22!");
    }

    vTaskDelay(2000 / portTICK_PERIOD_MS); // Chu kỳ đọc mỗi 2 giây
  }
}

// =================================================================
// 3. "Task cảnh báo": Đọc data Alert, điều khiển phun sương theo thuật toán (Core 1)
// =================================================================
void taskAlertControl(void *pvParameters) {
  float hum;
  
  // Cấu hình PWM chân phun sương theo chuẩn ESP32 Core mới
  ledcAttach(MIST_PIN, 5000, 8); // Tần số 5kHz, độ phân giải 8-bit (0-255)

  while (true) {
    // Đọc dữ liệu từ hàng đợi "data Alert"
    if (xQueueReceive(alertQueue, &hum, portMAX_DELAY) == pdPASS) {
      
      // Thuật toán cho cây công nghiệp (cà phê, hồ tiêu - độ ẩm ổn định 40%-60%)
      if (hum < 30.0) {
        // Dưới 30%: Bật 100% công suất (255), trong 30 giây
        ledcWrite(MIST_PIN, 255);
        Serial.println(">>> [CẢNH BÁO] Độ ẩm < 30%! Phun sương 100% công suất trong 30s.");
        vTaskDelay(30000 / portTICK_PERIOD_MS);
      } 
      else if (hum >= 30.0 && hum < 35.0) {
        // Từ 30% - 35%: Bật 50% công suất (128), trong 20 giây
        ledcWrite(MIST_PIN, 128);
        Serial.println(">>> [CẢNH BÁO] Độ ẩm 30%-35%. Phun sương 50% công suất trong 20s.");
        vTaskDelay(20000 / portTICK_PERIOD_MS);
      } 
      else if (hum >= 35.0 && hum < 40.0) {
        // Từ 35% - 40%: Bật 25% công suất (64), trong 10 giây
        ledcWrite(MIST_PIN, 64);
        Serial.println(">>> [CẢNH BÁO] Độ ẩm 35%-40%. Phun sương 25% công suất trong 10s.");
        vTaskDelay(10000 / portTICK_PERIOD_MS);
      } 
      else {
        // Độ ẩm từ 40% - 60% trở lên (Mức tối ưu): Tắt máy phun sương
        ledcWrite(MIST_PIN, 0);
      }
    }
  }
}

// =================================================================
// SETUP & LOOP (Khởi tạo hệ thống RTOS)
// =================================================================
void setup() {
  Serial.begin(115200);

  // Cấu hình chân thiết bị thủ công
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(FAN_PIN, OUTPUT);
  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(FAN_PIN, LOW);

  // Khởi tạo các Hàng đợi (Queues) trong FreeRTOS
  dataQueue = xQueueCreate(10, sizeof(SensorData));
  alertQueue = xQueueCreate(10, sizeof(float));

  // Tạo "Task truyền data" gán chạy trên Core 0
  xTaskCreatePinnedToCore(
    taskTransmission,   
    "TaskTransmission",  
    4096,                
    NULL,                
    1,                   
    NULL,                
    0                    // Core 0
  );

  // Tạo "Task đọc data" gán chạy trên Core 1
  xTaskCreatePinnedToCore(
    taskReadSensor,
    "TaskReadSensor",
    4096,
    NULL,
    2,
    NULL,
    1                    // Core 1
  );

  // Tạo "Task cảnh báo" gán chạy trên Core 1
  xTaskCreatePinnedToCore(
    taskAlertControl,
    "TaskAlertControl",
    4096,
    NULL,
    2,
    NULL,
    1                    // Core 1
  );
}

void loop() {
  // FreeRTOS đã quản lý đa nhiệm hoàn toàn trên các Task, vòng lặp chính để trống.
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}