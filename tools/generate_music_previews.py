"""Original offline BGM auditions, not installed or played automatically.

NumPy is optional for regeneration; normal builds embed the checked-in WAV.
Reproduce the selected original B arrangement at 168 BPM without touching A/B:
    python -B tools/generate_music_previews.py --track keygen --bpm 168
Regenerate resources/bgm.wav from that same arrangement:
    python -B gen_assets.py --music-only
The default ab audition mode retains the original B at 148 BPM.
"""
from __future__ import annotations

import argparse
import json
import math
import wave
from pathlib import Path

import numpy as np

SR = 22050
TAU = 2 * math.pi


def hz(note):
    return 440 * 2 ** ((note - 69) / 12)


def timebase(seconds):
    return np.arange(max(1, round(seconds * SR))) / SR


def edge(x, attack=.003, release=.008):
    n = len(x)
    a, r = min(n, round(attack * SR)), min(n, round(release * SR))
    env = np.ones(n)
    if a:
        env[:a] *= np.sin(np.linspace(0, math.pi / 2, a)) ** 2
    if r:
        env[-r:] *= np.sin(np.linspace(math.pi / 2, 0, r)) ** 2
    return x * env


def lp(noise, cutoff=2200):
    # FFT filtering is synthesis only; no audio samples or downloads.
    bins = np.fft.rfftfreq(len(noise), 1 / SR)
    gain = 1 / np.sqrt(1 + (bins / cutoff) ** 6)
    return np.fft.irfft(np.fft.rfft(noise) * gain, n=len(noise))


def saw(t, frequency, cutoff=2200):
    value = np.zeros_like(t)
    for h in range(1, min(28, int(cutoff / frequency)) + 1):
        value += np.sin(TAU * frequency * h * t) / h
    return value * (2 / math.pi)


