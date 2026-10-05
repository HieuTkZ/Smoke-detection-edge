#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>

// ============================================================================
//  ESP32 SMOKE SENSOR - ADC STREAMING + WEB LOG
//
//  Chức năng:
//  1. ESP32 kết nối vào Wi-Fi ở chế độ Station.
//  2. Đọc tín hiệu analog từ mạch photodiode tại GPIO34.
//  3. Stream dữ liệu ADC trực tiếp lên trình duyệt bằng Server-Sent Events (SSE).
//  4. Hiển thị đồ thị ADC thời gian thực.
//  5. Bắt đầu phiên khi IR hoặc MP-2 AO đạt START_THRESHOLD liên tục 5 giây.
//     Vẫn giữ 3 giây dữ liệu trước thời điểm trigger để không mất phần pre-trigger.
//  6. Kết thúc phiên khi cả IR và MP-2 AO cùng <= STOP_THRESHOLD liên tục 5 giây;
//     nếu một trong hai tín hiệu tăng lại thì hủy chờ kết thúc.
//  7. Mỗi log lưu:
//       - Đồ thị ADC theo thời gian
//       - IR ADC lớn nhất / trung bình
//       - Gas AO lớn nhất / trung bình
//       - Tỷ lệ DO ở mức HIGH
//       - Cụm 3 đồ thị: IR, AO và DO
//       - Nút tải CSV riêng cho từng phiên
//       - Hover đồng bộ 3 đồ thị để xem IR/AO/DO tại cùng một mẫu
//       - Đường trigger t = 0 để so sánh đáp ứng IR và MP-2
//       - Thời gian bắt đầu
//       - Thời gian kết thúc
//       - Thời lượng
//       - Số mẫu
//  8. Các log hoàn thành được lưu trong localStorage của trình duyệt,
//     nên refresh trang vẫn còn dữ liệu.
//
//  Lưu ý:
//  - Logic hiện tại giả sử: khói tăng -> ADC tăng.
//  - Nếu mạch của bạn cho quan hệ ngược lại (khói tăng -> ADC giảm),
//    cần đảo điều kiện START/STOP trong hàm processMeasurement() ở JavaScript.
// ============================================================================

// ============================================================================
// CẤU HÌNH WIFI
// ============================================================================

// Thay bằng tên Wi-Fi và mật khẩu thực tế.
const char* WIFI_SSID = "Thu Suong";
const char* WIFI_PASSWORD = "0906620436";

// ============================================================================
// CẤU HÌNH ADC
// ============================================================================

// GPIO34: tín hiệu analog từ mạch photodiode hồng ngoại.
// GPIO35: chân AO (Analog Output) của cảm biến MP-2/MQ-2.
// GPIO32: chân DO (Digital Output) của cảm biến MP-2/MQ-2.
//
// GPIO34 và GPIO35 được dùng để đọc ADC.
// GPIO32 được dùng để đọc trạng thái digital HIGH/LOW từ comparator trên module.
const int IR_PIN = 34;
const int GAS_AO_PIN = 35;
const int GAS_DO_PIN = 32;

// ADC ESP32 được đọc ở độ phân giải 12 bit: 0 -> 4095.
const int ADC_RESOLUTION_BITS = 12;

// Chu kỳ lấy mẫu.
// 50 ms tương đương 20 mẫu/giây, đủ nhanh cho tín hiệu khói và nhẹ cho web.
const unsigned long SAMPLE_INTERVAL_MS = 50;

// Thời gian giữa hai lần in ADC ra Serial Monitor.
// Không nên Serial.print ở mỗi mẫu vì sẽ làm log Serial quá nhiều.
const unsigned long SERIAL_PRINT_INTERVAL_MS = 500;

unsigned long lastSampleTime = 0;
unsigned long lastSerialPrintTime = 0;

// ============================================================================
// CẤU HÌNH TỰ ĐỘNG KẾT NỐI LẠI WIFI
// ============================================================================

const unsigned long WIFI_RECONNECT_INTERVAL_MS = 5000;
unsigned long lastWiFiReconnectTime = 0;

// ============================================================================
// WEB SERVER + SERVER SENT EVENTS
// ============================================================================

// Web Server chạy ở cổng 80.
AsyncWebServer server(80);

// Endpoint /events dùng để stream dữ liệu ADC xuống trình duyệt.
AsyncEventSource events("/events");

// ============================================================================
// TRANG WEB
// ============================================================================

