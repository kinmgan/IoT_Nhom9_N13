// ===== Core / Sensors =====
#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <DHT.h>
#include <Adafruit_BMP085.h>

// ===== InfluxDB (Hỗ trợ batching/offline) =====
#include <InfluxDbClient.h>
#include <InfluxDbCloud.h> // Dùng nếu bạn kết nối InfluxDB Cloud

// ===== EloquentTinyML (Wrapper cho TFLM + LSTM) =====
#include <tflm_esp32.h>
#include <eloquent_tinyml.h>

// ===== ESP-NOW =====
#include <esp_now.h>

// === User config (Chứa WiFi, InfluxDB tokens) ===
#include "config.h"

// ====== Model (từ file g_model.h) ======
#include "model_v3.h"

// ====== Pins & Sensors ======
#define DHTPIN 21
#define DHTTYPE DHT22
#define HALL_PIN 3
#define I2C_SDA 20
#define I2C_SCL 9

TwoWire I2C(0);
DHT dht(DHTPIN, DHTTYPE);
Adafruit_BMP085 bmp;
bool bmp_ok = false;

// ====== ESP-NOW Receiver MAC (từ giao_tiep_esp_now.ino) ======
uint8_t receiverMAC[] = { 0x84, 0x1F, 0xE8, 0x69, 0xD9, 0xCC };

// ====== UNIFIED DATA STRUCT (20 BYTE - T,H,W,P,PROB) ======
typedef struct {
  float temp_c;
  float humi_pct;
  float wind_kmh;
  float pres_hpa;
  float prob; // Xác suất mưa (0.0 to 1.0)
} UnifiedData;

// ====== InfluxDB Client ======
// Khởi tạo client với thông tin từ config.h
InfluxDBClient client(INFLUXDB_URL, INFLUXDB_ORG, INFLUXDB_BUCKET, INFLUXDB_TOKEN);
// "Point" là một bản ghi dữ liệu. "weather_station" là tên "measurement" (bảng)
Point sensorData("weather_station");

// ====== EloquentTinyML Globals ======
#define ARENA_SIZE 320 * 1024
#define TF_NUM_OPS 13
EXT_RAM_ATTR Eloquent::TF::Sequential<TF_NUM_OPS, ARENA_SIZE>* tfl;

// ====== Feature Buffer (Sequence) ======
static const int N_PAST = 4;
static const int N_FEATURES = 8;
float history_buffer[N_PAST][N_FEATURES];
int buf_idx = 0;
bool buf_full = false;

// ====== Normalization (from your scaler) ======
// T,H,W,P
float scaler_mean[4] = { 23.6524f, 78.9352f, 7.2618f, 1010.8179f };
float scaler_inv_scale[4] = { 0.1751f, 0.0716f, 0.2564f, 0.1346f }; // 1/std

// ====== Wind measurement (shared ISR) ======
volatile uint32_t pulseCount = 0;
unsigned long lastWindMs = 0;
const float CALIBRATION_FACTOR_KMH = 2.4f;
// Wind for 10s interval (avg over ~10 samples of 1s)
float wind_sum_interval = 0.0f;
uint32_t wind_sample_interval = 0;
float wind_kmh_avg = 0.0f;

// ====== Schedule (10s interval for read/send, 15min for predict/DB) ======
const unsigned long MS_INTERVAL = 10UL * 1000UL; // Read sensors, push buffer, send ESP-NOW every 10s
const unsigned long MS_MEASURE_WIND = 1000UL; // Measure wind every 1s
unsigned long lastIntervalMs = 0;
static int step_count = 0;
//const int STEPS_PER_PREDICT = (15UL * 60UL * 1000UL) / MS_INTERVAL; // 90 steps
const int STEPS_PER_PREDICT = 15;

// ====== Decision threshold ======
const float THRESHOLD = 0.60f;

// ====== Math helpers ======
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------- ISR ----------
void IRAM_ATTR hallISR() {
  pulseCount++;
}

