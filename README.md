# Hệ thống Giàn Phơi Thông Minh Tích Hợp AI (Smart Clothesline System)

## 1. Tổng quan bài toán
Dự án "Giàn Phơi Thông Minh" giải quyết vấn đề tự động thu/phơi quần áo dựa trên điều kiện thời tiết thực tế và dự báo sớm về khả năng có mưa. Khác với các hệ thống truyền thống chỉ hoạt động thụ động dựa vào cảm biến nước mưa (khiến quần áo bị ướt một phần trước khi được thu vào), hệ thống này kết hợp mô hình học máy (Machine Learning) để **dự đoán xác suất mưa** thông qua việc giám sát các thông số môi trường (Nhiệt độ, Độ ẩm, Vận tốc gió, Áp suất không khí). Từ đó, hệ thống có khả năng đưa ra quyết định thu giàn phơi chủ động *trước khi* trời thực sự mưa.

## 2. Sơ đồ kiến trúc hệ thống
Hệ thống được thiết kế theo kiến trúc phân tán gồm 2 Node, giao tiếp không dây với nhau qua giao thức độ trễ thấp ESP-NOW:
- **Node A (Trạm quan trắc & AI dự đoán):** Đóng vai trò thu thập dữ liệu môi trường, chạy mô hình học máy cục bộ (TinyML) để đưa ra dự báo và đẩy log lên cơ sở dữ liệu đám mây (InfluxDB).
- **Node B (Trạm điều khiển trung tâm):** Chịu trách nhiệm trực tiếp điều khiển động cơ giàn phơi, đọc các cảm biến vật lý tức thời (cảm biến mưa, ánh sáng, công tắc hành trình) và kết nối với nền tảng đám mây Blynk để cung cấp giao diện cho người dùng.

![Sơ đồ kiến trúc](.github/assets/so%20do%20kien%20truc.jpg)

## 3. Từ Data đến Model và Tích hợp phần cứng
Quá trình xây dựng và đưa mô hình trí tuệ nhân tạo xuống thiết bị IoT biên (Edge AI) được thực hiện qua các bước:

**A. Xử lý Dữ liệu (Data Pipeline):**
Dữ liệu gốc (`weather_hanoi_final.csv`) được lấy từ Open-Meteo API, chứa các thông số thời tiết lịch sử tại **Hà Nội** (cùng khu vực địa lý với nơi triển khai thiết bị vật lý nhằm tối ưu độ chính xác của mô hình dự báo). Quá trình tiền xử lý được thực hiện thông qua `clean_data.ipynb` với các bước kỹ thuật cốt lõi:
- **Chuẩn hóa tham số (Data Cleansing):** Làm sạch tên cột, loại bỏ ký tự đặc biệt, định dạng cột `time` thành chuỗi thời gian tiêu chuẩn và giữ lại 4 feature quan sát chính: Nhiệt độ (°C), Độ ẩm (%), Vận tốc gió (km/h) và Áp suất khí quyển (hPa).
- **Trích xuất đặc trưng chu kỳ (Cyclical Feature Engineering):** Biến đổi tham số thời gian thành các đặc trưng lượng giác (`hour_sin`, `hour_cos`, `month_sin`, `month_cos`) để mô hình học máy (LSTM) dễ dàng nắm bắt được tính tuần hoàn liên tục của ngày và mùa.
- **Định dạng nhãn mục tiêu (Label Generation & Time Shifting):** Dựa vào cột lượng mưa, hệ thống gán nhãn nhị phân cho sự kiện có mưa hay không, sau đó áp dụng phép toán dịch chuyển (shift -1) để sinh ra nhãn mục tiêu `will_rain_next_15min` (Dự báo trời có mưa trong 15 phút kế tiếp).
- Lọc bỏ các biến trung gian và xử lý dữ liệu khuyết (NaN), trích xuất ra tập dữ liệu huấn luyện cuối cùng (`weather_hanoi_cleaned.csv`).

**B. Huấn luyện Mô hình (Model Training):**
- Sử dụng mạng nơ-ron hồi quy tuần tự **LSTM** (`Final_model_LSTM.ipynb`) để trích xuất các đặc trưng chuỗi thời gian của thời tiết.
- Đầu ra của mô hình là hàm rủi ro với xác suất mưa trải từ `0.0` đến `1.0`.

**C. Tích hợp lên Vi điều khiển (TinyML):**
- Mô hình LSTM sau khi huấn luyện được lượng tử hóa và chuyển sang mảng byte C++ (`model_v3.h`).
- Khởi tạo trên bộ nhớ PSRAM của vi điều khiển ESP32 tại Node A thông qua thư viện `EloquentTinyML` (wrapper của `TensorFlow Lite for Microcontrollers`).

