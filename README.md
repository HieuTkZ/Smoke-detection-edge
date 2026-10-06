# Machine Learning-Based Real-Time Smoke Concentration Measurement System Using Optical Sensors on Edge Devices 

## 1. Tổng quan

Dự án xây dựng một hệ thống nhúng đo và giám sát khói dựa trên cảm biến quang học hồng ngoại, vi điều khiển ESP32 và mô hình học máy.

Hệ thống sử dụng:

- LED hồng ngoại để phát ánh sáng trong buồng khói.
- Photodiode để thu ánh sáng tán xạ bởi các hạt khói.
- ESP32 để đọc ADC, xử lý dữ liệu, ghi log và hiển thị trên web.
- Module MP-2 để thu tín hiệu tham chiếu trong quá trình thí nghiệm.
- Mô hình học máy để dự đoán tín hiệu `MP2_AO_raw` từ tín hiệu IR.

Ý tưởng chính:

```text
IR_raw
  ↓
Baseline correction
  ↓
EMA filter
  ↓
Temporal features
  ↓
Quantile Regression
  ↓
Baseline gate
  ↓
Output EMA
  ↓
MP2 Pred ADC
```

Mô hình cuối vẫn được triển khai trên ESP32 dưới dạng một phương trình tuyến tính nhẹ nên không cần cài thư viện Machine Learning trên ESP32.

---

## 2. Cấu trúc thư mục dự án

```text
TTTN/
│
├── SmokeMonitor_PCB
│   ├── Esp32_SmokeMonitor.pdsprj
│   └── Esp32_SmokeMonitor.pdsprj....workspace
│
├── codeEsp32_smoke/
│   └── codeEsp32_smoke.ino
│
├── input/
│   ├── train/
│   │   ├── giay1.csv
│   │   ├── giay2.csv
│   │   ├── ...
│   │   ├── log12.csv
│   │   ├── ...
│   │   └── log50.csv
│   │
│   └── test/
│       ├── giay11.csv
│       ├── log3.csv
│       ├── log7.csv
│       └── log8.csv
│
├── output/
│   ├── linear_ir_mp2.h
│   ├── linear_ir_mp2_base.h
│   ├── linear_ir_to_mp2.joblib
│   └── test_results.csv
│
├── inspect_ir_mp2_all_files.ipynb
├── QuantileRegressor.ipynb
├── requirements.txt
└── README.md
```

### Vai trò các file chính

| File / thư mục | Chức năng |
|---|---|
| `codeEsp32_smoke/codeEsp32_smoke.ino` | Firmware chính chạy trên ESP32 |
| `SmokeMonitor/` | Project thiết kế mạch Proteus |
| `input/train/` | Dữ liệu dùng để huấn luyện mô hình |
| `input/test/` | Dữ liệu dùng để đánh giá mô hình |
| `QuantileRegressor.ipynb` | Notebook tiền xử lý, train, test và export model |
| `inspect_ir_mp2_all_files.ipynb` | Notebook kiểm tra và trực quan hóa dữ liệu |
| `output/linear_ir_mp2_base.h` | Model đã chuyển sang dạng chạy trực tiếp trên ESP32 |
| `output/linear_ir_to_mp2.joblib` | Model lưu cho Python |
| `output/test_results.csv` | Kết quả đánh giá trên tập test |
| `requirements.txt` | Các thư viện Python cần thiết |

---

## 3. Định dạng dữ liệu CSV

Các phiên đo sử dụng định dạng:

```text
sample,time_s,time_from_trigger_s,IR_raw,MP2_AO_raw,MP2_DO
```

Ý nghĩa:

| Cột | Ý nghĩa |
|---|---|
| `sample` | Số thứ tự mẫu |
| `time_s` | Thời gian của mẫu |
| `time_from_trigger_s` | Thời gian tính từ trigger |
| `IR_raw` | ADC thô từ photodiode IR |
| `MP2_AO_raw` | ADC Analog Output của MP-2 |
| `MP2_DO` | Digital Output của MP-2 |

Tần số lấy mẫu:

```text
20 Hz
```

Tương ứng:

```text
1 sample / 50 ms
```

> `MP2_DO` không được dùng làm đầu vào của mô hình học máy.

---

## 4. Pipeline xử lý dữ liệu

### 4.1. Baseline correction

Baseline IR được lấy từ đoạn trước trigger hoặc khoảng đầu phiên đo.

```text
IR_corr = max(IR_raw - IR_baseline, 0)
```

Mục đích là giảm ảnh hưởng của offset khác nhau giữa các phiên đo.

---

### 4.2. EMA lọc IR