const char PAGE[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>ESP32 Smoke Sensor Logger</title>
<style>*{box-sizing:border-box;}body{margin:0;padding:20px;background:#111827;color:#f3f4f6;font-family:Arial,sans-serif;}.container{max-width:1200px;margin:auto;}h1,h2{margin-top:0;}.header{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:15px;margin-bottom:20px;}.header h1{margin-bottom:5px;}.subtitle{color:#9ca3af;}.statusBox{display:flex;align-items:center;gap:10px;padding:10px 16px;background:#1f2937;border-radius:30px;}.statusDot{width:12px;height:12px;border-radius:50%;background:#f59e0b;box-shadow:0 0 8px #f59e0b;}.statusText{color:#f59e0b;font-size:14px;font-weight:bold;}.chartBox{padding:15px;background:#1f2937;border-radius:12px;}.realtimeGrid{display:grid;grid-template-columns:1fr;gap:16px;}.chartTitle{margin-bottom:4px;font-size:16px;font-weight:bold;}.chartUnit{margin-bottom:10px;color:#9ca3af;font-size:13px;}#irChart,#gasAnalogChart,#gasDigitalChart{width:100%;height:300px;display:block;}#gasDigitalChart{height:210px;}.liveControls{margin-top:12px;margin-bottom:5px;}.controls{display:flex;gap:10px;flex-wrap:wrap;margin-top:15px;}button{padding:10px 18px;border:none;border-radius:8px;font-size:15px;cursor:pointer;}#pauseBtn{background:#f59e0b;color:#111827;}#clearLiveBtn{background:#374151;color:white;}#clearLogsBtn{background:#7f1d1d;color:white;}.recordPanel{display:flex;justify-content:space-between;align-items:center;gap:15px;flex-wrap:wrap;margin-top:20px;padding:16px 18px;background:#1f2937;border-radius:12px;}.recordInfo{color:#9ca3af;font-size:14px;}.recordStatus{padding:8px 14px;border-radius:20px;font-size:13px;font-weight:bold;}.recordStatus.waiting{background:#374151;color:#d1d5db;}.recordStatus.recording{background:#7f1d1d;color:#fca5a5;}.logSection{margin-top:25px;}.logHeader{display:flex;justify-content:space-between;align-items:center;gap:10px;flex-wrap:wrap;margin-bottom:15px;}.logHeader h2{margin-bottom:0;}.logItem{margin-bottom:18px;padding:18px;background:#1f2937;border-radius:12px;}.logTitleRow{display:flex;justify-content:space-between;align-items:center;gap:12px;flex-wrap:wrap;margin-bottom:15px;}.logTitle{font-size:18px;font-weight:bold;}.logActions{display:flex;gap:8px;flex-wrap:wrap;}.downloadCsvBtn{padding:8px 14px;background:#059669;color:white;font-size:14px;font-weight:bold;}.downloadCsvBtn:hover{filter:brightness(1.08);}.logInfo{display:grid;grid-template-columns:repeat(4,1fr);gap:12px;margin-bottom:15px;}.logInfoItem{padding:12px;background:#111827;border-radius:8px;}.logInfoLabel{margin-bottom:5px;color:#9ca3af;font-size:12px;}.logInfoValue{font-size:16px;font-weight:bold;}.logCanvas{width:100%;height:230px;display:block;background:#111827;border-radius:8px;}.logChartGroup{display:grid;grid-template-columns:1fr;gap:14px;}.logChartBlock{position:relative;padding:12px;background:#111827;border-radius:10px;}.logTooltip{position:absolute;z-index:5;top:44px;right:24px;display:none;min-width:210px;padding:10px 12px;background:rgba(3,7,18,0.94);border:1px solid #4b5563;border-radius:8px;color:#e5e7eb;font-size:12px;line-height:1.55;pointer-events:none;box-shadow:0 8px 24px rgba(0,0,0,0.28);}.logTooltip.active{display:block;}.logTooltipTitle{margin-bottom:4px;color:#ffffff;font-weight:bold;}.triggerLegend{margin:0 0 12px;color:#9ca3af;font-size:12px;}.logChartTitle{margin-bottom:10px;color:#d1d5db;font-size:14px;font-weight:bold;}.logChartBlock .logCanvas{background:#0b1220;}.digitalCanvas{height:180px;}.emptyLog{padding:20px;background:#1f2937;color:#9ca3af;border-radius:12px;text-align:center;}@media (max-width:950px){.logInfo{grid-template-columns:repeat(2,1fr);}}@media (max-width:520px){body{padding:12px;}#irChart,#gasAnalogChart{height:260px;}#gasDigitalChart{height:180px;}.logInfo{grid-template-columns:1fr;}.logCanvas{height:200px;}}</style>
</head>
<body>
<noscript><div style="padding:12px;background:#7f1d1d;color:white">Trình duyệt đang tắt JavaScript.</div></noscript>
<div class="container">
<div class="header">
<div>
<h1>ESP32 Smoke Sensor</h1>
<div class="subtitle">Photodiode GPIO34 + Gas AO GPIO35 + Gas DO GPIO32 — Streaming SSE</div>
</div>
<div class="statusBox">
<div id="statusDot" class="statusDot">
</div>
<div id="statusText" class="statusText">ĐANG KẾT NỐI</div>
</div>
</div>
<div class="realtimeGrid">
<div class="chartBox">
<div class="chartTitle">Photodiode IR — GPIO34</div>
<div class="chartUnit">Đơn vị: RAW ADC 12 bit</div>
<canvas id="irChart"></canvas>
</div>
<div class="chartBox">
<div class="chartTitle">MP-2 / MQ-2 Analog AO — GPIO35</div>
<div class="chartUnit">Đơn vị: RAW ADC 12 bit</div>
<canvas id="gasAnalogChart"></canvas>
</div>
<div class="chartBox">
<div class="chartTitle">MP-2 / MQ-2 Digital DO — GPIO32</div>
<div class="chartUnit">Đơn vị: Logic LOW/HIGH (0/1)</div>
<canvas id="gasDigitalChart"></canvas>
</div>
</div>
<div class="controls liveControls">
<button id="pauseBtn" onclick="togglePause()">Tạm dừng đồ thị</button>
<button id="clearLiveBtn" onclick="clearLiveData()">Xóa dữ liệu hiện tại</button>
</div>
<div class="recordPanel">
<div>
<strong>Ghi log tự động</strong>
<div class="recordInfo" id="recordInfo">Bắt đầu khi IR hoặc MP-2 AO ≥ 500 liên tục 5 giây; kết thúc khi cả hai ≤ 400 liên tục 5 giây.</div>
</div>
<div id="recordStatus" class="recordStatus waiting">ĐANG CHỜ TÍN HIỆU</div>
</div>
<div class="logSection">
<div class="logHeader">
<h2>Nhật ký đo</h2>
<button id="clearLogsBtn" onclick="clearLogs()">Xóa toàn bộ log</button>
</div>
<div id="logContainer"></div>
</div>
</div>
<script>
const MAX_POINTS = 500;
const IR_Y_MIN = 0;
const IR_Y_MAX = 5000;
const GAS_RAW_MIN = 0;
const GAS_RAW_MAX = 5000;
const DIGITAL_MIN = 0;
const DIGITAL_MAX = 1;
const START_THRESHOLD = 500;
const STOP_THRESHOLD = 400;
const START_HOLD_TIME = 5000;
const PRE_TRIGGER_TIME = 3000;
const POST_TRIGGER_TIME = 5000;
const MAX_LOGS = 20;
const STORAGE_KEY = "esp32_smoke_logs_ir_ao_do_v3";
const irCanvas = document.getElementById("irChart");
const irCtx = irCanvas.getContext("2d");
const gasAnalogCanvas = document.getElementById("gasAnalogChart");
const gasAnalogCtx = gasAnalogCanvas.getContext("2d");
const gasDigitalCanvas = document.getElementById("gasDigitalChart");
const gasDigitalCtx = gasDigitalCanvas.getContext("2d");
let liveIRData = [];
let liveGasAnalogRawData = [];
let liveGasDigitalData = [];
let paused = false;
let recording = false;
let currentLog = null;
let logHistory = [];
let aboveThresholdSince = null;
let belowThresholdSince = null;
let recentSampleBuffer = [];
let logCounter = 0;

function prepareCanvas(canvas, context) {
  const rect = canvas.getBoundingClientRect();
  const ratio = window.devicePixelRatio || 1;
  canvas.width = rect.width * ratio;
  canvas.height = rect.height * ratio;
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
}

function resizeRealtimeCharts() {
  prepareCanvas(irCanvas, irCtx);
  prepareCanvas(gasAnalogCanvas, gasAnalogCtx);
  prepareCanvas(gasDigitalCanvas, gasDigitalCtx);
  drawIRRealtimeChart();
  drawGasAnalogRealtimeChart();
  drawGasDigitalRealtimeChart();
}
window.addEventListener("resize", function() {
  resizeRealtimeCharts(); setTimeout(function() {
    redrawAllLogCharts();
  }, 50);
});

function drawAnalogRealtimeChart(canvas, context, values, yMin, yMax, unitText, lineColor) {
  const width = canvas.clientWidth;
  const height = canvas.clientHeight;
  context.clearRect(0, 0, width, height);
  const left = 62;
  const right = 18;
  const top = 15;
  const bottom = 35;
  const plotWidth = width - left - right;
  const plotHeight = height - top - bottom;
  context.strokeStyle = "#374151";
  context.lineWidth = 1;
  context.fillStyle = "#9ca3af";
  context.font = "11px Arial";
  const gridCount = 5;
  for (let i = 0; i <= gridCount; i++) {
    const y = top + (plotHeight / gridCount) * i;
    context.beginPath();
    context.moveTo(left, y);
    context.lineTo(width - right, y);
    context.stroke();
    const axisValue = yMax - ((yMax - yMin) / gridCount) * i;
    context.fillText(Math.round(axisValue), 5, y + 4);
  }
  context.fillStyle = "#9ca3af";
  context.fillText(unitText, 5, 12);
  if (values.length < 2) {
    return;
  }
  context.strokeStyle = lineColor;
  context.lineWidth = 2;
  context.lineJoin = "round";
  context.lineCap = "round";
  context.beginPath();
  for (let i = 0; i < values.length; i++) {
    const x = left + (i / Math.max(MAX_POINTS - 1, 1)) * plotWidth;
    const normalized = (values[i] - yMin) / (yMax - yMin);
    const clamped = Math.max(0, Math.min(1, normalized));
    const y = top + plotHeight - clamped * plotHeight;
    if (i === 0) {
      context.moveTo(x, y);
    }else {
      context.lineTo(x, y);
    }
  }
  context.stroke();
}

function drawIRRealtimeChart() {
  drawAnalogRealtimeChart(irCanvas, irCtx, liveIRData, IR_Y_MIN, IR_Y_MAX, "ADC", "#38bdf8");
}

function drawGasAnalogRealtimeChart() {
  drawAnalogRealtimeChart(gasAnalogCanvas, gasAnalogCtx, liveGasAnalogRawData, GAS_RAW_MIN, GAS_RAW_MAX, "RAW", "#f59e0b");
}

function drawGasDigitalRealtimeChart() {
  const width = gasDigitalCanvas.clientWidth;
  const height = gasDigitalCanvas.clientHeight;
  gasDigitalCtx.clearRect(0, 0, width, height);
  const left = 62;
  const right = 18;
  const top = 20;
  const bottom = 35;
  const plotWidth = width - left - right;
  const plotHeight = height - top - bottom;
  const yHigh = top + plotHeight * 0.2;
  const yLow = top + plotHeight * 0.8;
  gasDigitalCtx.strokeStyle = "#374151";
  gasDigitalCtx.lineWidth = 1;
  gasDigitalCtx.beginPath();
  gasDigitalCtx.moveTo(left, yHigh);
  gasDigitalCtx.lineTo(width - right, yHigh);
  gasDigitalCtx.stroke();
  gasDigitalCtx.beginPath();
  gasDigitalCtx.moveTo(left, yLow);
  gasDigitalCtx.lineTo(width - right, yLow);
  gasDigitalCtx.stroke();
  gasDigitalCtx.fillStyle = "#9ca3af";
  gasDigitalCtx.font = "11px Arial";
  gasDigitalCtx.fillText("HIGH 1", 5, yHigh + 4);
  gasDigitalCtx.fillText("LOW 0", 5, yLow + 4);
  gasDigitalCtx.fillText("Logic", 5, 12);
  if (liveGasDigitalData.length < 2) {
    return;
  }
  gasDigitalCtx.strokeStyle = "#a78bfa";
  gasDigitalCtx.lineWidth = 2;
  gasDigitalCtx.lineJoin = "miter";
  gasDigitalCtx.beginPath();
  let previousY = liveGasDigitalData[0] ? yHigh: yLow;
  gasDigitalCtx.moveTo(left, previousY);
  for (let i = 1; i < liveGasDigitalData.length; i++) {
    const x = left + (i / Math.max(MAX_POINTS - 1, 1)) * plotWidth;
    const currentY = liveGasDigitalData[i] ? yHigh: yLow;
    gasDigitalCtx.lineTo(x, previousY);
    if (currentY !== previousY) {
      gasDigitalCtx.lineTo(x, currentY);
    }
    previousY = currentY;
  }
  gasDigitalCtx.stroke();
}

function setConnectionStatus(text, color) {
  const dot = document.getElementById("statusDot");
  const status = document.getElementById("statusText");
  status.innerText = text;
  status.style.color = color;
  dot.style.background = color;
  dot.style.boxShadow = "0 0 8px " + color;
}

function addLiveValue(irValue, gasAnalogRaw, gasDigitalValue) {
  if (paused) {
    return;
  }
  liveIRData.push(irValue);
  liveGasAnalogRawData.push(gasAnalogRaw);
  liveGasDigitalData.push(gasDigitalValue);
  if (liveIRData.length > MAX_POINTS) {
    liveIRData.shift();
  }
  if (liveGasAnalogRawData.length > MAX_POINTS) {
    liveGasAnalogRawData.shift();
  }
  if (liveGasDigitalData.length > MAX_POINTS) {
    liveGasDigitalData.shift();
  }
  drawIRRealtimeChart();
  drawGasAnalogRealtimeChart();
  drawGasDigitalRealtimeChart();
}

function setRecordStatus(isRecording) {
  const element = document.getElementById("recordStatus");
  if (isRecording) {
    element.innerText = "● ĐANG GHI LOG";
    element.className = "recordStatus recording";
  }else {
    element.innerText = "ĐANG CHỜ TÍN HIỆU";
    element.className = "recordStatus waiting";
  }
}

function startLog(irValue, gasAnalogRaw, gasDigitalValue, espTime, preTriggerSamples, triggerEspTime) {
  recording = true;
  aboveThresholdSince = null;
  belowThresholdSince = null;
  logCounter++;
  const firstSample = preTriggerSamples.length > 0 ? preTriggerSamples[0]: { espTime: espTime };
  const preTriggerDuration = Math.max(0, triggerEspTime - firstSample.espTime);
  const bufferedDuration = Math.max(0, espTime - firstSample.espTime);
  currentLog = { id: logCounter, startDate: new Date(Date.now() - bufferedDuration).toISOString(), startEspTime: firstSample.espTime, triggerEspTime: triggerEspTime, triggerOffset: preTriggerDuration, endDate: null, duration: 0, irValues: [], gasAnalogRawValues: [], gasDigitalValues: [], times: [], irSum: 0, gasAnalogRawSum: 0, count: 0, digitalHighCount: 0, digitalLowCount: 0, maxIR: irValue, maxGasAnalogRaw: gasAnalogRaw, averageIR: 0, averageGasAnalogRaw: 0, digitalHighPercent: 0 };
  setRecordStatus(true);
  for (const sample of preTriggerSamples) {
    addLogSample(sample.irValue, sample.gasAnalogRaw, sample.gasDigitalValue, sample.espTime);
  }
  console.log("Bat dau log #", currentLog.id, "| pre-trigger =", preTriggerDuration, "ms | xac nhan sau =", START_HOLD_TIME, "ms");
}

function addLogSample(irValue, gasAnalogRaw, gasDigitalValue, espTime) {
  if (!recording || currentLog === null) {
    return;
  }
  const elapsed = espTime - currentLog.startEspTime;
  currentLog.irValues.push(irValue);
  currentLog.gasAnalogRawValues.push(gasAnalogRaw);
  currentLog.gasDigitalValues.push(gasDigitalValue);
  currentLog.times.push(elapsed);
  currentLog.irSum += irValue;
  currentLog.gasAnalogRawSum += gasAnalogRaw;
  currentLog.count++;
  if (irValue > currentLog.maxIR) {
    currentLog.maxIR = irValue;
  }
  if (gasAnalogRaw > currentLog.maxGasAnalogRaw) {
    currentLog.maxGasAnalogRaw = gasAnalogRaw;
  }
  if (gasDigitalValue) {
    currentLog.digitalHighCount++;
  }else {
    currentLog.digitalLowCount++;
  }
}

function finishLog(espTime) {
  if (currentLog === null) {
    return;
  }
  currentLog.endDate = new Date().toISOString();
  currentLog.duration = espTime - currentLog.startEspTime;
  currentLog.averageIR = currentLog.count > 0 ? currentLog.irSum / currentLog.count: 0;
  currentLog.averageGasAnalogRaw = currentLog.count > 0 ? currentLog.gasAnalogRawSum / currentLog.count: 0;
  currentLog.digitalHighPercent = currentLog.count > 0 ? (currentLog.digitalHighCount / currentLog.count) * 100: 0;
  logHistory.unshift(currentLog);
  if (logHistory.length > MAX_LOGS) {
    logHistory = logHistory.slice(0, MAX_LOGS);
  }
  console.log("Ket thuc log #", currentLog.id, currentLog);
  saveLogs();
  renderLogs();
  currentLog = null;
  recording = false;
  aboveThresholdSince = null;
  belowThresholdSince = null;
  setRecordStatus(false);
}

function updateRecentSampleBuffer(irValue, gasAnalogRaw, gasDigitalValue, espTime) {
  recentSampleBuffer.push( { irValue: irValue, gasAnalogRaw: gasAnalogRaw, gasDigitalValue: gasDigitalValue, espTime: espTime });
  const cutoffTime = espTime - (PRE_TRIGGER_TIME + START_HOLD_TIME);
  while (recentSampleBuffer.length > 0 && recentSampleBuffer[0].espTime < cutoffTime) {
    recentSampleBuffer.shift();
  }
}

function processMeasurement(irValue, gasAnalogRaw, gasDigitalValue, espTime) {
  updateRecentSampleBuffer(irValue, gasAnalogRaw, gasDigitalValue, espTime);
  if (!recording) {
    const startCondition = irValue >= START_THRESHOLD || gasAnalogRaw >= START_THRESHOLD;
    if (startCondition) {
      if (aboveThresholdSince === null) {
        aboveThresholdSince = espTime;
      }
      if (espTime - aboveThresholdSince >= START_HOLD_TIME) {
        const triggerEspTime = aboveThresholdSince;
        const preTriggerStartTime = triggerEspTime - PRE_TRIGGER_TIME;
        const preTriggerSamples = recentSampleBuffer.filter(function(sample) {
          return sample.espTime >= preTriggerStartTime;
        });
        startLog(irValue, gasAnalogRaw, gasDigitalValue, espTime, preTriggerSamples, triggerEspTime);
      }
    }else {
      aboveThresholdSince = null;
    }
    return;
  }
  addLogSample(irValue, gasAnalogRaw, gasDigitalValue, espTime);
  const stopCondition = irValue <= STOP_THRESHOLD && gasAnalogRaw <= STOP_THRESHOLD;
  if (stopCondition) {
    if (belowThresholdSince === null) {
      belowThresholdSince = espTime;
    }
    if (espTime - belowThresholdSince >= POST_TRIGGER_TIME) {
      finishLog(espTime);
    }
  }else {
    belowThresholdSince = null;
  }
}

function formatDate(dateString) {
  if (!dateString) {
    return "-";
  }
  const date = new Date(dateString);
  return date.toLocaleTimeString("vi-VN", { hour: "2-digit", minute: "2-digit", second: "2-digit" });
}

function formatDuration(ms) {
  const totalSeconds = ms / 1000;
  if (totalSeconds < 60) {
    return(totalSeconds.toFixed(1) + " giây");
  }
  const minutes = Math.floor(totalSeconds / 60);
  const seconds = totalSeconds - minutes * 60;
  return(minutes + " phút " + seconds.toFixed(1) + " giây");
}

function saveLogs() {
  try {
    localStorage.setItem(STORAGE_KEY, JSON.stringify(logHistory));
  }catch (error) {
    console.warn("Khong the luu log vao localStorage:", error);
  }
}

function loadLogs() {
  try {
    const stored = localStorage.getItem(STORAGE_KEY);
    if (!stored) {
      return;
    }
    const parsed = JSON.parse(stored);
    if (!Array.isArray(parsed)) {
      return;
    }
    logHistory = parsed.slice(0, MAX_LOGS);
    for (const log of logHistory) {
      if (Number(log.id) > logCounter) {
        logCounter = Number(log.id);
      }
    }
  }catch (error) {
    console.warn("Khong the doc log tu localStorage:", error);
    logHistory = [];
  }
}

function getTriggerOffsetMs(log) {
  const directOffset = Number(log.triggerOffset);
  if (Number.isFinite(directOffset) && directOffset >= 0) {
    return directOffset;
  }
  const triggerEspTime = Number(log.triggerEspTime);
  const startEspTime = Number(log.startEspTime);
  if (Number.isFinite(triggerEspTime) && Number.isFinite(startEspTime)) {
    return Math.max(0, triggerEspTime - startEspTime);
  }
  return 0;
}

function getLogSampleCount(log) {
  const arrays = [log.times, log.irValues, log.gasAnalogRawValues, log.gasDigitalValues];
  let count = Infinity;
  for (const values of arrays) {
    if (!Array.isArray(values)) {
      return 0;
    }
    count = Math.min(count, values.length);
  }
  return Number.isFinite(count) ? count: 0;
}

function formatRelativeSeconds(seconds) {
  const value = Number(seconds);
  if (!Number.isFinite(value)) {
    return "-";
  }
  if (Math.abs(value) < 0.0005) {
    return "0.000 s";
  }
  return(value > 0 ? "+": "") + value.toFixed(3) + " s";
}

function makeLogCsvFileName(log) {
  let date = new Date(log.startDate || Date.now());
  if (Number.isNaN(date.getTime())) {
    date = new Date();
  }
  const pad = function(value) {
    return String(value).padStart(2, "0");
  };
  const stamp = date.getFullYear() + "-" + pad(date.getMonth() + 1) + "-" + pad(date.getDate()) + "_" + pad(date.getHours()) + "-" + pad(date.getMinutes()) + "-" + pad(date.getSeconds());
  return("smoke_log_" + log.id + "_" + stamp + ".csv");
}

function downloadLogCsv(logId) {
  const log = logHistory.find(function(item) {
    return Number(item.id) === Number(logId);
  });
  if (!log) {
    alert("Không tìm thấy phiên log cần xuất.");
    return;
  }
  const sampleCount = getLogSampleCount(log);
  if (sampleCount === 0) {
    alert("Phiên log này không có dữ liệu mẫu để xuất.");
    return;
  }
  const triggerOffsetMs = getTriggerOffsetMs(log);
  const rows = ["sample,time_s,time_from_trigger_s,IR_raw,MP2_AO_raw,MP2_DO"];
  for (let i = 0; i < sampleCount; i++) {
    const elapsedMs = Number(log.times[i]);
    const timeSeconds = elapsedMs / 1000;
    const triggerSeconds = (elapsedMs - triggerOffsetMs) / 1000;
    rows.push([i, timeSeconds.toFixed(3), triggerSeconds.toFixed(3), Number(log.irValues[i]), Number(log.gasAnalogRawValues[i]), Number(log.gasDigitalValues[i]) ? 1: 0].join(","));
  }
  const csvContent = "\uFEFF" + rows.join("\r\n");
  const blob = new Blob([csvContent], { type: "text/csv;charset=utf-8;" });
  const url = URL.createObjectURL(blob);
  const link = document.createElement("a");
  link.href = url;
  link.download = makeLogCsvFileName(log);
  document.body.appendChild(link);
  link.click();
  document.body.removeChild(link);
  setTimeout(function() {
    URL.revokeObjectURL(url);
  }, 1000);
}

function renderLogs() {
  const container = document.getElementById("logContainer");
  container.innerHTML = "";
  if (logHistory.length === 0) {
    container.innerHTML = "<div class='emptyLog'>Chưa có phiên đo nào.</div>";
    return;
  }
  logHistory.forEach(function(log) {
    const item = document.createElement("div"); item.className = "logItem"; const irCanvasId = "logIR_" + log.id; const aoCanvasId = "logAO_" + log.id; const doCanvasId = "logDO_" + log.id; const triggerOffsetMs = getTriggerOffsetMs(log); item.innerHTML = `
<div class="logTitleRow">
<div class="logTitle">
Phiên đo #${log.id}
</div>
<div class="logActions">
<button
class="downloadCsvBtn"
id="downloadCSV_${log.id}">
Tải dữ liệu CSV
</button>
</div>
</div>
<div class="logInfo">
<div class="logInfoItem">
<div class="logInfoLabel">
IR lớn nhất
</div>
<div class="logInfoValue">
${Number(log.maxIR).toFixed(0)}
</div>
</div>
<div class="logInfoItem">
<div class="logInfoLabel">
IR trung bình
</div>
<div class="logInfoValue">
${Number(log.averageIR).toFixed(1)}
</div>
</div>
<div class="logInfoItem">
<div class="logInfoLabel">
Gas AO lớn nhất (RAW)
</div>
<div class="logInfoValue">
${Number(log.maxGasAnalogRaw).toFixed(0)}
</div>
</div>
<div class="logInfoItem">
<div class="logInfoLabel">
Gas AO trung bình (RAW)
</div>
<div class="logInfoValue">
${Number(log.averageGasAnalogRaw).toFixed(1)}
</div>
</div>
<div class="logInfoItem">
<div class="logInfoLabel">
DO HIGH
</div>
<div class="logInfoValue">
${Number(log.digitalHighPercent).toFixed(1)}%
</div>
</div>
<div class="logInfoItem">
<div class="logInfoLabel">
Bắt đầu
</div>
<div class="logInfoValue">
${formatDate(log.startDate)}
</div>
</div>
<div class="logInfoItem">
<div class="logInfoLabel">
Trigger sau khi bắt đầu
</div>
<div class="logInfoValue">
${(triggerOffsetMs / 1000).toFixed(3)} s
</div>
</div>
<div class="logInfoItem">
<div class="logInfoLabel">
Kết thúc
</div>
<div class="logInfoValue">
${formatDate(log.endDate)}
</div>
</div>
<div class="logInfoItem">
<div class="logInfoLabel">
Thời lượng
</div>
<div class="logInfoValue">
${formatDuration(log.duration)}
</div>
</div>
<div class="logInfoItem">
<div class="logInfoLabel">
Số mẫu
</div>
<div class="logInfoValue">
${log.count}
</div>
</div>
</div>
<div class="triggerLegend">
Trục X dùng thời gian tương đối với trigger: trước trigger là số âm,
đường đứt đoạn “Trigger 0 s” là thời điểm IR hoặc MP-2 AO bắt đầu đạt ngưỡng và sau đó duy trì điều kiện đủ 5 giây.
Rê chuột lên bất kỳ đồ thị nào để xem đồng thời IR, MP-2 AO và MP-2 DO.
</div>
<div class="logChartGroup">
<div class="logChartBlock">
<div class="logChartTitle">
1. Photodiode IR — GPIO34
</div>
<canvas
id="${irCanvasId}"
class="logCanvas">
</canvas>
<div
id="${irCanvasId}_tooltip"
class="logTooltip">
</div>
</div>
<div class="logChartBlock">
<div class="logChartTitle">
2. Gas Analog AO — GPIO35 (RAW)
</div>
<canvas
id="${aoCanvasId}"
class="logCanvas">
</canvas>
<div
id="${aoCanvasId}_tooltip"
class="logTooltip">
</div>
</div>
<div class="logChartBlock">
<div class="logChartTitle">
3. Gas Digital DO — GPIO32
</div>
<canvas
id="${doCanvasId}"
class="logCanvas digitalCanvas">
</canvas>
<div
id="${doCanvasId}_tooltip"
class="logTooltip">
</div>
</div>
</div>
`; container.appendChild(item); const downloadButton = document.getElementById("downloadCSV_" + log.id); if (downloadButton) {
      downloadButton.onclick = function() {
        downloadLogCsv(log.id);
      };
    }
  });
  setTimeout(function() {
    redrawAllLogCharts(); setupAllLogHoverHandlers();
  }, 0);
}
const LOG_CHART_LEFT = 52;
const LOG_CHART_RIGHT = 15;
const LOG_CHART_TOP = 18;
const LOG_CHART_BOTTOM = 34;

function prepareLogCanvas(logCanvas) {
  const logCtx = logCanvas.getContext("2d");
  const rect = logCanvas.getBoundingClientRect();
  const ratio = window.devicePixelRatio || 1;
  logCanvas.width = rect.width * ratio;
  logCanvas.height = rect.height * ratio;
  logCtx.setTransform(ratio, 0, 0, ratio, 0, 0);
  return { ctx: logCtx, width: rect.width, height: rect.height, plotWidth: rect.width - LOG_CHART_LEFT - LOG_CHART_RIGHT, plotHeight: rect.height - LOG_CHART_TOP - LOG_CHART_BOTTOM };
}

function getLogLastTimeMs(log) {
  const count = getLogSampleCount(log);
  if (count === 0) {
    return 1;
  }
  const value = Number(log.times[count - 1]);
  if (!Number.isFinite(value) || value <= 0) {
    return 1;
  }
  return value;
}

function timeToLogX(timeMs, lastTimeMs, plotWidth) {
  return(LOG_CHART_LEFT + (timeMs / lastTimeMs) * plotWidth);
}

function drawTriggerMarker(ctx, width, height, plotWidth, plotHeight, lastTimeMs, triggerOffsetMs) {
  if (triggerOffsetMs < 0 || triggerOffsetMs > lastTimeMs) {
    return;
  }
  const x = timeToLogX(triggerOffsetMs, lastTimeMs, plotWidth);
  ctx.save();
  ctx.strokeStyle = "#ef4444";
  ctx.lineWidth = 1.5;
  ctx.setLineDash([6, 4]);
  ctx.beginPath();
  ctx.moveTo(x, LOG_CHART_TOP);
  ctx.lineTo(x, LOG_CHART_TOP + plotHeight);
  ctx.stroke();
  ctx.setLineDash([]);
  ctx.fillStyle = "#fca5a5";
  ctx.font = "11px Arial";
  ctx.fillText("Trigger 0 s", Math.min(x + 5, width - 78), LOG_CHART_TOP + 12);
  ctx.restore();
}

function drawLogXAxisLabels(ctx, width, height, lastTimeMs, triggerOffsetMs, plotWidth) {
  const leftRelative = (0 - triggerOffsetMs) / 1000;
  const rightRelative = (lastTimeMs - triggerOffsetMs) / 1000;
  ctx.fillStyle = "#9ca3af";
  ctx.font = "11px Arial";
  ctx.fillText(formatRelativeSeconds(leftRelative), LOG_CHART_LEFT, height - 8);
  const rightLabel = formatRelativeSeconds(rightRelative);
  const rightLabelWidth = ctx.measureText(rightLabel).width;
  ctx.fillText(rightLabel, Math.max(LOG_CHART_LEFT, width - LOG_CHART_RIGHT - rightLabelWidth), height - 8);
  if (triggerOffsetMs >= 0 && triggerOffsetMs <= lastTimeMs) {
    const triggerX = timeToLogX(triggerOffsetMs, lastTimeMs, plotWidth);
    const zeroLabel = "0 s";
    const zeroWidth = ctx.measureText(zeroLabel).width;
    ctx.fillStyle = "#fca5a5";
    ctx.fillText(zeroLabel, Math.max(LOG_CHART_LEFT, Math.min(width - LOG_CHART_RIGHT - zeroWidth, triggerX - zeroWidth / 2)), height - 8);
  }
}

function drawHoverVerticalLine(ctx, timeMs, lastTimeMs, plotWidth, plotHeight) {
  const x = timeToLogX(timeMs, lastTimeMs, plotWidth);
  ctx.save();
  ctx.strokeStyle = "#f3f4f6";
  ctx.lineWidth = 1;
  ctx.setLineDash([3, 3]);
  ctx.beginPath();
  ctx.moveTo(x, LOG_CHART_TOP);
  ctx.lineTo(x, LOG_CHART_TOP + plotHeight);
  ctx.stroke();
  ctx.restore();
  return x;
}

function drawAnalogLogChart(canvasId, log, values, yMin, yMax, unitText, lineColor, hoverIndex) {
  const logCanvas = document.getElementById(canvasId);
  if (!logCanvas) {
    return;
  }
  const geometry = prepareLogCanvas(logCanvas);
  const logCtx = geometry.ctx;
  const width = geometry.width;
  const height = geometry.height;
  const plotWidth = geometry.plotWidth;
  const plotHeight = geometry.plotHeight;
  logCtx.clearRect(0, 0, width, height);
  logCtx.strokeStyle = "#374151";
  logCtx.lineWidth = 1;
  logCtx.fillStyle = "#9ca3af";
  logCtx.font = "11px Arial";
  const gridCount = 4;
  for (let i = 0; i <= gridCount; i++) {
    const y = LOG_CHART_TOP + (plotHeight / gridCount) * i;
    logCtx.beginPath();
    logCtx.moveTo(LOG_CHART_LEFT, y);
    logCtx.lineTo(width - LOG_CHART_RIGHT, y);
    logCtx.stroke();
    const axisValue = yMax - ((yMax - yMin) / gridCount) * i;
    logCtx.fillText(Math.round(axisValue), 5, y + 4);
  }
  logCtx.fillText(unitText, 5, 12);
  const sampleCount = getLogSampleCount(log);
  if (sampleCount < 2 || !Array.isArray(values)) {
    return;
  }
  const lastTimeMs = getLogLastTimeMs(log);
  const triggerOffsetMs = getTriggerOffsetMs(log);
  logCtx.strokeStyle = lineColor;
  logCtx.lineWidth = 2;
  logCtx.lineJoin = "round";
  logCtx.lineCap = "round";
  logCtx.beginPath();
  for (let i = 0; i < sampleCount; i++) {
    const timeMs = Number(log.times[i]);
    const value = Number(values[i]);
    const x = timeToLogX(timeMs, lastTimeMs, plotWidth);
    const normalized = Math.max(0, Math.min(1, (value - yMin) / (yMax - yMin)));
    const y = LOG_CHART_TOP + plotHeight - normalized * plotHeight;
    if (i === 0) {
      logCtx.moveTo(x, y);
    }else {
      logCtx.lineTo(x, y);
    }
  }
  logCtx.stroke();
  drawTriggerMarker(logCtx, width, height, plotWidth, plotHeight, lastTimeMs, triggerOffsetMs);
  if (Number.isInteger(hoverIndex) && hoverIndex >= 0 && hoverIndex < sampleCount) {
    const hoverTimeMs = Number(log.times[hoverIndex]);
    const x = drawHoverVerticalLine(logCtx, hoverTimeMs, lastTimeMs, plotWidth, plotHeight);
    const hoverValue = Number(values[hoverIndex]);
    const normalized = Math.max(0, Math.min(1, (hoverValue - yMin) / (yMax - yMin)));
    const y = LOG_CHART_TOP + plotHeight - normalized * plotHeight;
    logCtx.fillStyle = lineColor;
    logCtx.beginPath();
    logCtx.arc(x, y, 4, 0, Math.PI * 2);
    logCtx.fill();
  }
  drawLogXAxisLabels(logCtx, width, height, lastTimeMs, triggerOffsetMs, plotWidth);
}

function drawDigitalLogChart(canvasId, log, hoverIndex) {
  const logCanvas = document.getElementById(canvasId);
  if (!logCanvas) {
    return;
  }
  const geometry = prepareLogCanvas(logCanvas);
  const logCtx = geometry.ctx;
  const width = geometry.width;
  const height = geometry.height;
  const plotWidth = geometry.plotWidth;
  const plotHeight = geometry.plotHeight;
  logCtx.clearRect(0, 0, width, height);
  const yHigh = LOG_CHART_TOP + plotHeight * 0.2;
  const yLow = LOG_CHART_TOP + plotHeight * 0.8;
  logCtx.strokeStyle = "#374151";
  logCtx.lineWidth = 1;
  logCtx.fillStyle = "#9ca3af";
  logCtx.font = "11px Arial";
  logCtx.beginPath();
  logCtx.moveTo(LOG_CHART_LEFT, yHigh);
  logCtx.lineTo(width - LOG_CHART_RIGHT, yHigh);
  logCtx.stroke();
  logCtx.beginPath();
  logCtx.moveTo(LOG_CHART_LEFT, yLow);
  logCtx.lineTo(width - LOG_CHART_RIGHT, yLow);
  logCtx.stroke();
  logCtx.fillText("1", 25, yHigh + 4);
  logCtx.fillText("0", 25, yLow + 4);
  const sampleCount = getLogSampleCount(log);
  if (sampleCount < 2) {
    return;
  }
  const lastTimeMs = getLogLastTimeMs(log);
  const triggerOffsetMs = getTriggerOffsetMs(log);
  logCtx.strokeStyle = "#a78bfa";
  logCtx.lineWidth = 2;
  logCtx.lineJoin = "miter";
  logCtx.beginPath();
  let previousY = Number(log.gasDigitalValues[0]) ? yHigh: yLow;
  logCtx.moveTo(LOG_CHART_LEFT, previousY);
  for (let i = 1; i < sampleCount; i++) {
    const x = timeToLogX(Number(log.times[i]), lastTimeMs, plotWidth);
    const currentY = Number(log.gasDigitalValues[i]) ? yHigh: yLow;
    logCtx.lineTo(x, previousY);
    if (currentY !== previousY) {
      logCtx.lineTo(x, currentY);
    }
    previousY = currentY;
  }
  logCtx.stroke();
  drawTriggerMarker(logCtx, width, height, plotWidth, plotHeight, lastTimeMs, triggerOffsetMs);
  if (Number.isInteger(hoverIndex) && hoverIndex >= 0 && hoverIndex < sampleCount) {
    const hoverTimeMs = Number(log.times[hoverIndex]);
    const x = drawHoverVerticalLine(logCtx, hoverTimeMs, lastTimeMs, plotWidth, plotHeight);
    const y = Number(log.gasDigitalValues[hoverIndex]) ? yHigh: yLow;
    logCtx.fillStyle = "#a78bfa";
    logCtx.beginPath();
    logCtx.arc(x, y, 4, 0, Math.PI * 2);
    logCtx.fill();
  }
  drawLogXAxisLabels(logCtx, width, height, lastTimeMs, triggerOffsetMs, plotWidth);
}

function drawSingleLogCharts(log, hoverIndex) {
  drawAnalogLogChart("logIR_" + log.id, log, log.irValues, IR_Y_MIN, IR_Y_MAX, "ADC", "#38bdf8", hoverIndex);
  drawAnalogLogChart("logAO_" + log.id, log, log.gasAnalogRawValues, GAS_RAW_MIN, GAS_RAW_MAX, "RAW", "#f59e0b", hoverIndex);
  drawDigitalLogChart("logDO_" + log.id, log, hoverIndex);
}

function redrawAllLogCharts() {
  for (const log of logHistory) {
    drawSingleLogCharts(log, null);
  }
}

function findNearestLogSampleIndex(log, canvas, mouseEvent) {
  const sampleCount = getLogSampleCount(log);
  if (sampleCount === 0) {
    return - 1;
  }
  const rect = canvas.getBoundingClientRect();
  const plotWidth = rect.width - LOG_CHART_LEFT - LOG_CHART_RIGHT;
  if (plotWidth <= 0) {
    return - 1;
  }
  const mouseX = mouseEvent.clientX - rect.left;
  const clampedX = Math.max(LOG_CHART_LEFT, Math.min(rect.width - LOG_CHART_RIGHT, mouseX));
  const lastTimeMs = getLogLastTimeMs(log);
  const targetTime = ((clampedX - LOG_CHART_LEFT) / plotWidth) * lastTimeMs;
  let low = 0;
  let high = sampleCount - 1;
  while (low < high) {
    const mid = Math.floor((low + high) / 2);
    if (Number(log.times[mid]) < targetTime) {
      low = mid + 1;
    }else {
      high = mid;
    }
  }
  if (low === 0) {
    return 0;
  }
  const previous = low - 1;
  const currentDistance = Math.abs(Number(log.times[low]) - targetTime);
  const previousDistance = Math.abs(Number(log.times[previous]) - targetTime);
  return previousDistance <= currentDistance ? previous: low;
}

function hideLogTooltips(logId) {
  const ids = ["logIR_" + logId + "_tooltip", "logAO_" + logId + "_tooltip", "logDO_" + logId + "_tooltip"];
  for (const id of ids) {
    const element = document.getElementById(id);
    if (element) {
      element.classList.remove("active");
    }
  }
}

function showLogTooltip(log, canvas, sampleIndex) {
  hideLogTooltips(log.id);
  const tooltip = document.getElementById(canvas.id + "_tooltip");
  if (!tooltip) {
    return;
  }
  const triggerOffsetMs = getTriggerOffsetMs(log);
  const elapsedMs = Number(log.times[sampleIndex]);
  const relativeSeconds = (elapsedMs - triggerOffsetMs) / 1000;
  const digitalValue = Number(log.gasDigitalValues[sampleIndex]) ? 1: 0;
  tooltip.innerHTML = `
<div class="logTooltipTitle">
Mẫu #${sampleIndex}
</div>
<div>So với trigger: <strong>${formatRelativeSeconds(relativeSeconds)}</strong></div>
<div>Từ đầu log: <strong>${(elapsedMs / 1000).toFixed(3)} s</strong></div>
<div>IR GPIO34: <strong>${Number(log.irValues[sampleIndex]).toFixed(0)}</strong></div>
<div>MP-2 AO GPIO35: <strong>${Number(log.gasAnalogRawValues[sampleIndex]).toFixed(0)}</strong></div>
<div>MP-2 DO GPIO32: <strong>${digitalValue} (${digitalValue ? "HIGH" : "LOW"})</strong></div>
`;
  tooltip.classList.add("active");
}

function setupLogHoverHandlers(log) {
  const canvasIds = ["logIR_" + log.id, "logAO_" + log.id, "logDO_" + log.id];
  for (const canvasId of canvasIds) {
    const canvas = document.getElementById(canvasId);
    if (!canvas) {
      continue;
    }
    canvas.onmousemove = function(event) {
      const sampleIndex = findNearestLogSampleIndex(log, canvas, event);
      if (sampleIndex < 0) {
        return;
      }
      drawSingleLogCharts(log, sampleIndex);
      showLogTooltip(log, canvas, sampleIndex);
    };
    canvas.onmouseleave = function() {
      hideLogTooltips(log.id);
      drawSingleLogCharts(log, null);
    };
  }
}

function setupAllLogHoverHandlers() {
  for (const log of logHistory) {
    setupLogHoverHandlers(log);
  }
}

function togglePause() {
  paused = !paused;
  const button = document.getElementById("pauseBtn");
  if (paused) {
    button.innerText = "Tiếp tục đồ thị";
  }else {
    button.innerText = "Tạm dừng đồ thị";
  }
}

function clearLiveData() {
  liveIRData = [];
  liveGasAnalogRawData = [];
  liveGasDigitalData = [];
  drawIRRealtimeChart();
  drawGasAnalogRealtimeChart();
  drawGasDigitalRealtimeChart();
}

function clearLogs() {
  const ok = confirm("Bạn có chắc muốn xóa toàn bộ nhật ký đo?");
  if (!ok) {
    return;
  }
  logHistory = [];
  localStorage.removeItem(STORAGE_KEY);
  renderLogs();
}
const source = new EventSource("/events");
source.onopen = function() {
  setConnectionStatus("ĐANG CHẠY", "#22c55e");
};
source.onerror = function() {
  setConnectionStatus("MẤT KẾT NỐI", "#ef4444");
};
source.addEventListener("adc", function(event) {
  try {
    const packet = JSON.parse(event.data); const irValue = Number(packet.ir); const gasAnalogValue = Number(packet.gasAO); const gasDigitalValue = Number(packet.gasDO); const espTime = Number(packet.time); if (!Number.isFinite(irValue) || !Number.isFinite(gasAnalogValue) || !Number.isFinite(gasDigitalValue) || !Number.isFinite(espTime)) {
      return;
    }
    addLiveValue(irValue, gasAnalogValue, gasDigitalValue); processMeasurement(irValue, gasAnalogValue, gasDigitalValue, espTime);
  }catch (error) {
    console.warn("Loi du lieu SSE:", error);
  }
});
document.getElementById("recordInfo").innerText = "Bắt đầu khi IR hoặc MP-2 AO ≥ " + START_THRESHOLD + " liên tục " + (START_HOLD_TIME / 1000).toFixed(1) + " giây. Log vẫn giữ " + (PRE_TRIGGER_TIME / 1000).toFixed(1) + " giây trước trigger; kết thúc khi cả IR và MP-2 AO ≤ " + STOP_THRESHOLD + " liên tục " + (POST_TRIGGER_TIME / 1000).toFixed(1) + " giây.";
loadLogs();
renderLogs();
resizeRealtimeCharts();
setRecordStatus(false);
</script>
</body>
</html>)rawliteral";

// ============================================================================
// HÀM KẾT NỐI WIFI LẦN ĐẦU
// ============================================================================

void connectWiFi()
{
    Serial.println();
    Serial.println("========================================");
    Serial.print("Dang ket noi WiFi: ");
    Serial.println(WIFI_SSID);

    // ESP32 hoạt động như một thiết bị kết nối vào router/hotspot.
    WiFi.mode(WIFI_STA);

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );

    // Chờ kết nối.
    while (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        delay(500);
        Serial.print(".");
    }

    Serial.println();
    Serial.println("Da ket noi WiFi.");

    Serial.print("Dia chi IP: ");
    Serial.println(
        WiFi.localIP()
    );

    Serial.println("========================================");
}

