import edge_tts
import asyncio
import tempfile
import os
from pydub import AudioSegment
from flask import Flask, request, Response
import requests
from faster_whisper import WhisperModel

app = Flask(__name__)

OLLAMA_API_URL = "http://localhost:11434/api/generate"
TTS_VOICE = "zh-CN-XiaoxiaoNeural"
MODEL_NAME = "qwen2.5:1.5b"

# ---------- 加载 ASR 模型 ----------
print("加载语音识别模型...")
asr_model = WhisperModel("base", device="cpu", compute_type="int8")
print("✅ ASR 模型就绪")

def text_to_speech_pcm_wav(text, target_sample_rate=24000):
    tmp_in = tempfile.NamedTemporaryFile(suffix=".mp3", delete=False)
    tmp_in.close()
    temp_filename = tmp_in.name
    try:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        loop.run_until_complete(edge_tts.Communicate(text, TTS_VOICE).save(temp_filename))
        loop.close()

        audio = AudioSegment.from_file(temp_filename)
        audio = (audio
                 .set_frame_rate(target_sample_rate)
                 .set_channels(1)
                 .set_sample_width(2))
        wav_bytes = audio.export(format="wav").read()
        print(f"✅ TTS 转换完成: {len(wav_bytes)} 字节")
        return wav_bytes
    except Exception as e:
        print(f"❌ TTS 转换失败: {e}")
        return None
    finally:
        if os.path.exists(temp_filename):
            os.remove(temp_filename)

# ---------- 原有接口 ----------
@app.route('/ask', methods=['POST'])
def ask_ai():
    data = request.get_json()
    if not data or 'text' not in data:
        return {"error": "请提供 'text' 字段"}, 400

    user_text = data['text']
    print(f"📝 收到提问: {user_text}")

    ollama_payload = {
    "model": MODEL_NAME,
    "prompt": f"请用简短的中文回答，不超过30个字。问题：{user_text}",
    "stream": False,
    "max_tokens": 80,          # 限制生成 token 数，80 个 token 大约对应 30~40 个中文字
    "temperature": 0.7
}
    try:
        resp = requests.post(OLLAMA_API_URL, json=ollama_payload, timeout=120)
        resp.raise_for_status()
        ai_response = resp.json().get("response", "对不起，我没有理解。")
        print(f"🤖 AI 回答: {ai_response}")
    except Exception as e:
        print(f"❌ Ollama 调用失败: {e}")
        ai_response = "AI 服务暂时不可用，请稍后再试。"

    audio_bytes = text_to_speech_pcm_wav(ai_response, target_sample_rate=24000)
    if audio_bytes is None:
        return {"error": "音频生成失败"}, 500
    return Response(audio_bytes, mimetype="audio/wav")

@app.route('/asr', methods=['POST'])
def asr():
    audio_bytes = request.get_data()
    if not audio_bytes:
        return {"error": "没有音频数据"}, 400

    tmp_path = "/tmp/asr_input.wav"
    with open(tmp_path, "wb") as f:
        f.write(audio_bytes)

    try:
        segments, info = asr_model.transcribe(tmp_path, beam_size=5, language="zh", vad_filter=True)
        text = "".join([seg.text for seg in segments])
        print(f"🗣️ ASR 结果: {text}")
        return {"text": text.strip()}
    except Exception as e:
        print(f"ASR 错误: {e}")
        return {"error": "语音识别失败"}, 500
    finally:
        if os.path.exists(tmp_path):
            os.remove(tmp_path)

# ---------- 新增接口 ----------
@app.route('/wakeup', methods=['GET'])
def wakeup():
    text = "你好，主人，有什么需要帮助的吗？"
    audio = text_to_speech_pcm_wav(text, target_sample_rate=24000)
    if audio is None:
        return {"error": "TTS失败"}, 500
    return Response(audio, mimetype="audio/wav")

@app.route('/temperature', methods=['POST'])
def temperature():
    data = request.get_json()
    if not data:
        return {"error": "没有数据"}, 400
    temp = data.get("temp")
    hum = data.get("humidity")
    if temp is None or hum is None:
        return {"error": "参数缺失"}, 400
    if temp < -50:  # 读取失败标志
        reply = "传感器读取失败，请稍后再试。"
    else:
        reply = f"当前温度{temp:.1f}摄氏度，湿度{hum:.1f}%。"
    audio = text_to_speech_pcm_wav(reply, target_sample_rate=24000)
    if audio is None:
        return {"error": "TTS失败"}, 500
    return Response(audio, mimetype="audio/wav")

if __name__ == '__main__':
    try:
        requests.post(OLLAMA_API_URL,
                      json={"model": MODEL_NAME, "prompt": "", "stream": False, "max_tokens": 1},
                      timeout=10)
        print("✅ 模型预热完成")
    except Exception:
        pass
    app.run(host='0.0.0.0', port=5000, debug=True)
