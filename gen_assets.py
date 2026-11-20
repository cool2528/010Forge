# -*- coding: utf-8 -*-
"""生成图标 (.ico) 和背景音乐 (.wav) 资源。

音乐重新生成需要可选的 NumPy；正常 CMake 构建直接嵌入现有资源，无需运行此脚本。
只生成选定的 B168 音乐：python -B gen_assets.py --music-only
"""
import os
from pathlib import Path

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

# ---------- 原B编曲：Terminal Pulse，168 BPM / 8 bars ----------
def make_music():
    """Keep B's score, synth voices and seed; change only its tempo to 168 BPM."""
    from tools.generate_music_previews import keygen
    keygen(Path(OUT) / 'bgm.wav', bpm=168)
    return True

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--music-only', action='store_true')
    args = parser.parse_args()
    if not args.music_only:
        make_icon()
    make_music()
