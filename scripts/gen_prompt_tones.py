#!/usr/bin/env python3
"""为无屏按键语音助手生成本地提示音 WAV（16kHz / 16bit / 单声道）。

说明：这些是"可区分的事件提示音"，用于配网/错误等无网络、无法调用服务端
TTS 的场景。后续若需要真人语音播报，可把同名文件替换为对应的 WAV/OGG，
播放器（framework/audio_prompt_player.cc）不需要改动。

用法：python scripts/gen_prompt_tones.py
输出：main/assets/prompts/*.wav
"""
import os
import math
import struct
import wave

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "..", "main", "assets", "prompts")
SAMPLE_RATE = 16000
AMPLITUDE = 0.22  # 音量不要太响，避免吓人/破音


def tone(freq, dur_ms, amp=AMPLITUDE):
    """生成一段正弦音（带 8ms 淡入淡出避免爆音）。"""
    n = int(SAMPLE_RATE * dur_ms / 1000)
    fade = int(SAMPLE_RATE * 0.008)
    out = []
    for i in range(n):
        t = i / SAMPLE_RATE
        v = math.sin(2 * math.pi * freq * t) * amp
        if i < fade:
            v *= i / fade
        elif i > n - fade:
            v *= (n - i) / fade
        out.append(int(v * 32767))
    return out


def silence(dur_ms):
    return [0] * int(SAMPLE_RATE * dur_ms / 1000)


def concat(*parts):
    out = []
    for p in parts:
        out.extend(p)
    return out


def save(name, samples):
    path = os.path.join(OUT_DIR, name)
    os.makedirs(OUT_DIR, exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(b"".join(struct.pack("<h", s) for s in samples))
    print(f"generated {name} ({len(samples) / SAMPLE_RATE:.2f}s)")


def pkg(name, samples):
    save(name, samples)


# 1) 进入配网：双击上升音
pkg("pkg_enter_config.wav", concat(
    tone(660, 180), silence(60), tone(880, 220)))

# 2) 等待配置：低沉双音
pkg("pkg_wait_network.wav", concat(
    tone(330, 200), silence(120), tone(330, 200)))

# 3) 配网成功：三连上升音
pkg("pkg_network_success.wav", concat(
    tone(523, 140), silence(40), tone(659, 140), silence(40), tone(784, 200)))

# 4) 配网失败：两连下降音
pkg("pkg_network_fail.wav", concat(
    tone(392, 240), silence(60), tone(311, 300)))

# 5) 请先配置服务端：交替高低音
pkg("pkg_need_service.wav", concat(
    tone(880, 120), silence(40), tone(660, 120), silence(40), tone(880, 180)))

# 6) 登录失败：三连低音
pkg("pkg_login_fail.wav", concat(
    tone(262, 180), silence(50), tone(262, 180), silence(50), tone(262, 240)))

# 7) 请等待当前回答：单中音
pkg("pkg_wait_answer.wav", concat(tone(440, 300)))

# 8) 没有听清：两短高音
pkg("pkg_no_speech.wav", concat(
    tone(988, 100), silence(50), tone(988, 100)))

# 9) 网络异常：警笛上下
pkg("pkg_network_error.wav", concat(
    tone(400, 250), tone(800, 250), tone(400, 250)))