class Session:
    def __init__(self, bpm, bars, seed):
        self.bpm = bpm
        self.beat = 60 / bpm
        self.seconds = bars * 4 * self.beat
        self.n = round(self.seconds * SR)
        self.audio = np.zeros((self.n, 2))
        self.rng = np.random.default_rng(seed)
        self.beat_t = np.arange(self.n) / SR / self.beat

    def put(self, seconds, sound, gain=1, pan=0, echo=False):
        index = (round(seconds * SR) + np.arange(len(sound))) % self.n
        left = math.sqrt((1 - pan) / 2)
        right = math.sqrt((1 + pan) / 2)
        np.add.at(self.audio[:, 0], index, gain * left * sound)
        np.add.at(self.audio[:, 1], index, gain * right * sound)
        if echo:
            self.put(seconds + self.beat * .75, sound, gain * .24, -pan * .7)
            self.put(seconds + self.beat * 1.5, sound, gain * .08, pan * .8)

    def noise(self, duration):
        return self.rng.uniform(-1, 1, len(timebase(duration)))

    def kick(self):
        t = timebase(.29)
        phase = TAU * (45 * t + 2.65 * (1 - np.exp(-42 * t)))
        body = np.sin(phase) * np.exp(-17 * t)
        attack = self.noise(.29) * np.exp(-210 * t) * .12
        return edge(np.tanh((body + attack) * 1.8) * .72, .0005, .012)

    def snare(self, metallic=False):
        t = timebase(.2)
        noise = self.noise(.2)
        crack = noise - lp(noise, 1200)
        ring = np.sin(TAU * 178 * t) + .25 * np.sin(TAU * 331 * t)
        if metallic:
            ring += .22 * np.sin(TAU * 1690 * t) * np.sin(TAU * 2203 * t)
        return edge(crack * np.exp(-25 * t) * .95 + ring * np.exp(-35 * t) * .35, .0004, .009)

    def hat(self, opened=False):
        dur = .12 if opened else .047
        t = timebase(dur)
        noise = self.noise(dur)
        high = noise - lp(noise, 3200)
        metal = np.sin(TAU * 7013 * t) * np.sin(TAU * 3929 * t)
        return edge((high * .6 + metal * .2) * np.exp(-t * (34 if opened else 95)), .0003, .006)

    def finish(self, path):
        # Broad harmonic saturation and gentle dynamic compression, not a quiet
        # sine-wave bed. Keep low-frequency centre coherent in stereo.
        x = self.audio - self.audio.mean(axis=0)
        x = np.tanh(x * 1.65)
        # Slow compression on 10 ms blocks approximates a limiter without clips.
        block = 220
        level = np.array([np.max(np.abs(x[i:i+block])) for i in range(0, self.n, block)])
        gain = 1 / np.maximum(1, level / .64)
        smooth = np.interp(np.arange(self.n), np.arange(len(gain)) * block, gain)
        x *= smooth[:, None]
        x *= .71 / max(.001, np.max(np.abs(x)))
        nfade = round(.004 * SR)
        x[:nfade] *= np.linspace(0, 1, nfade)[:, None]
        x[-nfade:] *= np.linspace(1, 0, nfade)[:, None]
        pcm = np.round(x * 32767).astype('<i2')
        path.parent.mkdir(parents=True, exist_ok=True)
        with wave.open(str(path), 'wb') as w:
            w.setparams((2, 2, SR, self.n, 'NONE', 'not compressed'))
            w.writeframes(pcm.tobytes())
        peak = float(np.max(np.abs(x)))
        rms = float(np.sqrt(np.mean(x*x)))
        result = dict(file=path.name, bpm=self.bpm, seconds=round(self.n/SR, 3),
                      channels=2, sample_rate=SR, peak_dbfs=round(20*math.log10(peak), 2),
                      rms_dbfs=round(20*math.log10(rms), 2), bytes=path.stat().st_size,
                      clipped_samples=int(np.count_nonzero(np.abs(pcm.astype(np.int32)) >= 32767)),
                      endpoint_silent=bool(np.all(pcm[0] == 0) and np.all(pcm[-1] == 0)))
        assert result['clipped_samples'] == 0 and result['endpoint_silent']
        assert -24 < result['rms_dbfs'] < -9
        print(json.dumps(result, ensure_ascii=False))
        return result


def industrial(path):
    """136 BPM, near-monotone syncopated industrial/breakbeat, dark pressure."""
    s = Session(136, 8, 400136)
    kick, snare = s.kick(), s.snare(True)
    for bar in range(8):
        b = bar * 4 * s.beat
        for step in ([0, 3, 6, 8, 11, 14] if bar % 2 == 0 else [0, 4, 7, 10, 12, 15]):
            s.put(b + step*s.beat/4, kick, .83 if step in (0, 8) else .64)
        for step in (4, 12):
            s.put(b + step*s.beat/4, snare, .78)
        if bar % 2:
            s.put(b + 10.5*s.beat/4, snare, .13, -.1)
            s.put(b + 15*s.beat/4, snare[:1800], .22, .12)
        for step in range(16):
            s.put(b + step*s.beat/4, s.hat(step % 8 == 6), .085 if step % 2 else .13,
                  -.27 if step % 2 else .27)
        # Dissonance is rhythmic / timbral, not a bright game melody.
        root = [28, 28, 28, 27, 28, 28, 31, 29][bar]
        for step in (0, 2, 3, 6, 7, 8, 10, 11, 14):
            t = timebase(s.beat * (.38 if step % 3 else .65))
            f = hz(root + (12 if step in (3, 11) else 0))
            ramp = np.minimum(1, t/.004)*np.exp(-5*t)
            mod = 2.4*np.sin(TAU*f*.5*t)*np.exp(-6*t)
            growl = .5*saw(t, f, 1450) + .33*saw(t, f*1.007, 1450) + .35*np.sin(TAU*f*t+mod)
            bass = edge(np.tanh(growl*3)*ramp, .003, .014)
            s.put(b + step*s.beat/4, bass, .39)
        # Stab clusters plus filtered feedback. Pulse width and FM provide the
        # 'terminal alarm' edge without copying an identifiable theme.
        for step in (1, 6, 9, 14):
            t = timebase(.13)
            f = hz(64 + (1 if step in (6, 14) else 0))
            stab = np.sin(TAU*f*t + 3*np.sin(TAU*f*2.01*t))
            stab += .4*np.sin(TAU*f*1.5*t)
            s.put(b+step*s.beat/4, edge(stab*np.exp(-27*t)), .07,
                  -.42 if step % 2 else .42, True)
        # Mechanical noise bursts/sweeps at phrase changes.
        if bar in (0, 3, 7):
            t = timebase(s.beat*.7)
            noise = lp(s.noise(s.beat*.7), 4800)
            ring = np.sin(TAU*2411*t)*np.sin(TAU*1733*t)
            s.put(b+3.25*s.beat, edge((noise*.5+ring*.25)*np.exp(-10*t)), .085, -.3, True)
        if bar in (3, 7):
            for step in range(8):
                t = timebase(.035)
                f = 430 + step*311
                pulse = np.sin(TAU*f*t + 3*np.sin(TAU*47*t)) * np.exp(-110*t)
                s.put(b+3.5*s.beat+step*s.beat/16, edge(pulse, .0005, .003), .095, (-1)**step*.7)
    return s.finish(path)


