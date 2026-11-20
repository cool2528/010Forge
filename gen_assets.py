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

# ---------- 原创暗色赛博电子循环：Grid After Midnight ----------
def make_music():
    """Original synth/drums, no samples or third-party soundtrack. Pure stdlib."""
    import random
    import array
    sr, bpm, bars = 22050, 100, 8
    beat = 60.0 / bpm
    seconds = bars * 4 * beat
    size = round(sr * seconds)
    mix = array.array('d', [0.0]) * size
    rng = random.Random(10402026)
    tau = math.tau

    def hz(midi):
        return 440.0 * 2 ** ((midi - 69) / 12)

    def add(start, duration, synth, gain):
        offset = round(start * sr)
        count = round(duration * sr)
        for j in range(count):
            t = j / sr
            # Wrap delay tails so the phrase is periodic, not cut off at end.
            mix[(offset + j) % size] += gain * synth(t, j, count)

    # E-minor bass roots with ominous b6/b2 tension; eight original bars.
    roots = [28, 28, 24, 26, 28, 31, 29, 27]
    for bar, root in enumerate(roots):
        start = bar * 4 * beat
        for step in (0, 3, 6, 8, 11, 14):
            f = hz(root if step < 11 else root + 12)
            def bass(t, j, count, f=f):
                attack = min(1.0, t / .006)
                env = attack * math.exp(-8 * t)
                tone = math.sin(tau*f*t) + .24*math.sin(tau*2*f*t) + .09*math.sin(tau*3*f*t)
                return math.tanh(1.4*tone) * env
            add(start + step*beat/4, beat*.38, bass, .17)
        # Detuned low pad, slow gated pump. No copied melody.
        for interval in (12, 19, 22):
            f = hz(root + interval)
            def pad(t, j, count, f=f):
                env = math.sin(math.pi * j / count)**2
                pulse = .35 + .65*(.5-.5*math.cos(tau*t/beat))
                return env*pulse*(math.sin(tau*f*t) + .4*math.sin(tau*f*1.003*t))
            add(start, 4*beat, pad, .018)
        pattern = [0, 7, 12, 15, 19, 12, 7, 22, 0, 12, 19, 15, 7, 22, 12, 7]
        for step, interval in enumerate(pattern):
            f = hz(root + 36 + interval)
            def arp(t, j, count, f=f):
                env = min(1., t/.002)*math.exp(-24*t)
                return env*(math.sin(tau*f*t + 1.8*math.exp(-28*t)*math.sin(tau*2*f*t)))
            when = start + step*beat/4
            add(when, beat*.38, arp, .035 if step % 4 else .055)
            add(when + beat*.75, beat*.38, arp, .011)
        for pulse in range(4):
            def kick(t, j, count):
                phase = tau*(43*t + 2.8*(1-math.exp(-35*t)))
                return math.sin(phase)*math.exp(-17*t) + .04*rng.uniform(-1,1)*math.exp(-140*t)
            add(start+pulse*beat, .32, kick, .23)
            if pulse in (1, 3):
                def snare(t, j, count):
                    noise = rng.uniform(-1.,1.)
                    return (noise*.65+math.sin(tau*165*t)*.35)*math.exp(-26*t)*min(1.,t/.001)
                add(start+pulse*beat, .18, snare, .09)
        for tick in range(8):
            def hat(t, j, count):
                metallic = math.sin(tau*6700*t)*math.sin(tau*4310*t)
                return (metallic*.55+rng.uniform(-1,1)*.45)*math.exp(-95*t)
            add(start+tick*beat/2, .065, hat, .025 if tick % 2 else .017)
        if bar in (3, 7):
            for tick in range(6):
                f = hz(96 - tick*3)
                def glitch(t, j, count, f=f):
                    return math.sin(tau*f*t)*math.exp(-95*t)*(1 if j % 22 < 14 else 0)
                add(start+3*beat+tick*beat/8, .055, glitch, .016)

    mean = sum(mix)/size
    pcm = array.array('h')
    peak, energy = 0.0, 0.0
    for i, value in enumerate(mix):
        fade = min(1.0, i/(sr*.004), (size-1-i)/(sr*.004))
        sample = .82*math.tanh((value-mean)*1.25)*max(0., fade)
        peak = max(peak, abs(sample))
        energy += sample*sample
        pcm.append(round(sample*32767))
    import sys
    if sys.byteorder != 'little':
        pcm.byteswap()
    path = os.path.join(OUT, 'bgm.wav')
    with wave.open(path, 'wb') as wav:
        wav.setparams((1, 2, sr, size, 'NONE', 'not compressed'))
        wav.writeframes(pcm.tobytes())
    rms = math.sqrt(energy/size)
    assert peak < .83 and 0.025 < rms < .25
    assert pcm[0] == 0 and pcm[-1] == 0
    print('Grid After Midnight | %.1f s | %d Hz | peak %.3f | RMS %.3f | %d bytes'
          % (seconds, sr, peak, rms, os.path.getsize(path)))
    return True

if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('--music-only', action='store_true')
    args = parser.parse_args()
    if not args.music_only:
        make_icon()
    make_music()
