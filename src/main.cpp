#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "AudioTools.h"
#include "esp_task_wdt.h"
#include <vector>
#include <driver/i2s.h>
#include <DHT.h>

// ================= 配置参数 =================
const char* ssid       = "TP-LINK_2540";
const char* password   = "WDZ2004cn";
const char* server_ip  = "192.168.0.104";
const int   server_port = 5000;

// 播放 I2S 引脚（接 MAX98357A）
#define I2S_BCLK   15
#define I2S_LRC    16
#define I2S_DIN    17
#define SD_PIN     11

// 录音 I2S 引脚（接 INMP441）
#define I2S_MIC_BCLK  4
#define I2S_MIC_LRC   5
#define I2S_MIC_DOUT  18

// DHT22 引脚
#define DHTPIN 48
#define DHTTYPE DHT22

// 录音参数
const int RECORD_SAMPLE_RATE = 16000;
const int RECORD_BITS        = 16;
const int RECORD_CHANNELS    = 1;
const int RECORD_DURATION_MS = 10000;
const int SILENCE_THRESHOLD  = 500;
const int SILENCE_TIME_MS    = 800;
// =============================================

// 对象
I2SStream i2sOut;
bool i2sOutReady = false;
DHT dht(DHTPIN, DHTTYPE);

// 录音状态
std::vector<uint8_t> pcmBuffer;
bool isRecording = false;
uint32_t recordStartMs = 0;
uint32_t lastSoundMs = 0;

// 函数声明
void askAndPlay(const String& question);
void processRecordedAudio();
void playWavFromStream(WiFiClient* s);
void callWakeup();
void callTemperature();
String getTemperatureJson();

// ===================== 播放辅助函数 =====================
bool safeRead(WiFiClient* s, uint8_t* buf, size_t len, uint32_t timeout_ms) {
  size_t done = 0;
  uint32_t start = millis();
  while (done < len && millis() - start < timeout_ms) {
    esp_task_wdt_reset();
    if (s->available()) {
      done += s->readBytes(buf + done, len - done);
    } else if (!s->connected()) {
      return false;
    } else {
      delay(5);
    }
  }
  return done == len;
}

uint32_t parseWavHeader(WiFiClient* s) {
  uint8_t h[8];
  if (!safeRead(s, h, 4, 5000)) return 0;
  if (memcmp(h, "RIFF", 4)) { Serial.println("✗ 非 RIFF"); return 0; }
  if (!safeRead(s, h, 4, 5000)) return 0;
  if (!safeRead(s, h, 4, 5000)) return 0;
  if (memcmp(h, "WAVE", 4)) { Serial.println("✗ 非 WAVE"); return 0; }

  while (s->connected()) {
    esp_task_wdt_reset();
    if (!safeRead(s, h, 4, 5000)) break;
    if (!safeRead(s, h + 4, 4, 5000)) break;
    uint32_t sz = (uint32_t)h[4] | ((uint32_t)h[5] << 8) |
                  ((uint32_t)h[6] << 16) | ((uint32_t)h[7] << 24);

    if (!memcmp(h, "fmt ", 4)) {
      uint8_t fm[16];
      uint32_t rs = min((uint32_t)16, sz);
      if (!safeRead(s, fm, rs, 5000)) break;
      Serial.printf("WAV: %s %dch %luHz %dbit\n",
                    (fm[0]|(fm[1]<<8))==1?"PCM":"?",
                    fm[2]|(fm[3]<<8),
                    (uint32_t)fm[4]|((uint32_t)fm[5]<<8)|((uint32_t)fm[6]<<16)|((uint32_t)fm[7]<<24),
                    fm[14]|(fm[15]<<8));
      uint32_t left = sz - rs;
      while (left) { esp_task_wdt_reset(); size_t n = s->readBytes(h, min((size_t)left, sizeof(h))); if(n==0)break; left-=n; }
    } else if (!memcmp(h, "data", 4)) {
      Serial.printf("DATA: %u bytes\n", sz);
      return sz;
    } else {
      uint32_t left = sz;
      while (left) { esp_task_wdt_reset(); size_t n = s->readBytes(h, min((size_t)left, sizeof(h))); if(n==0)break; left-=n; }
    }
  }
  return 0;
}

void playWavFromStream(WiFiClient* s) {
  if (!s || !i2sOutReady) return;
  uint32_t dsz = parseWavHeader(s);
  if (dsz == 0) return;

  uint8_t buf[256];
  uint32_t written = 0, deadline = millis() + 60000;
  while (written < dsz && millis() < deadline) {
    esp_task_wdt_reset();
    if (s->available() == 0) { if(!s->connected()) break; delay(5); continue; }
    size_t n = s->readBytes(buf, min(sizeof(buf), (size_t)(dsz - written)));
    if (n == 0) continue;
    i2sOut.write(buf, n);
    written += n;
  }
  Serial.printf("播放 %u/%u B\n", written, dsz);
}