// ---------- ESP-NOW Callback (Old API for Arduino ESP32 core 2.0.x) ----------
void OnDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "ESP-NOW Sent OK" : "ESP-NOW Send Fail");
}

// ---------- WiFi + InfluxDB + ESP-NOW ----------
void setupNetwork(const char* ssid, const char* pass) {
  Serial.print("[WiFi] Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, pass);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.print("\n[WiFi] OK. IP: ");
  Serial.println(WiFi.localIP());

  // Config NTP (Rất quan trọng cho InfluxDB để đóng dấu thời gian)
  configTime(7 * 3600, 0, "pool.ntp.org");
  Serial.println("[NTP] Configured.");

  // === INIT ESP-NOW ===
  WiFi.mode(WIFI_STA);
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ESP-NOW] Init Failed");
    while (1) delay(1000);
  }
  esp_now_register_send_cb(OnDataSent);

  // Add peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("[ESP-NOW] Failed to add peer");
    while (1) delay(1000);
  }
  Serial.println("[ESP-NOW] Ready.");

  Serial.println("[InfluxDB] Configuring...");
  // Thêm Tag cố định cho tất cả dữ liệu gửi đi (giống "tên thiết bị")
  sensorData.addTag("device", DEVICE_NAME);

  // === CẤU HÌNH OFFLINE QUAN TRỌNG NHẤT ===
  client.setWriteOptions(WriteOptions().batchSize(5).bufferSize(20));
  client.setHTTPOptions(HTTPOptions().httpReadTimeout(20000));

  // Kiểm tra kết nối
  if (client.validateConnection()) {
    Serial.print("[InfluxDB] Connection OK. Bucket: ");
    Serial.println(INFLUXDB_BUCKET);
  } else {
    Serial.print("[InfluxDB] Connection FAILED: ");
    Serial.println(client.getLastErrorMessage());
  }
}

// ---------- Sensors ----------
void setupSensors() {
  dht.begin();
  I2C.begin(I2C_SDA, I2C_SCL, 100000);
  if (!bmp.begin(BMP085_ULTRAHIGHRES, &I2C)) {
    Serial.println("[BMP180] Not found. Check wiring.");
    bmp_ok = false; // Set flag
  } else {
    Serial.println("[BMP180] OK.");
    bmp_ok = true;
  }
  // Bỏ comment khi bạn kết nối cảm biến Hall
  pinMode(HALL_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(HALL_PIN), hallISR, FALLING);
}

// ---------- EloquentTinyML Setup ----------
void setupTinyML() {
  Serial.println("[TinyML] Initializing...");
  Serial.println("[PSRAM] Allocating 320KB Arena...");
  tfl = new Eloquent::TF::Sequential<TF_NUM_OPS, ARENA_SIZE>();
  if (tfl == nullptr) {
    Serial.println("[PSRAM] Allocation failed!");
    while (true) delay(1000);
  }
  Serial.println("[PSRAM] OK.");
  tfl->setNumInputs(N_PAST * N_FEATURES);
  tfl->setNumOutputs(1);
  // Auto register LSTM ops
  tfl->resolver.AddUnidirectionalSequenceLSTM();
  tfl->resolver.AddAdd();
  tfl->resolver.AddFill();
  tfl->resolver.AddFullyConnected();
  tfl->resolver.AddLogistic();
  tfl->resolver.AddMul();
  tfl->resolver.AddPack();
  tfl->resolver.AddShape();
  tfl->resolver.AddSplit();
  tfl->resolver.AddStridedSlice();
  tfl->resolver.AddTanh();
  tfl->resolver.AddTranspose();
  tfl->resolver.AddUnpack();
  while (!tfl->begin(g_model).isOk()) {
    Serial.println(tfl->exception.toString());
    delay(1000);
  }
  Serial.println("[TinyML] LSTM ready.");
}

