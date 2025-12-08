#define BLYNK_TEMPLATE_ID "TMPL6Qkr6crlP"
#define BLYNK_TEMPLATE_NAME "Gian Phoi Thong Minh"
#define BLYNK_AUTH_TOKEN "CAr5FOa2VBRHc9I8rYQzULtaSeKVDRTC"

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <esp_now.h>
#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_BMP085.h>

char ssid[] = "BETWEEN COFFEE T2";
char pass[] = "88888888";

// === MAC ESP32-S3 (BÊN NHẬN MƯA) ===
uint8_t s3MAC[] = {0x80, 0xB5, 0x4E, 0xC6, 0x8B, 0x64}; // Thay bằng MAC thật

// === DỮ LIỆU NHẬN TỪ ESP32-S3 (20 BYTE) ===
typedef struct {
    float temp_c;
    float humi_pct;
    float wind_kmh;
    float pres_hpa;
    uint8_t autoMode;
    uint8_t padding[3];
} UnifiedData;

UnifiedData receivedData;

// === DỮ LIỆU GỬI MƯA CHO ESP32-S3 ===
typedef struct {
    uint8_t isRaining; // 0 = Không mưa, 1 = Mưa
    uint8_t padding[3];
} RainData;

int mode = 0;  // 0 = Thủ công, 1 = Tự động
int blynkControlState = 0;
BlynkTimer timer;
Adafruit_BMP085 bmp;

// === CHÂN KẾT NỐI ===
#define IN1 26
#define IN2 27
#define ENA 25
#define BTN_UP 32
#define BTN_DOWN 33
#define LIMIT1 14
#define LIMIT2 15
#define RAIN_SENSOR_PIN 17
#define LIGHT_SENSOR_PIN 4

int motorSpeed = 170;
bool initialRun = true;

// === HÀM HỖ TRỢ ===
void stopMotor();
void moveIn();
void moveOut();
void updateTerminal();

// === GỬI TRẠNG THÁI MƯA ===
void sendRainStatus(bool raining) {
    RainData rainData;
    rainData.isRaining = raining ? 1 : 0;
    memset(rainData.padding, 0, 3);

    esp_now_send(s3MAC, (uint8_t*)&rainData, sizeof(rainData));
    Serial.println("Gửi mưa sang S3: " + String(raining ? "MƯA!" : "KHÔNG MƯA"));
}

// === NHẬN DỮ LIỆU TỪ ESP32-S3 ===
void OnDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
    if (len == sizeof(UnifiedData)) {
        memcpy(&receivedData, data, sizeof(receivedData));

        Serial.printf("ESP-NOW: T=%.1f°C | H=%.1f%% | W=%.1f km/h | P=%.1f hPa | AUTO=%d\n",
                      receivedData.temp_c, receivedData.humi_pct,
                      receivedData.wind_kmh, receivedData.pres_hpa,
                      receivedData.autoMode);

        // Gửi dữ liệu lên Blynk
        Blynk.virtualWrite(V2, receivedData.temp_c);
        Blynk.virtualWrite(V3, receivedData.humi_pct);
        Blynk.virtualWrite(V8, receivedData.wind_kmh);
        Blynk.virtualWrite(V11, receivedData.pres_hpa);
        Blynk.virtualWrite(V9, receivedData.autoMode ? "THU VÀO" : "KÉO RA");

        // Tự động theo dự báo
        if (mode == 1) {
            if (receivedData.autoMode == 1 && !digitalRead(LIMIT2)) {
                moveIn();
                Serial.println("AUTO (ESP-NOW): THU VÀO (dự báo mưa)");
            } else if (receivedData.autoMode == 0 && !digitalRead(LIMIT1)) {
                moveOut();
                Serial.println("AUTO (ESP-NOW): KÉO RA (không mưa)");
            }
        }

        updateTerminal();
    } else {
        Serial.printf("ESP-NOW: Sai kích thước! Nhận %d byte, mong đợi %d\n", len, sizeof(UnifiedData));
    }
}

// === CHẾ ĐỘ (V1) ===
BLYNK_WRITE(V1) {
    mode = param.asInt();
    initialRun = true;
    Serial.println("Chế độ: " + String(mode == 0 ? "Thủ công" : "Tự động"));
    stopMotor();
    blynkControlState = 0;
    Blynk.virtualWrite(V1, mode);
}

// === LỆNH (V0) ===
BLYNK_WRITE(V0) {
    int action = param.asInt();
    if (mode == 0) {
        if (action == 0) {
            blynkControlState = 1;
            Serial.println("Blynk: THU VÀO");
        } else if (action == 1) {
            blynkControlState = 2;
            Serial.println("Blynk: KÉO RA");
        } else {
            blynkControlState = 0;
        }
    } else {
        blynkControlState = 0;
    }
}

void setup() {
    Serial.begin(115200);

    WiFi.mode(WIFI_STA);
    Serial.print("MAC Address: ");
    Serial.println(WiFi.macAddress());

    if (esp_now_init() != ESP_OK) {
        Serial.println("Lỗi khởi tạo ESP-NOW");
        while (1);
    }
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, s3MAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;

    if (esp_now_add_peer(&peerInfo) != ESP_OK) {
        Serial.println("Lỗi thêm peer S3");
        while (1);
    }

    Blynk.begin(BLYNK_AUTH_TOKEN, ssid, pass);

    pinMode(IN1, OUTPUT);
    pinMode(IN2, OUTPUT);
    pinMode(ENA, OUTPUT);
    pinMode(BTN_UP, INPUT_PULLUP);
    pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(LIMIT1, INPUT_PULLUP);
    pinMode(LIMIT2, INPUT_PULLUP);
    pinMode(RAIN_SENSOR_PIN, INPUT);
    pinMode(LIGHT_SENSOR_PIN, INPUT);

    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    analogWrite(ENA, 0);
    analogWriteFrequency(ENA, 1000);

    if (!bmp.begin()) {
        Serial.println("Không tìm thấy BMP180!");
    } else {
        Serial.println("BMP180 sẵn sàng!");
    }

    timer.setInterval(30000L, sensorCheck);
    Serial.println("Hệ thống sẵn sàng!");
}