// ===================== HTTP 请求函数 =====================
void askAndPlay(const String& question) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin("http://" + String(server_ip) + ":" + String(server_port) + "/ask");
  http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(10000);
  http.setTimeout(150000);

  JsonDocument doc;
  doc["text"] = question;
  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  if (code == 200) {
    WiFiClient* s = http.getStreamPtr();
    playWavFromStream(s);
  }
  http.end();
}

void callWakeup() {
  HTTPClient http;
  http.begin("http://" + String(server_ip) + ":" + String(server_port) + "/wakeup");
  http.setTimeout(10000);
  int code = http.GET();
  if (code == 200) {
    WiFiClient* s = http.getStreamPtr();
    playWavFromStream(s);
  }
  http.end();
}

void callTemperature() {
  String jsonData = getTemperatureJson();
  HTTPClient http;
  http.begin("http://" + String(server_ip) + ":" + String(server_port) + "/temperature");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(10000);
  int code = http.POST(jsonData);
  if (code == 200) {
    WiFiClient* s = http.getStreamPtr();
    playWavFromStream(s);
  }
  http.end();
}

// ===================== 录音后处理 =====================
void processRecordedAudio() {
  if (pcmBuffer.empty()) return;

  uint32_t dataSize = pcmBuffer.size();
  uint8_t header[44] = {0};
  header[0]='R'; header[1]='I'; header[2]='F'; header[3]='F';
  uint32_t fileSize = 36 + dataSize;
  memcpy(header+4, &fileSize, 4);
  header[8]='W'; header[9]='A'; header[10]='V'; header[11]='E';
  header[12]='f'; header[13]='m'; header[14]='t'; header[15]=' ';
  uint32_t fmtSize = 16;
  memcpy(header+16, &fmtSize, 4);
  uint16_t audioFormat = 1;
  memcpy(header+20, &audioFormat, 2);
  uint16_t numChannels = RECORD_CHANNELS;
  memcpy(header+22, &numChannels, 2);
  uint32_t sampleRate = RECORD_SAMPLE_RATE;
  memcpy(header+24, &sampleRate, 4);
  uint32_t byteRate = RECORD_SAMPLE_RATE * RECORD_CHANNELS * (RECORD_BITS/8);
  memcpy(header+28, &byteRate, 4);
  uint16_t blockAlign = RECORD_CHANNELS * (RECORD_BITS/8);
  memcpy(header+32, &blockAlign, 2);
  uint16_t bitsPerSample = RECORD_BITS;
  memcpy(header+34, &bitsPerSample, 2);
  header[36]='d'; header[37]='a'; header[38]='t'; header[39]='a';
  memcpy(header+40, &dataSize, 4);

  std::vector<uint8_t> wavData;
  wavData.insert(wavData.end(), header, header + 44);
  wavData.insert(wavData.end(), pcmBuffer.begin(), pcmBuffer.end());

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 未连接，无法识别语音");
    pcmBuffer.clear();
    return;
  }

  HTTPClient http;
  http.begin("http://" + String(server_ip) + ":" + String(server_port) + "/asr");
  http.addHeader("Content-Type", "application/octet-stream");
  http.setTimeout(10000);

  int code = http.POST(wavData.data(), wavData.size());
  if (code == 200) {
    String response = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, response);
    if (!err) {
      String question = doc["text"].as<String>();
      Serial.printf("🗣️ 识别结果: %s\n", question.c_str());
      if (!question.isEmpty()) {
    // 唤醒词分支（支持多种变体）
    if (question.indexOf("小屋") >= 0 ||
        question.indexOf("嘿小屋") >= 0 ||
        question.indexOf("小屋小屋") >= 0) {
        Serial.println("唤醒词检测到");
        callWakeup();
    }
    // 温度查询分支（保持不变）
    else if (question.indexOf("温度") >= 0 &&
             (question.indexOf("今天") >= 0 || question.indexOf("当前") >= 0)) {
        Serial.println("温度查询检测到");
        callTemperature();
    }
    // 其它问题走 AI
    else {
        askAndPlay(question);
    }
}
    } else {
      Serial.println("ASR 返回 JSON 解析失败");
    }
  } else {
    Serial.printf("ASR 上传失败: HTTP %d\n", code);
  }
  http.end();
  pcmBuffer.clear();
}

// ===================== 传感器 =====================
String getTemperatureJson() {
  float temp = dht.readTemperature();
  float hum = dht.readHumidity();
  if (isnan(temp) || isnan(hum)) {
    Serial.println("DHT22 读取失败");
    return "{\"temp\":-100,\"humidity\":-1}";
  }
  JsonDocument d;
  d["temp"] = temp;
  d["humidity"] = hum;
  String out;
  serializeJson(d, out);
  return out;
}