// ---------- Time features (Offline Estimation) ----------
void getTimeFeatures(float& hour_sin, float& hour_cos, float& month_sin, float& month_cos) {
  struct tm tinfo;
  if (!getLocalTime(&tinfo)) {
    // Default nếu chưa sync bao giờ
    tinfo.tm_hour = 12;
    tinfo.tm_mon = 0; // Trưa, tháng 1
    Serial.println("[Time] Using default time.");
  }
  float hour = (float)tinfo.tm_hour;
  float month = (float)(tinfo.tm_mon + 1);
  hour_sin = sinf(2.0f * M_PI * hour / 24.0f);
  hour_cos = cosf(2.0f * M_PI * hour / 24.0f);
  month_sin = sinf(2.0f * M_PI * month / 12.0f);
  month_cos = cosf(2.0f * M_PI * month / 12.0f);
}

// ---------- Wind every 1s (accumulate for 10s avg) ----------
void measureWindSpeed(unsigned long nowMs) {
  if (nowMs - lastWindMs < MS_MEASURE_WIND) return;
  lastWindMs = nowMs;
  noInterrupts();
  uint32_t pulses = pulseCount;
  pulseCount = 0;
  interrupts();
  float rps = (float)pulses;
  float wind_kmh_instant = rps * CALIBRATION_FACTOR_KMH;
  // Accumulate for interval avg
  wind_sum_interval += wind_kmh_instant;
  wind_sample_interval++;
}

// ---------- Push 1 record into ring buffer ----------
void pushRecord(float normT, float normH, float normW, float normP,
                float hour_sin, float hour_cos, float month_sin, float month_cos) {
  history_buffer[buf_idx][0] = normT;
  history_buffer[buf_idx][1] = normH;
  history_buffer[buf_idx][2] = normW;
  history_buffer[buf_idx][3] = normP;
  history_buffer[buf_idx][4] = hour_sin;
  history_buffer[buf_idx][5] = hour_cos;
  history_buffer[buf_idx][6] = month_sin;
  history_buffer[buf_idx][7] = month_cos;
  buf_idx = (buf_idx + 1) % N_PAST;
  if (buf_idx == 0) buf_full = true;
}

// ---------- Inference ----------
float runInference(float* seq) {
  if (!tfl->predict(seq).isOk()) {
    Serial.println(tfl->exception.toString());
    return -1.0f;
  }
  float prob = tfl->outputs[0];
  if (isnan(prob) || prob < 0.0f || prob > 1.0f) {
    Serial.println("[TinyML] Invalid prob.");
    return -1.0f;
  }
  return prob;
}

// === GỬI DỮ LIỆU THỐNG NHẤT (20 BYTE - T,H,W,P,PROB) ===
void sendUnifiedData(float t, float h, float w, float p, float prob) {
  UnifiedData data_out;
  data_out.temp_c = t;
  data_out.humi_pct = h;
  data_out.wind_kmh = w;
  data_out.pres_hpa = p;
  data_out.prob = prob;
  esp_now_send(receiverMAC, (uint8_t*)&data_out, sizeof(data_out));
  Serial.println("[ESP-NOW] Unified data sent (T,H,W,P,prob).");
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  delay(200);
  setupNetwork(WIFI_SSID, WIFI_PASSWORD);
  setupSensors();
  setupTinyML();
  lastIntervalMs = millis();
}