// ============================================================================
// HÀM THỬ KẾT NỐI LẠI WIFI
// ============================================================================

void reconnectWiFiIfNeeded(
    unsigned long currentTime
)
{
    // Nếu Wi-Fi vẫn còn kết nối thì không làm gì.
    if (
        WiFi.status() ==
        WL_CONNECTED
    )
    {
        return;
    }

    // Không reconnect liên tục.
    if (
        currentTime -
        lastWiFiReconnectTime <
        WIFI_RECONNECT_INTERVAL_MS
    )
    {
        return;
    }

    lastWiFiReconnectTime =
        currentTime;

    Serial.println(
        "Mat WiFi. Dang thu ket noi lai..."
    );

    WiFi.disconnect();

    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );
}

// ============================================================================
// SETUP
// ============================================================================

void setup()
{
    // ------------------------------------------------------------------------
    // SERIAL MONITOR
    // ------------------------------------------------------------------------

    Serial.begin(
        115200
    );

    delay(
        500
    );

    // ------------------------------------------------------------------------
    // ADC
    // ------------------------------------------------------------------------

    pinMode(
        IR_PIN,
        INPUT
    );

    pinMode(
        GAS_AO_PIN,
        INPUT
    );

    pinMode(
        GAS_DO_PIN,
        INPUT
    );

    // ADC 12 bit => giá trị từ 0 đến 4095.
    analogReadResolution(
        ADC_RESOLUTION_BITS
    );

    // Cấu hình attenuation cho cả hai kênh ADC1.
    // Dù vậy, điện áp thực tế đưa vào GPIO34/GPIO35 vẫn phải nằm
    // trong giới hạn an toàn của ESP32.
    analogSetPinAttenuation(
        IR_PIN,
        ADC_11db
    );

    analogSetPinAttenuation(
        GAS_AO_PIN,
        ADC_11db
    );

    // ------------------------------------------------------------------------
    // WIFI
    // ------------------------------------------------------------------------

    connectWiFi();

    // ------------------------------------------------------------------------
    // ROUTE TRANG CHÍNH
    // ------------------------------------------------------------------------

    server.on(
        "/",
        HTTP_GET,
        [](AsyncWebServerRequest* request)
        {
            request->send_P(
                200,
                "text/html",
                PAGE
            );
        }
    );

    // ------------------------------------------------------------------------
    // KHI TRÌNH DUYỆT KẾT NỐI VÀO SSE
    // ------------------------------------------------------------------------

    events.onConnect(
        [](AsyncEventSourceClient* client)
        {
            Serial.println(
                "Web client da ket noi SSE."
            );

            // Gửi một gói để trình duyệt biết kết nối đã sẵn sàng.
            // reconnect = 1000 ms: nếu mất kết nối, browser thử reconnect sau 1 s.
            client->send(
                "connected",
                NULL,
                millis(),
                1000
            );
        }
    );

    // Gắn SSE handler vào Web Server.
    server.addHandler(
        &events
    );

    // ------------------------------------------------------------------------
    // KHỞI ĐỘNG WEB SERVER
    // ------------------------------------------------------------------------

    server.begin();

    Serial.println(
        "Web Server da khoi dong."
    );

    Serial.print(
        "Mo trinh duyet tai: http://"
    );

    Serial.println(
        WiFi.localIP()
    );
}