void sensorCheck() {
    if (mode == 1) {
        bool isRaining = (digitalRead(RAIN_SENSOR_PIN) == LOW);
        bool isDark = (digitalRead(LIGHT_SENSOR_PIN) == LOW);
        Serial.println(isRaining ? "TRỜI MƯA!" : "TRỜI KHÔNG MƯA");
        Serial.println(isDark ? "TRỜI TỐI" : "TRỜI SÁNG");
    }
}

// === TERMINAL HIỂN THỊ MƯA + ÁNH SÁNG ===
void updateTerminal() {
    static unsigned long lastUpdate = 0;
    if (millis() - lastUpdate < 1000) return;
    lastUpdate = millis();

    bool isRaining = (digitalRead(RAIN_SENSOR_PIN) == LOW);
    bool isDark = (digitalRead(LIGHT_SENSOR_PIN) == LOW);

    unsigned long s = millis() / 1000;
    int hh = (s / 3600) % 24;
    int mm = (s / 60) % 60;
    int ss = s % 60;

    char buffer[100];
    snprintf(buffer, sizeof(buffer),
             "[%02d:%02d:%02d] %s | %s",
             hh, mm, ss,
             isRaining ? "MƯA!" : "KHÔ",
             isDark ? "TỐI" : "SÁNG");

    Blynk.virtualWrite(V10, buffer);
    Blynk.virtualWrite(V10, "\n");
}

void loop() {
    Blynk.run();
    timer.run();

    bool upPressed = !digitalRead(BTN_UP);
    bool downPressed = !digitalRead(BTN_DOWN);
    bool limit1Hit = digitalRead(LIMIT1);
    bool limit2Hit = digitalRead(LIMIT2);

    static bool isRaining = false;
    static bool isDark = false;

    static unsigned long lastSensorRead = 0;
    if (millis() - lastSensorRead > 1000 && mode == 1) {
        lastSensorRead = millis();
        isRaining = (digitalRead(RAIN_SENSOR_PIN) == LOW);
        isDark = (digitalRead(LIGHT_SENSOR_PIN) == LOW);
    }

    // === CHẾ ĐỘ THỦ CÔNG ===
    if (mode == 0) {
        bool isMoving = false;

        if (upPressed && !downPressed && !limit1Hit) {
            moveIn();
            isMoving = true;
            Serial.println("Thủ công (nút): THU VÀO");
        } else if (downPressed && !upPressed && !limit2Hit) {
            moveOut();
            isMoving = true;
            Serial.println("Thủ công (nút): KÉO RA");
        } else if (!upPressed && !downPressed) {
            if (blynkControlState == 1 && !limit2Hit) {
                moveIn();
                isMoving = true;
                Serial.println("Thủ công (Blynk): THU VÀO");
            } else if (blynkControlState == 2 && !limit1Hit) {
                moveOut();
                isMoving = true;
                Serial.println("Thủ công (Blynk): KÉO RA");
            }
        }

        if (!isMoving) {
            stopMotor();
        } else if ((blynkControlState == 1 && limit2Hit) ||
                   (blynkControlState == 2 && limit1Hit) ||
                   (upPressed && limit1Hit) ||
                   (downPressed && limit2Hit)) {
            stopMotor();
            Serial.println("Thủ công: DỪNG (giới hạn)");
            blynkControlState = 0;
        }
    }

    // === CHẾ ĐỘ TỰ ĐỘNG ===
    else {
        blynkControlState = 0;

        if (initialRun && !limit1Hit) {
            moveOut();
            Serial.println("Tự động: KÉO RA (khởi động)");
        } else if ((isRaining || isDark) && !limit2Hit) {
            moveIn();
            Serial.println("Tự động: THU VÀO (mưa/tối)");
            initialRun = false;
        } else if (!isRaining && !isDark && !limit1Hit) {
            moveOut();
            Serial.println("Tự động: KÉO RA (sáng, khô)");
            initialRun = false;
        } else {
            stopMotor();
            Serial.println("Tự động: DỪNG");
            initialRun = false;
        }
    }

    // === GỬI MƯA MỖI 1 GIÂY ===
    static unsigned long lastRainSend = 0;
    if (millis() - lastRainSend > 1000) {
        lastRainSend = millis();
        bool isRaining = (digitalRead(RAIN_SENSOR_PIN) == LOW);
        sendRainStatus(isRaining);
    }

    updateTerminal();
    delay(100);
}

// === ĐIỀU KHIỂN MOTOR ===
void stopMotor() {
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, LOW);
    analogWrite(ENA, 0);
}

void moveIn() {
    digitalWrite(IN1, LOW);
    digitalWrite(IN2, HIGH);
    analogWrite(ENA, motorSpeed);
}

void moveOut() {
    digitalWrite(IN1, HIGH);
    digitalWrite(IN2, LOW);
    analogWrite(ENA, motorSpeed);
}