// ---------- Main loop ----------
void loop() {
  unsigned long now = millis();

  // 1. Đo gió mỗi giây (accumulate for 10s avg)
  measureWindSpeed(now);

  // 2. Read sensors, send, push buffer every 10s
  if (now - lastIntervalMs >= MS_INTERVAL) {
    lastIntervalMs = now;

    // Calc wind avg over last ~10s
    if (wind_sample_interval > 0) {
      wind_kmh_avg = wind_sum_interval / (float)wind_sample_interval;
    } else {
      wind_kmh_avg = 0.0f;
    }
    wind_sum_interval = 0.0f;
    wind_sample_interval = 0;

    // Đọc cảm biến thật (bỏ fake)
    float h = dht.readHumidity();
    float t = dht.readTemperature();
    float p_hpa;
    if (bmp_ok) {
      p_hpa = bmp.readPressure() / 100.0f;
    } else {
      p_hpa = 1013.25f; // Default sea-level pressure (hPa)
      Serial.println("[BMP180] Using default pressure: 1013.25 hPa");
    }

    // Kiểm tra lỗi đọc (thêm nếu cần)
    if (isnan(h) || isnan(t) || isnan(p_hpa)) {
      Serial.println("[Sensors] Read error, skipping.");
      return;
    }

    // Log ra Serial
    Serial.print("[Sensors] Temp: ");
    Serial.print(t, 2);
    Serial.print("°C, Humi: ");
    Serial.print(h, 2);
    Serial.print("%, Wind Avg(10s): ");
    Serial.print(wind_kmh_avg, 2);
    Serial.print(" km/h, Press: ");
    Serial.print(p_hpa, 2);
    Serial.println(" hPa");

    // Chuẩn hóa dữ liệu
    float hsin, hcos, msin, mcos;
    getTimeFeatures(hsin, hcos, msin, mcos);
    float nT = (t - scaler_mean[0]) * scaler_inv_scale[0];
    float nH = (h - scaler_mean[1]) * scaler_inv_scale[1];
    float nW = (wind_kmh_avg - scaler_mean[2]) * scaler_inv_scale[2];
    float nP = (p_hpa - scaler_mean[3]) * scaler_inv_scale[3];

    // Đẩy vào buffer (sequence cho predict)
    pushRecord(nT, nH, nW, nP, hsin, hcos, msin, mcos);
    if (!buf_full) {
      Serial.println("[Seq] Buffer not full yet.");
    }

    // Gửi unified data every 10s (prob=0)
    sendUnifiedData(t, h, wind_kmh_avg, p_hpa, 0.0f);

    // 3. Predict every 15min (90 steps)
    step_count++;
    if (step_count % STEPS_PER_PREDICT == 0 && buf_full) {
      Serial.println("[Predict] Running inference...");

      // Chuẩn bị sequence cho mô hình
      float seq[N_PAST * N_FEATURES];
      int idx = 0;
      for (int k = 0; k < N_PAST; ++k) {
        int pos = (buf_idx + k) % N_PAST;
        for (int f = 0; f < N_FEATURES; ++f) {
          seq[idx++] = history_buffer[pos][f];
        }
      }

      // Chạy dự báo
      float prob = runInference(seq);
      if (prob < 0.0f) {
        Serial.println("[Predict] Inference failed.");
        return;
      }
      Serial.print("[Predict] Rain prob = ");
      Serial.println(prob, 4);

      // Ra quyết định
      bool retract = (prob > THRESHOLD);
      if (retract) {
        Serial.println("[ACTION] RETRACT!");
      } else {
        Serial.println("[ACTION] NO RAIN.");
      }

      // Gửi unified data với prob thực
      sendUnifiedData(t, h, wind_kmh_avg, p_hpa, prob);

      // === GHI LOG VÀO INFLUXDB (chỉ current values + prob) ===
      sensorData.clearFields();
      sensorData.addField("prob", prob);
      sensorData.addField("temp_c", t);
      sensorData.addField("humi", h);
      sensorData.addField("wind_kmh", wind_kmh_avg);
      sensorData.addField("pres_hpa", p_hpa);
      sensorData.setTime(time(nullptr));
      Serial.print("[InfluxDB] Writing point... ");
      if (!client.writePoint(sensorData)) {
        Serial.print("FAILED: ");
        Serial.println(client.getLastErrorMessage());
      } else {
        Serial.println("OK (queued).");
      }
    }
  }
}