Tín hiệu IR sau baseline correction được lọc bằng EMA causal.

```text
EMA_t = EMA_(t-1) + alpha × (x_t - EMA_(t-1))
```

Với:

```text
alpha = 1 - exp(-dt / tau)
```

Trong model hiện tại:

```text
dt = 0.05 s
tau_IR = 0.25 s
```

---

## 5. Các feature của mô hình

Mô hình cuối sử dụng 7 feature:

```text
1. IR_filtered
2. IR_ema_1s
3. IR_ema_5s
4. IR_ema_15s
5. IR_area_5s
6. IR_area_15s
7. IR_max_5s
```

Ý nghĩa:

- `IR_filtered`: trạng thái IR hiện tại sau lọc.
- `IR_ema_1s`: bộ nhớ ngắn.
- `IR_ema_5s`: bộ nhớ trung hạn.
- `IR_ema_15s`: bộ nhớ dài.
- `IR_area_5s`: tổng mức IR trong 5 giây gần nhất.
- `IR_area_15s`: tổng mức IR trong 15 giây gần nhất.
- `IR_max_5s`: giá trị IR lớn nhất trong 5 giây gần nhất.

Ở 20 Hz:

```text
5 s  = 100 samples
15 s = 300 samples
```

---

## 6. Mô hình học máy

### 6.1. Linear Regression

Linear Regression được sử dụng ở giai đoạn đầu vì:

- Nhẹ.
- Dễ giải thích.
- Dễ kiểm tra.
- Dễ chuyển sang ESP32.
- Chỉ cần phép nhân và cộng.

Dạng tổng quát:

```text
MP2_raw = intercept + Σ(coef[i] × feature[i])
```

Tuy nhiên, Linear Regression có thể xuất hiện các trường hợp dự đoán cao hơn MP2 thực tế.

---

### 6.2. Quantile Regression

Model hiện tại sử dụng Quantile Regression.

Mục tiêu là làm prediction có xu hướng bias về phía thấp hơn để hạn chế overprediction ở vùng ADC cao.

Giá trị đang sử dụng:

```text
q = 0.375
```

Do:

```text
q < 0.5
```

mô hình có xu hướng dự đoán thấp hơn mô hình nhắm vào giá trị trung tâm.

Điều này quan trọng vì phép chuyển ADC → PPM là phi tuyến. Ở vùng ADC cao, một sai số ADC dương có thể làm sai số PPM tăng rất mạnh.

> Quantile Regression chỉ tạo xu hướng bias thấp, không đảm bảo 100% mọi sample đều có `MP2 Pred <= MP2 Real`.

---

## 7. Baseline gate

Sau khi model dự đoán `MP2_raw`, hệ thống áp dụng baseline gate.

```text
activity = max(
    IR_filtered,
    IR_ema_1s,
    IR_ema_5s,
    IR_ema_15s,
    IR_max_5s
)
```

```text
gate = clip(activity / GATE_THRESHOLD, 0, 1)
```

```text
MP2_gated =
    MP2_BASELINE
    + gate × (MP2_raw - MP2_BASELINE)
```

Gate giúp prediction quay về baseline khi IR gần như không có hoạt động.

Giá trị hiện tại:

```text
GATE_THRESHOLD = 400
```

---

## 8. EMA đầu ra

Prediction sau gate tiếp tục qua EMA đầu ra.

```text
ModelPred_t =
    ModelPred_(t-1)
    + alpha_output
      × (MP2_gated - ModelPred_(t-1))
```

Model hiện tại sử dụng:

```text
tau_output = 0.25 s
```

Tại 20 Hz:

```text
alpha_output ≈ 0.181269
```

Mục đích:

- Làm prediction ổn định hơn.
- Giảm nhiễu.
- Vẫn giữ phản ứng đủ nhanh.

---

# 9. Cài môi trường Python

Khuyến nghị Python 3.10 trở lên.

Tạo virtual environment:

```bash
python -m venv .venv
```

Windows:

```bash
.venv\Scripts\activate
```

Linux/macOS:

```bash
source .venv/bin/activate
```

Cài thư viện:

```bash
pip install -r requirements.txt
```

Các thư viện chính:

```text
numpy
pandas
matplotlib
scikit-learn
joblib
jupyterlab
```

Khởi động Jupyter:

```bash
jupyter lab
```

---

# 10. Huấn luyện lại mô hình

Mở:

```text
QuantileRegressor.ipynb
```

Sau đó chạy lần lượt toàn bộ notebook.

Dữ liệu train phải nằm trong:

```text
input/train/
```

Dữ liệu test phải nằm trong:

```text
input/test/
```

Quy trình:

```text
CSV train
  ↓
Load data
  ↓
Baseline correction
  ↓
Feature extraction
  ↓
Train Quantile Regression
  ↓
Evaluate
  ↓
Export joblib
  ↓
Export ESP32 header
```

Sau khi chạy hoàn tất, các file được tạo trong:

```text
output/
```

Quan trọng nhất để nạp lên ESP32:

```text
output/linear_ir_mp2_base.h
```

---

# 11. Cách nạp trọng số / model vào ESP32

## 11.1. Model trên ESP32 nằm ở đâu?

Model sau khi train được chuyển thành file:

```text
linear_ir_mp2_base.h
```

File này chứa:

- Intercept.
- 7 coefficient.
- MP2 baseline.
- Gate threshold.
- Các hệ số EMA.
- Buffer lịch sử.
- Hàm cập nhật feature.
- Hàm chạy prediction.

Vì vậy có thể xem file `.h` này là **model/trọng số đã đóng gói để chạy trên ESP32**.

ESP32 không cần file `.joblib`.

---

## 11.2. Đặt file `.h` bên cạnh file `.ino`

Arduino yêu cầu project nên có dạng:

```text
codeEsp32_smoke/
│
├── codeEsp32_smoke.ino
└── linear_ir_mp2_base.h
```

Thực hiện:

1. Train model bằng `QuantileRegressor.ipynb`.
2. Vào thư mục:

```text
output/
```

3. Copy:

```text
linear_ir_mp2_base.h
```

4. Paste vào cùng thư mục với:

```text
codeEsp32_smoke.ino
```

Kết quả:

```text
codeEsp32_smoke/
├── codeEsp32_smoke.ino
└── linear_ir_mp2_base.h
```

---

## 11.3. Include model trong Arduino

Trong file `.ino`:

```cpp
#include "linear_ir_mp2_base.h"
```

Không dùng:

```cpp
#include <linear_ir_mp2_base.h>
```

vì đây là file local nằm cùng thư mục sketch.

---

## 11.4. Khởi tạo trạng thái model

Khai báo:

```cpp
MP2ModelState mp2Model;
```

Sau khi đã xác định baseline IR:

```cpp
mp2ModelReset(
    mp2Model,
    irBaseline
);
```

`irBaseline` phải là mức IR nền khi môi trường sạch.

---

## 11.5. Chạy prediction

Model phải được gọi **một lần cho mỗi sample mới**.

Với sampling 20 Hz:

```cpp
float mp2PredAdc =
    mp2ModelUpdate(
        mp2Model,
        irRaw
    );
```

Thời gian giữa hai lần gọi:

```text
50 ms
```

Ví dụ:

```cpp
if (millis() - lastSampleMs >= 50) {
    lastSampleMs += 50;

    int irRaw = analogRead(IR_PIN);
    int mp2Raw = analogRead(MP2_PIN);

    float mp2PredAdc =
        mp2ModelUpdate(
            mp2Model,
            irRaw
        );

    // sử dụng mp2PredAdc
}
```

---

## 11.6. Cực kỳ quan trọng: không gọi model nhiều lần cho cùng một sample

Sai:

```cpp
float pred1 = mp2ModelUpdate(mp2Model, irRaw);

// gửi web
float pred2 = mp2ModelUpdate(mp2Model, irRaw);

// ghi log
float pred3 = mp2ModelUpdate(mp2Model, irRaw);
```

Vì mỗi lần gọi `mp2ModelUpdate()` sẽ cập nhật:

- EMA.
- History.
- Area.
- Max.
- Prediction state.

Nếu gọi nhiều lần trong cùng một sample, thời gian nội bộ của model sẽ bị chạy nhanh giả tạo.

Đúng:

```cpp
float mp2PredAdc =
    mp2ModelUpdate(
        mp2Model,
        irRaw
    );

// dùng lại cùng giá trị:
sendToWeb(mp2PredAdc);
saveLog(mp2PredAdc);
```

---

# 12. Cập nhật model mới lên ESP32

Khi train lại:

```text
QuantileRegressor.ipynb
       ↓
output/linear_ir_mp2_base.h
```

Chỉ cần:

```text
Copy file .h mới
       ↓
Replace file .h cũ trong sketch Arduino
       ↓
Compile
       ↓
Upload ESP32
```

Không cần sửa lại toàn bộ firmware nếu interface của header không đổi.

---

# 13. Kiểm tra đúng model sau khi thay `.h`

Trước khi upload:

1. Đảm bảo `.h` mới nằm cùng thư mục `.ino`.
2. Đảm bảo tên file đúng:

```text
linear_ir_mp2_base.h
```

3. Kiểm tra `.ino` include đúng:

```cpp
#include "linear_ir_mp2_base.h"
```

4. Compile lại toàn bộ project.
5. Upload lại ESP32.
6. Test với một phiên đo mới.

Nếu ESP32 vẫn cho prediction giống model cũ, kiểm tra xem có đang dùng nhầm header cũ ở thư mục khác hay không.

---

# 14. Lỗi Arduino thường gặp

## 14.1. `linear_ir_mp2_base.h: No such file or directory`

Nguyên nhân:

- File `.h` chưa nằm cùng thư mục `.ino`.
- Tên file không đúng.
- Include sai.

Cấu trúc đúng:

```text
codeEsp32_smoke/
├── codeEsp32_smoke.ino
└── linear_ir_mp2_base.h
```

---

## 14.2. Lỗi `redefinition of setup()` hoặc `loop()`

Arduino sẽ compile **tất cả file `.ino` nằm trong cùng thư mục sketch**.

Không để:

```text
codeEsp32_smoke/
├── old_code.ino
├── test.ino
└── codeEsp32_smoke.ino
```

nếu các file đều chứa `setup()` và `loop()`.

Khuyến nghị chỉ để một file `.ino` chính:

```text
codeEsp32_smoke/
├── codeEsp32_smoke.ino
└── linear_ir_mp2_base.h
```

Các code cũ nên chuyển ra thư mục khác.

---

## 14.3. Prediction ESP32 khác Python

Kiểm tra:

- Có dùng đúng file `.h` mới nhất không?
- Sampling có đúng 20 Hz / 50 ms không?
- `mp2ModelUpdate()` có bị gọi nhiều lần mỗi sample không?
- Baseline IR trên ESP32 có đúng không?
- Feature order có bị chỉnh sửa không?
- Có chỉnh coefficient bằng tay không?
- Có dùng raw IR thay vì pipeline trong header không?

---

# 15. Không chỉnh tay trọng số nếu không cần thiết

Không nên sửa thủ công:

```cpp
MP2_INTERCEPT
MP2_COEFFICIENTS[]
MP2_ALPHA_...
MP2_BASELINE
```

trừ khi đang thực hiện một thí nghiệm có kiểm soát.

Quy trình đúng:

```text
Thay đổi dữ liệu / model trong Python
        ↓
Train lại
        ↓
Export header mới
        ↓
Copy header sang Arduino
```

Điều này giúp tránh tình trạng model Python và model ESP32 không còn giống nhau.

---

# 16. Chuyển ADC MP-2 sang PPM

Theo phương pháp sử dụng trong đề tài:

```text
V_MP2 = (ADC / 4095) × 5
```

```text
Rs = RL × (5 - V_MP2) / V_MP2
```

Sau đó:

```text
PPM = A × (Rs / R0)^B
```

Lưu ý:

- Quan hệ ADC → PPM là phi tuyến.
- Khi ADC tăng cao, sai số ADC có thể gây sai số PPM rất lớn.
- Đây là lý do model hiện tại ưu tiên giảm overprediction bằng Quantile Regression.

Do đó nên đánh giá model ở miền ADC trước khi đánh giá ở miền PPM.

---

# 17. Các metric đánh giá model

Các metric chính:

### R²

Đánh giá khả năng model bám theo biến thiên của MP2.

```text
R² càng gần 1 càng tốt
```

### RMSE

```text
RMSE = sqrt(mean((Real - Pred)^2))
```

Phạt mạnh các sai số lớn.

### MAE

```text
MAE = mean(abs(Real - Pred))
```

Cho biết trung bình prediction lệch bao nhiêu ADC.

Ngoài ra nên theo dõi thêm:

```text
Bias = mean(Pred - Real)
```

```text
OverRate = số mẫu Pred > Real / tổng số mẫu
```

```text
MaxOverError = max(Pred - Real)
```

đặc biệt vì hệ thống cần hạn chế prediction cao ở vùng ADC lớn.

---

# 18. Hướng cải thiện mô hình

Hiện tại số lượng phiên train còn hạn chế.

Ưu tiên đầu tiên:

```text
Thu thêm dữ liệu
```

Nên mở rộng:

- Nhiều mức khói.
- Nhiều thời gian duy trì khói.
- Nhiều khoảng cách.
- Nhiều điều kiện môi trường.
- Nhiều trạng thái tăng / giảm.
- Các trường hợp gần saturation.

Khi dữ liệu đủ lớn và quan hệ phi tuyến rõ hơn, có thể thử ANN.

Ví dụ:

```text
7 input features
      ↓
Dense 16
      ↓
Dense 8
      ↓
1 output
```

Nếu vẫn muốn model ưu tiên bias thấp, ANN có thể tiếp tục sử dụng Quantile/Pinball Loss.

---

# 19. Hướng cải thiện hiệu năng ESP32

Model tuyến tính hiện tại rất nhẹ.

Tuy nhiên ESP32 còn phải xử lý:

```text
ADC
Model
Logging
Web
SSE
Wi-Fi
JSON
```

Nếu tất cả nằm trong một `loop()` và tải tăng, có thể ảnh hưởng chu kỳ 50 ms.

Hướng phát triển:

```text
SensorTask
    ↓
Queue
    ↓
ModelTask
   ├── LogTask
   └── Web/SSE Task
```

Mục tiêu:

```text
Sampling và Model không phải chờ Web/Wi-Fi
```

FreeRTOS Task và Queue chỉ nên được triển khai khi thực sự xuất hiện vấn đề về timing, blocking hoặc tải hệ thống.

---

# 20. Checklist trước khi chạy dự án

## Python

```text
[ ] Cài requirements.txt
[ ] Dữ liệu train đúng thư mục input/train/
[ ] Dữ liệu test đúng thư mục input/test/
[ ] Chạy QuantileRegressor.ipynb từ đầu đến cuối
[ ] Kiểm tra metric
[ ] Kiểm tra đồ thị prediction
[ ] Export linear_ir_mp2_base.h
```

## ESP32

```text
[ ] codeEsp32_smoke.ino
[ ] linear_ir_mp2_base.h nằm cùng thư mục
[ ] #include "linear_ir_mp2_base.h"
[ ] Chỉ có một file .ino chứa setup()/loop()
[ ] Baseline IR được xác định trước khi reset model
[ ] mp2ModelUpdate() gọi đúng 1 lần / sample
[ ] Sampling 20 Hz ~ 50 ms
[ ] Compile lại sau khi thay header
[ ] Upload firmware mới
[ ] Test bằng phiên đo thực tế
```

---

# 21. Ghi chú quan trọng

1. MP-2 trong dự án được dùng làm tín hiệu tham chiếu thực nghiệm cho quá trình train.
2. Giá trị PPM phụ thuộc vào quá trình quy đổi và hiệu chuẩn cảm biến, do đó không nên xem prediction PPM là giá trị chuẩn tuyệt đối nếu chưa có thiết bị tham chiếu đã hiệu chuẩn.
3. Model học từ dữ liệu đã thu thập; chất lượng prediction phụ thuộc trực tiếp vào độ đa dạng và chất lượng dữ liệu.
4. Không dùng `MP2_DO` làm feature cho model.
5. Khi thay model, ưu tiên export lại header thay vì sửa coefficient trực tiếp trong Arduino.
6. Mọi thay đổi về feature trong Python phải được đồng bộ với phần tính feature trên ESP32.
7. Khi test lỗi prediction, nên kiểm tra ADC trước, sau đó mới kiểm tra PPM.

---

# 22. Quy trình làm việc đề xuất

```text
Thu dữ liệu mới
      ↓
Kiểm tra CSV
      ↓
inspect_ir_mp2_all_files.ipynb
      ↓
QuantileRegressor.ipynb
      ↓
Đánh giá R² / RMSE / MAE / Bias
      ↓
Export linear_ir_mp2_base.h
      ↓
Copy .h cạnh .ino
      ↓
Compile + Upload ESP32
      ↓
Test thực tế
      ↓
Lưu log mới
      ↓
Bổ sung data cho lần train tiếp theo
```

---

## Tóm tắt nhanh cách thay model trên ESP32

```text
1. Chạy QuantileRegressor.ipynb
2. Lấy output/linear_ir_mp2_base.h
3. Copy vào codeEsp32_smoke/
4. Đảm bảo:
   codeEsp32_smoke/
   ├── codeEsp32_smoke.ino
   └── linear_ir_mp2_base.h
5. Compile Arduino
6. Upload ESP32
```

Trong `.ino`:

```cpp
#include "linear_ir_mp2_base.h"
```

Khởi tạo:

```cpp
MP2ModelState mp2Model;

mp2ModelReset(
    mp2Model,
    irBaseline
);
```

Mỗi 50 ms:

```cpp
float mp2PredAdc =
    mp2ModelUpdate(
        mp2Model,
        irRaw
    );
```

**Không cần cài model Machine Learning riêng trên ESP32. File `.h` chính là phiên bản model đã được chuyển sang C/C++ để firmware sử dụng trực tiếp.**