def keygen(path, bpm=148):
    """Original B tracker/keygen; only tempo varies, not instruments or score."""
    if not math.isfinite(bpm) or bpm <= 0:
        raise ValueError('bpm must be a positive finite number')
    # Keep the original seed even at other tempos to preserve B's noise voices.
    s = Session(bpm, 8, 400148)
    kick, snare = s.kick(), s.snare()
    riff = [0, 0, 12, 7, 0, 15, 12, 7, 0, 0, 12, 19, 15, 12, 7, 1]
    for bar in range(8):
        b = bar * 4 * s.beat
        root = [28, 28, 24, 27, 28, 28, 29, 27][bar]
        for step in (0, 4, 8, 12):
            s.put(b+step*s.beat/4, kick, .79)
        for step in (4, 12):
            s.put(b+step*s.beat/4, snare, .7)
        for step in range(16):
            s.put(b+step*s.beat/4, s.hat(step % 4 == 2), .12 if step % 2 else .075, (-1)**step*.3)
            t = timebase(s.beat*.4)
            f = hz(root+12)
            bass = edge(np.tanh(3*saw(t, f, 2000)) * np.exp(-12*t))
            s.put(b+step*s.beat/4, bass, .28 if step % 4 else .2)
            f = hz(root+36+riff[(step+2*(bar%2)) % 16])
            t = timebase(s.beat*.32)
            # Band-limited pulse oscillator, 8-bit timbre with overdrive.
            phase = TAU*f*t
            pulse = np.zeros_like(t)
            for harmonic in (1, 3, 5, 7):
                if harmonic*f < SR*.42:
                    pulse += np.sin(phase*harmonic)/harmonic
            lead = edge(np.tanh(pulse*1.5)*np.exp(-20*t), .001, .01)
            s.put(b+step*s.beat/4, lead, .10, (-1)**step*.38, True)
        if bar in (3, 7):
            for step in (13, 14, 15):
                t = timebase(.065)
                z = s.noise(.065)
                z = np.round(z*8)/8
                s.put(b+step*s.beat/4, edge(z*np.exp(-60*t)), .11, .5)
    return s.finish(path)