// ===================== 初始化 =====================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== ESP32 智能语音助手 ===");

  esp_task_wdt_init(30, false);
  esp_task_wdt_add(NULL);

  pinMode(SD_PIN, OUTPUT);
  digitalWrite(SD_PIN, LOW);
  delay(10);

  // 播放 I2S
  Serial.print("播放 I2S...");
  auto cfgOut = i2sOut.defaultConfig(TX_MODE);
  cfgOut.pin_bck = I2S_BCLK;
  cfgOut.pin_ws  = I2S_LRC;
  cfgOut.pin_data = I2S_DIN;
  cfgOut.sample_rate = 24000;
  cfgOut.channels = 1;
  cfgOut.bits_per_sample = 16;
  cfgOut.i2s_format = I2S_STD_FORMAT;
  cfgOut.is_master = true;
  cfgOut.use_apll = true;
  if (i2sOut.begin(cfgOut)) { i2sOutReady = true; Serial.println("OK"); }
  else Serial.println("FAIL");

  // 录音 I2S（原生）
  Serial.print("录音 I2S...");
  i2s_config_t i2s_mic_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = RECORD_SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 512
  };
  i2s_pin_config_t mic_pin_config = {
    .bck_io_num = I2S_MIC_BCLK,
    .ws_io_num = I2S_MIC_LRC,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_MIC_DOUT
  };
  if (i2s_driver_install(I2S_NUM_1, &i2s_mic_config, 0, NULL) != ESP_OK ||
      i2s_set_pin(I2S_NUM_1, &mic_pin_config) != ESP_OK) {
    Serial.println("FAIL");
    while (1);
  }
  Serial.println("OK");

  // DHT22
  dht.begin();
  Serial.println("DHT22 已启动");

  // WiFi
  Serial.print("WiFi");
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.begin(ssid, password);
  WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
    if(event == SYSTEM_EVENT_STA_DISCONNECTED) {
      Serial.printf("\n[WiFi] 断开, 原因: %d\n", info.wifi_sta_disconnected.reason);
    } else if(event == SYSTEM_EVENT_STA_CONNECTED) {
      Serial.println("[WiFi] 已连接");
    }
  });
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 30) {
    delay(500); Serial.print("."); tries++;
    esp_task_wdt_reset();
  }
  if (WiFi.status() == WL_CONNECTED) Serial.println("\n" + WiFi.localIP().toString());
  else Serial.println("\nWiFi FAIL");

  digitalWrite(SD_PIN, HIGH);
  Serial.println("就绪 —— 请对着麦克风说话");
  esp_task_wdt_reset();
}

void loop() {
  esp_task_wdt_reset();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 断开，尝试重连...");
    WiFi.reconnect();
    for (int i = 0; i < 20; i++) {
      delay(500); esp_task_wdt_reset();
      if (WiFi.status() == WL_CONNECTED) { Serial.println("WiFi 已重连"); break; }
    }
  }

  const int bufferSize = 512;
  int16_t sampleBuffer[bufferSize];
  size_t bytesRead = 0;

  esp_err_t err = i2s_read(I2S_NUM_1, sampleBuffer, bufferSize * sizeof(int16_t), &bytesRead, pdMS_TO_TICKS(100));
  if (err != ESP_OK || bytesRead == 0) { delay(5); return; }

  int samples = bytesRead / sizeof(int16_t);
  long sumSquare = 0;
  for (int i = 0; i < samples; i++) sumSquare += (int32_t)sampleBuffer[i] * sampleBuffer[i];
  float rms = sqrt((float)sumSquare / samples);
  uint32_t now = millis();

  if (rms > SILENCE_THRESHOLD) {
    if (!isRecording) {
      isRecording = true;
      recordStartMs = now;
      pcmBuffer.clear();
      Serial.println("🔴 检测到声音，开始录音...");
    }
    lastSoundMs = now;
  }

  if (isRecording) {
    pcmBuffer.insert(pcmBuffer.end(), (uint8_t*)sampleBuffer, (uint8_t*)sampleBuffer + bytesRead);
    if (now - recordStartMs > RECORD_DURATION_MS) {
      isRecording = false;
      Serial.println("⏰ 录音超时，自动结束");
      processRecordedAudio();
    }
  }

  if (isRecording && (now - lastSoundMs > SILENCE_TIME_MS)) {
    isRecording = false;
    uint32_t duration = now - recordStartMs;
    Serial.printf("⏹️ 录音结束，时长: %u ms\n", duration);
    if (duration > 500) processRecordedAudio();
    else { Serial.println("⚠️ 录音太短，忽略"); pcmBuffer.clear(); }
  }

  delay(5);
}