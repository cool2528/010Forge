# -*- coding: utf-8 -*-
"""生成图标 (.ico) 和背景音乐 (.wav) 资源"""
import os, math, struct, wave

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'resources')
os.makedirs(OUT, exist_ok=True)

# ---------- 图标 ----------
def make_icon():
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print("PIL not available, skip icon")
        return False
    sz = 64
    img = Image.new('RGBA', (sz, sz), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    # 圆角背景
    d.rounded_rectangle([2, 2, sz-3, sz-3], radius=12, fill=(28, 30, 36, 255), outline=(255, 170, 40, 255), width=3)
    # 书本/补丁意象：一个 "010" 样式
    d.text((8, 20), "010", fill=(255, 185, 60, 255))
    # 底部小扳手色块
    d.rectangle([10, 46, 54, 52], fill=(255, 170, 40, 255))
    ico = os.path.join(OUT, 'app.ico')
    img.save(ico, format='ICO', sizes=[(16,16),(32,32),(48,48),(64,64)])
    print("wrote", ico, os.path.getsize(ico))
    return True

# ---------- 背景音乐（循环 chiptune 旋律）----------
def make_music():
    """生成一段 8 秒左右可循环的芯片音乐 WAV（16-bit mono 22050Hz）"""
    sr = 11025
    bpm = 120
    beat = 60.0 / bpm
    # 音符频率 (A4=440)
    def note(n):
        # n: MIDI note number
        return 440.0 * (2.0 ** ((n - 69) / 12.0))
    # 一段循环的旋律（每拍一个音符，MIDI 音高），8 小节 x 4 拍
    melody = [
        69, 72, 76, 72, 74, 77, 81, 77,
        76, 72, 69, 72, 74, 76, 74, 72,
        69, 72, 76, 79, 77, 76, 74, 72,
        71, 74, 77, 74, 72, 69, 71, 72,
    ]
    dur = beat * 0.9
    samples = []
    for i, m in enumerate(melody):
        f = note(m)
        n = int(sr * dur)
        for j in range(n):
            t = j / sr
            # 方波 + 轻微衰减，芯片音色
            phase = (f * t) % 1.0
            sq = 1.0 if phase < 0.5 else -1.0
            env = 1.0 - (j / n) * 0.6
            # 加一个低八度正弦做 bass
            bass = 0.25 * math.sin(2 * math.pi * f * 0.5 * t)
            v = (sq * 0.18 + bass) * env
            samples.append(max(-1.0, min(1.0, v)))
    # 转成 16-bit PCM
    data = bytearray()
    for s in samples:
        val = int(s * 22000)
        data += struct.pack('<h', val)
    wav = os.path.join(OUT, 'bgm.wav')
    with wave.open(wav, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(bytes(data))
    print("wrote", wav, len(data)+44, "bytes,", round(len(samples)/sr, 1), "sec")
    return True

if __name__ == '__main__':
    make_icon()
    make_music()