**D. Luồng xử lý thời gian thực:**
- Cụm cảm biến tại Node A (DHT22, BMP180, Cảm biến Hall đo gió) lấy mẫu với chu kỳ 10 giây.
- Dữ liệu thô được chuẩn hóa (Normalization) và đẩy vào ring buffer.
- Mỗi 15 phút, ESP32 sẽ trích xuất sequence từ buffer, cấp cho mô hình LSTM chạy inference. Nếu dự báo xác suất mưa > `60%`, bản tin điều khiển sẽ lập tức được broadcast sang Node B thông qua ESP-NOW.

## 4. Hoạt động thực tế của hệ thống
Hệ thống vận hành theo 2 chế độ độc lập và cho phép chuyển đổi linh hoạt (qua App hoặc vật lý):

- **Chế độ Tự động (Auto Mode):**
  - **Thu vào:** Ưu tiên mức cao nhất. Sẽ thu giàn khi Node A cảnh báo có mưa (xác suất > `60%`), hoặc khi cụm cảm biến cục bộ tại Node B phát hiện có nước mưa thật / cường độ sáng giảm mạnh (trời tối).
  - **Kéo ra:** Chỉ kéo ra khi thỏa mãn đồng thời: trời sáng, cảm biến khô ráo và AI đánh giá không có rủi ro mưa.
- **Chế độ Thủ công (Manual Mode):**
  - Trao toàn quyền cho người dùng. Giàn phơi chỉ di chuyển khi có tín hiệu từ nút nhấn vật lý (UP/DOWN) trên tủ điện hoặc các nút điều khiển trên giao diện Blynk.
- **Hệ thống an toàn (Fail-safe):** Dù ở chế độ nào, Node B luôn giám sát 2 công tắc hành trình (Limit Switch). Động cơ sẽ bị ngắt (phanh) ngay lập tức khi thanh phơi chạm ngưỡng mở tối đa hoặc thu tối đa.

## 5. Hướng dẫn khởi chạy
### Lắp ráp phần cứng
Thực hiện đấu nối linh kiện điện tử cho 2 Node theo sơ đồ Breadboard thực tế dưới đây:

**Node A (Cụm Cảm biến & Vi điều khiển AI):**
![Mạch lắp ráp Node A](.github/assets/node%20A%20breadboard.png)

**Node B (Cụm Điều khiển Động cơ & Nút nhấn):**
![Mạch lắp ráp Node B](.github/assets/node%20B%20breadboard.png)

### Cấu hình phần mềm
1. **Môi trường & Thư viện:** Sử dụng Arduino IDE có cài đặt core ESP32. Đảm bảo đã cài đặt các thư viện: `WiFi`, `InfluxDbClient`, `EloquentTinyML`, `Adafruit_BMP085`, `DHT`, `BlynkSimpleEsp32`.
2. **Setup Node A:** 
   - Cần bổ sung file `config.h` vào thư mục `NODE_A/` chứa thông tin mạng và token (ví dụ: `WIFI_SSID`, `WIFI_PASSWORD`, `INFLUXDB_URL`, `INFLUXDB_TOKEN`).
   - Mở `NODE_A.ino`, chọn board ESP32, kích hoạt phân vùng PSRAM và tiến hành nạp code.
3. **Setup Node B:**
   - Mở `NODE_B.ino`.
   - Khai báo đúng `BLYNK_AUTH_TOKEN`, thông tin WiFi và đặc biệt là cập nhật `s3MAC` (địa chỉ MAC của Node A) để liên kết ESP-NOW hoạt động chính xác.
   - Biên dịch và nạp code xuống thiết bị.

## 6. Giao diện web/app điều khiển (Blynk)
Để tiện lợi cho người dùng cuối, toàn bộ hệ thống được trực quan hóa và giám sát từ xa thông qua nền tảng **Blynk IoT**. Giao diện đáp ứng các tính năng:
- Giám sát thông số môi trường tức thời: Nhiệt độ, Độ ẩm, Tốc độ gió, Áp suất khí quyển.
- Terminal Log: Lưu trữ và hiển thị các biến động thời tiết cục bộ (Sáng/Tối, Mưa/Khô) kèm thời gian (timestamp).
- Switch điều hướng chế độ: Tự động (Auto) / Thủ công (Manual).
- Control Panel: Phím cứng ảo hỗ trợ Kéo ra / Thu vào trực tiếp từ smartphone.

![Giao diện Blynk](.github/assets/Giao%20dien.png)