// ============================================================================
// LOOP
// ============================================================================

void loop()
{
    const unsigned long currentTime =
        millis();

    // ------------------------------------------------------------------------
    // KIỂM TRA / KẾT NỐI LẠI WIFI
    // ------------------------------------------------------------------------

    reconnectWiFiIfNeeded(
        currentTime
    );

    // Nếu chưa có Wi-Fi thì không gửi dữ liệu web.
    if (
        WiFi.status() !=
        WL_CONNECTED
    )
    {
        return;
    }

    // ------------------------------------------------------------------------
    // KIỂM TRA ĐÃ ĐẾN CHU KỲ LẤY MẪU CHƯA
    // ------------------------------------------------------------------------

    if (
        currentTime -
        lastSampleTime <
        SAMPLE_INTERVAL_MS
    )
    {
        return;
    }

    lastSampleTime =
        currentTime;

    // ------------------------------------------------------------------------
    // ĐỌC HAI KÊNH ADC
    // ------------------------------------------------------------------------

    // Tín hiệu photodiode hồng ngoại.
    const int irValue =
        analogRead(
            IR_PIN
        );

    // Tín hiệu Analog Output (AO) raw từ cảm biến MP-2/MQ-2.
    const int gasAnalogValue =
        analogRead(
            GAS_AO_PIN
        );

    // Tín hiệu Digital Output (DO) từ comparator trên module.
    // Kết quả chỉ là LOW (0) hoặc HIGH (1).
    const int gasDigitalValue =
        digitalRead(
            GAS_DO_PIN
        );

    // ------------------------------------------------------------------------
    // TẠO GÓI JSON
    //
    // Ví dụ:
    // {"ir":1520,"gasAO":875,"gasDO":1,"time":123456}
    //
    // ir    : RAW ADC photodiode GPIO34, 0..4095
    // gasAO : RAW ADC chân AO GPIO35, 0..4095
    // gasDO : RAW digital chân DO GPIO32, 0 hoặc 1
    // time  : millis() của ESP32, đơn vị ms
    // ------------------------------------------------------------------------

    char json[96];

    snprintf(
        json,
        sizeof(json),
        "{\"ir\":%d,\"gasAO\":%d,\"gasDO\":%d,\"time\":%lu}",
        irValue,
        gasAnalogValue,
        gasDigitalValue,
        currentTime
    );

    // ------------------------------------------------------------------------
    // STREAM GÓI DỮ LIỆU TỚI TẤT CẢ TRÌNH DUYỆT ĐANG KẾT NỐI
    // ------------------------------------------------------------------------

    events.send(
        json,
        "adc",
        currentTime
    );

    // ------------------------------------------------------------------------
    // IN GIÁ TRỊ RA SERIAL MONITOR
    // ------------------------------------------------------------------------

    if (
        currentTime -
        lastSerialPrintTime >=
        SERIAL_PRINT_INTERVAL_MS
    )
    {
        lastSerialPrintTime =
            currentTime;

        Serial.print(
            "IR ADC = "
        );

        Serial.print(
            irValue
        );

        Serial.print(
            " | GAS AO = "
        );

        Serial.print(
            gasAnalogValue
        );

        Serial.print(
            " | GAS DO = "
        );

        Serial.println(
            gasDigitalValue
        );
    }

}