def keygen_plus(path):
    """Original 152 BPM tracker / patcher loop, emphasizing MOD-style effects.

    Based on B's pulse-lead + offbeat-bass foundation, with a composed 16-bar
    melody, tick-rate 0xy chord arpeggios, portamento, delayed vibrato and
    a narrow stereo echo. No film music, vocals or sampled melodies.
    """
    s = Session(152, 16, 401152)

    def pulse_phase(phase, frequency, duty=.25):
        sound = np.zeros_like(phase)
        limit = max(1, min(18, int(SR * .42 / frequency)))
        for harmonic in range(1, limit + 1):
            coefficient = 2 * math.sin(math.pi * harmonic * duty) / (math.pi * harmonic)
            sound += coefficient * np.cos(harmonic * phase - math.pi * harmonic * duty)
        return sound

    def lead(midi, duration, previous=None, echo_voice=False):
        t = timebase(duration)
        frequency = hz(midi)
        pitch = np.full_like(t, frequency)
        if previous is not None:
            # Tracker-style tone portamento: a short glide, not a long siren.
            pitch = frequency + (hz(previous) - frequency) * np.exp(-t / .014)
        vibrato_depth = np.clip((t - .045) / .08, 0, 1) * .0045
        pitch *= 1 + vibrato_depth * np.sin(TAU * 6.2 * t)
        phase = TAU * np.cumsum(pitch) / SR
        sound = pulse_phase(phase, frequency, .25 if not echo_voice else .375)
        sound += .14 * np.sin(phase * 2 + .025 * np.sin(TAU * 5 * t))
        # Gentle sample quantization gives an old tracker instrument texture.
        sound = np.round(np.tanh(sound * 1.9) * 96) / 96
        envelope = .85 * np.exp(-t * 3) + .15
        return edge(sound * envelope, .0018, .014)

    # Sixteen 1/16 rows per bar. Tuples are (row, length in rows, MIDI note).
    # This is a newly composed hook, not the previous stream of random beeps.
    phrases = [
        [(0,2,76),(2,2,79),(4,3,83),(7,1,81),(8,2,79),(10,2,76),(12,2,74),(14,2,76)],
        [(0,3,79),(3,1,81),(4,2,83),(6,2,86),(8,3,83),(11,1,81),(12,2,79),(14,2,78)],
        [(0,2,76),(2,2,79),(4,3,84),(7,1,83),(8,2,79),(10,2,76),(12,2,74),(14,2,72)],
        [(0,3,71),(3,1,74),(4,2,78),(6,2,81),(8,2,78),(10,2,74),(12,2,75),(14,2,78)],
        [(0,2,76),(2,2,79),(4,4,83),(8,2,88),(10,2,86),(12,2,83),(14,2,79)],
        [(0,2,81),(2,2,84),(4,3,88),(7,1,86),(8,2,84),(10,2,81),(12,2,79),(14,2,78)],
        [(0,2,79),(2,2,83),(4,2,86),(6,2,83),(8,3,79),(11,1,78),(12,2,76),(14,2,74)],
        [(0,2,75),(2,2,78),(4,3,83),(7,1,81),(8,2,78),(10,2,75),(12,2,78),(14,2,75)],
    ]
    roots = [28,28,24,23,28,21,24,23] * 2
    chords = [(0,3,7),(0,3,7),(0,4,7),(0,4,7),
              (0,3,7),(0,3,7),(0,4,7),(0,4,7)] * 2
    previous_note = 75
    kick = s.kick()
    snare = s.snare(False)
    for bar in range(16):
        at = bar * 4 * s.beat
        root = roots[bar]
        # B's four-on-the-floor rhythm stays, but percussion is pulled back
        # so it feels like a melodic patch-window module rather than a club mix.
        for row in (0, 4, 8, 12):
            s.put(at + row*s.beat/4, kick, .60)
        for row in (4, 12):
            s.put(at + row*s.beat/4, snare, .43)
            # Small double-hit approximates a tracker retrigger effect.
            s.put(at + row*s.beat/4 + .016, snare[:1500], .075, .1)
        for row in range(16):
            s.put(at + row*s.beat/4, s.hat(row % 4 == 2),
                  .065 if row % 2 else .04, (-1)**row*.35)

        # Octave-bouncing saw/pulse bass replaces the industrial FM growl.
        for row in (0, 2, 3, 6, 8, 10, 11, 14):
            t = timebase(s.beat*.43)
            frequency = hz(root + (12 if row in (3, 11) else 0))
            bass = .65*saw(t, frequency, 1550) + .30*np.sin(TAU*frequency*t)
            bass = edge(np.tanh(bass*1.9)*np.exp(-t*6), .002, .012)
            s.put(at + row*s.beat/4, bass, .24)

        # Authentic tracker 0xy effect: three chord notes cycle at ~50 Hz
        # on one oscillator, rather than playing a smooth modern pad.
        for row in (0, 2, 4, 6, 8, 10, 12, 14):
            t = timebase(s.beat*.43)
            offsets = np.array(chords[bar])[(np.floor(t * 50).astype(int) % 3)]
            frequencies = hz(root + 36) * 2**(offsets/12)
            phase = TAU*np.cumsum(frequencies)/SR
            arpeggio = np.sin(phase) + .22*np.sin(phase*2) + .10*np.sin(phase*3)
            arpeggio = edge(arpeggio*np.exp(-t*8), .001, .01)
            s.put(at + row*s.beat/4, arpeggio, .072,
                  -.50 if row % 4 else .50)

        for row, length, midi in phrases[bar % 8]:
            # Second 8 bars add a few octave lifts, a classic keygen 'chorus'.
            lift = 12 if bar >= 8 and row in (4, 8) and bar % 8 not in (3, 7) else 0
            note = midi + lift
            duration = length*s.beat/4*.91
            sound = lead(note, duration, previous_note if row in (4, 8, 14) else None)
            s.put(at + row*s.beat/4, sound, .20 if row % 4 == 0 else .17,
                  -.10 if row < 8 else .10, True)
            # Quiet octave harmony, not a second competing melody.
            if bar >= 8 and row in (0, 8):
                s.put(at + row*s.beat/4, lead(note-12, duration, echo_voice=True), .025, -.32)
            previous_note = note

        if bar % 4 == 3:
            # Snare roll + downward pitch run signals the loop/phrase change.
            for index, row in enumerate((14, 14.5, 15, 15.5)):
                s.put(at + row*s.beat/4, snare[:1700], .09+index*.035, (-1)**index*.2)
            t = timebase(s.beat*.22)
            frequency = hz(91)*np.exp(-t*9)
            phase = TAU*np.cumsum(frequency)/SR
            blip = edge(np.sin(phase)*np.exp(-t*25), .001, .008)
            s.put(at + 15*s.beat/4, blip, .035, .5)
    return s.finish(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[1]/'dist'/'music-previews')
    ap.add_argument('--track', choices=['ab', 'keygen', 'keygen-plus'], default='ab')
    ap.add_argument('--bpm', type=float, default=148,
                    help='Tempo for --track keygen (default: original B at 148 BPM)')
    args = ap.parse_args()
    if not math.isfinite(args.bpm) or args.bpm <= 0:
        ap.error('--bpm must be a positive finite number')
    if args.track != 'keygen' and args.bpm != 148:
        ap.error('--bpm is used only with --track keygen')
    if args.track == 'keygen':
        # A tempo-qualified output leaves both original A/B auditions intact.
        keygen(args.output/f'B-Terminal-Pulse-keygen-{args.bpm:g}bpm.wav', bpm=args.bpm)
        print('Audition WAV only; resources/bgm.wav and releases are unchanged.')
        return
    if args.track == 'keygen-plus':
        tracks = [keygen_plus(args.output/'B-plus-Patchloader-2001-tracker.wav')]
        report = 'audio-checks-keygen-plus.json'
    else:
        tracks = [industrial(args.output/'A-Cipher-Breach-industrial.wav'),
                  keygen(args.output/'B-Terminal-Pulse-keygen.wav')]
        report = 'audio-checks.json'
    (args.output/report).write_text(json.dumps(tracks, indent=2), encoding='utf-8')
    print('Audition files only; resources/bgm.wav and releases are unchanged.')


if __name__ == '__main__':
    main()
