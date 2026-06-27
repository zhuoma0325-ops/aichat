# ESP32 AI 语音助手

基于 ESP32-S3 的离线+在线混合 AI 语音交互系统。

## 功能

- 🎤 **语音识别** — INMP441 麦克风 → faster-whisper base 中文识别
- 🧠 **AI 对话** — Ollama qwen2.5:1.5b 本地推理
- 🔊 **语音合成** — edge-tts (XiaoxiaoNeural) 自然语音输出
- 🌡️ **传感器** — DHT22 温湿度查询

## 硬件

| 组件 | 型号 | 引脚 |
|------|------|------|
| 主控 | ESP32-S3 (4D Systems GEN4 R8N16) | — |
| 麦克风 | INMP441 (I2S) | BCLK=4, LRC=5, DOUT=18 |
| 功放 | MAX98357A (I2S) | BCLK=15, LRC=16, DIN=17 |
| 温湿度 | DHT22 | PIN=48 |

## 架构

```
[INMP441] → I2S → ESP32-S3 → WiFi → Flask 后端 (:5000)
                                         ├── /asr → faster-whisper
                                         ├── /ask → Ollama qwen2.5
                                         ├── /temperature → TTS
                                         └── /wakeup → TTS
                                    ← WAV 音频流 ←
[MAX98357A] ← I2S ← ESP32-S3 ←─────────┘
```

## 使用

### 1. 启动后端

```bash
cd server
python serv.py
```

需要：
- Ollama 运行中，模型 `qwen2.5:1.5b`
- Python 依赖：flask, faster-whisper, edge-tts, pydub, requests

### 2. 烧录 ESP32

```bash
pio run -t upload
```

### 3. 交互

对着麦克风说话：
- "**小屋**" → 唤醒词，AI 打招呼
- "**今天温度**" → 查询 DHT22 读数
- 其他任意问题 → AI 对话

## 文件结构

```
aichat/
├── src/main.cpp          # ESP32 固件（Arduino）
├── server/serv.py        # Flask 后端
├── platformio.ini        # PlatformIO 配置
└── test/                 # 测试文档
```
