"""Composes the bot's chiptune music and zips it as mp3/NNNN_name.mp3 for the DFPlayer Mini's sd card.

Track numbers come from the order of TRACKS and must match tracks[]/gameTracks[] in src/face.cpp
and dances[] in src/main.cpp. Run: pip install lameenc, then python make_music.py
"""
import math
import random
import zipfile
from array import array
from pathlib import Path

import lameenc

RATE = 44100
NOTES = {'C': 0, 'D': 2, 'E': 4, 'F': 5, 'G': 7, 'A': 9, 'B': 11}


def midi(name):
    """'C4' -> 60, 'F#5' -> 78, 'Bb2' -> 46"""
    accidentals = name[1:-1]
    return NOTES[name[0]] + accidentals.count('#') - accidentals.count('b') + 12 * (int(name[-1]) + 1)


def parse(text, each):
    """'C4 E4:.5 -:2' -> [(midi or None, beats)], - is a rest, | is a bar line for reading"""
    notes = []
    for token in text.replace('|', ' ').split():
        name, _, beats = token.partition(':')
        notes.append((None if name == '-' else midi(name), float(beats) if beats else each))
    return notes


def square(duty):
    return lambda p: 1.0 if p % 1 < duty else -1.0


def triangle(p):
    return 4 * abs(p % 1 - 0.5) - 1


def musicbox(p):
    return math.sin(2 * math.pi * p) + 0.3 * math.sin(4 * math.pi * p)


LEAD = square(0.25)
BEEP = square(0.5)
THIN = square(0.125)


def tone(mix, start, length, freq, wave, vol, decay, vibrato):
    i0 = int(start * RATE)
    n = min(int(length * RATE), len(mix) - i0)
    attack, release = 0.005 * RATE, 0.01 * RATE
    step = freq / RATE
    phase = 0.0
    for i in range(n):
        env = min(1.0, i / attack, (n - i) / release) * math.exp(-decay * i / RATE)
        phase += step * 2 ** (vibrato / 12 * math.sin(2 * math.pi * 5 * i / RATE)) if vibrato else step
        mix[i0 + i] += vol * env * wave(phase)


def kick(mix, i0, vol):
    phase = 0.0
    for i in range(min(int(0.2 * RATE), len(mix) - i0)):
        t = i / RATE
        phase += (45 + 110 * math.exp(-t * 35)) / RATE  # pitch drops fast, that's the thump
        mix[i0 + i] += vol * math.sin(2 * math.pi * phase) * math.exp(-t * 18)


def snare(mix, i0, vol):
    for i in range(min(int(0.2 * RATE), len(mix) - i0)):
        t = i / RATE
        mix[i0 + i] += vol * (random.uniform(-0.7, 0.7) * math.exp(-t * 30) + 0.4 * math.sin(2 * math.pi * 190 * t) * math.exp(-t * 20))


def hat(mix, i0, vol):
    last = 0.0
    for i in range(min(int(0.05 * RATE), len(mix) - i0)):
        noise = random.uniform(-0.5, 0.5)
        mix[i0 + i] += vol * (noise - last) * math.exp(-i / RATE * 90)  # differencing keeps only the hiss
        last = noise


def pop(mix, i0, vol):
    phase = 0.0
    for i in range(min(int(0.15 * RATE), len(mix) - i0)):
        t = i / RATE
        phase += 300 * 5 ** min(t / 0.12, 1) / RATE  # slides up 300 -> 1500 Hz
        mix[i0 + i] += vol * math.sin(2 * math.pi * phase) * math.exp(-t * 15)


def voice(notes, wave, vol, each=1.0, decay=0.0, vibrato=0.0, gate=0.85):
    return parse(notes, each), wave, vol, decay, vibrato, gate


def drum(hit, hits, vol, steps=2):
    """hits: one char per 1/steps beat, x = hit"""
    return hit, hits.replace(' ', '').replace('|', ''), vol, steps


def song(bpm, voices, drums=(), seconds=None):
    """mixes the parts, each looping to fill seconds (None = the longest part once)"""
    beat = 60 / bpm
    lengths = [sum(b for _, b in v[0]) for v in voices] + [len(d[1]) / d[3] for d in drums]
    longest = max(lengths)
    assert all(longest % n == 0 for n in lengths), f'parts loop at mismatched lengths {lengths}, a bar is miscounted'
    mix = array('f', bytes(4 * int((seconds or longest * beat) * RATE)))
    total = len(mix) / RATE
    for notes, wave, vol, decay, vibrato, gate in voices:
        t = 0.0
        while t < total:
            for note, beats in notes:
                if note is not None:
                    tone(mix, t, beats * beat * gate, 440 * 2 ** ((note - 69) / 12), wave, vol, decay, vibrato)
                t += beats * beat
    for hit, hits, vol, steps in drums:
        for k in range(int(total / beat * steps + 0.5)):
            if hits[k % len(hits)] == 'x':
                hit(mix, int(k * beat / steps * RATE), vol)
    return mix


def dance(steps, step_ms, bpm, voices, drums):
    """a dance's music, as long as the dance itself in main.cpp"""
    mix = song(bpm, voices, drums)
    assert abs(len(mix) / RATE - steps * step_ms / 1000) < 0.01, 'dance music and dance moves differ in length'
    return mix


def forward():  # march
    return song(140, [
        voice('G4 C5 E5 G5 E5:1 C5:1 | F4 A4 C5 F5 A5:1 F5:1 | G4 B4 D5 G5 F5 D5 B4 G4 | C5:1 G4:1 C5:2', LEAD, .5, each=.5),
        voice('C3 G2 C3 G2 | F2 C3 F2 C3 | G2 D3 G2 D3 | C3 G2 C3 -', triangle, .6),
    ], [
        drum(kick, 'x...x...', .8), drum(snare, '..x...x.', .4), drum(hat, 'xxxxxxxx', .15),
    ], seconds=20)


def backward():  # reversing-truck beeps over a worried tiptoe
    return song(100, [
        voice('B5:1 -:1', BEEP, .2),
        voice('A2 - C3 - E3 - C3 - | D3 - F3 - E3 - G#2 -', triangle, .7, each=.5, decay=6),
        voice('-:3 E5:.5 C5:.5 | -:3 D5:.5 B4:.5', THIN, .3, decay=4),
    ], seconds=20)


# C G Am F, each bar tumbling down
CASCADE = [
    'C6 G5 E5 C5 G5 E5 C5 G4 E5 C5 G4 E4 C5 G4 E4 C4',
    'B5 G5 D5 B4 G5 D5 B4 G4 D5 B4 G4 D4 B4 G4 D4 B3',
    'A5 E5 C5 A4 E5 C5 A4 E4 C5 A4 E4 C4 A4 E4 C4 A3',
    'A5 F5 C5 A4 F5 C5 A4 F4 C5 A4 F4 C4 A4 F4 C4 A3',
]


def turn(bars):
    return song(150, [
        voice(' '.join(bars), LEAD, .45, each=.25, decay=3),
        voice('C3 - C3 C3 | G2 - G2 G2 | A2 - A2 A2 | F2 - F2 F2', triangle, .6),
    ], [
        drum(kick, 'x..xx...', .7), drum(snare, '..x...x.', .35), drum(hat, 'xxxxxxxx', .12),
    ], seconds=20)


def left():  # notes tumble down
    return turn(CASCADE)


def right():  # same notes climbing up
    return turn(' '.join(bar.split()[::-1]) for bar in CASCADE)


def happy():  # ta-da, short enough to finish before the face goes back to idle (HAPPY_MS)
    return song(200, [
        voice('C5:.25 E5:.25 G5:.25 C6:1.75', BEEP, .4, decay=2),
        voice('-:.75 E5:1.75', LEAD, .25, decay=2),
        voice('C4:.75 C3:1.75', triangle, .6),
    ])


def sleep():  # music box lullaby that fades as the bot dozes off, PLAY_MS long
    mix = song(72, [
        voice('C5 C5 G5 G5 | A5 A5 G5:2 | F5 F5 E5 E5 | D5 D5 C5:2 | G5 G5 F5 F5 | E5 E5 D5:2 | '
              'G5 G5 F5 F5 | E5 E5 D5:2 | C5 C5 G5 G5 | A5 A5 G5:2 | F5 F5 E5 E5 | D5 D5 C5:2', musicbox, .5, decay=3, gate=1),
        voice('C4:2 G3:2 | F3:2 C4:2 | F3:2 C4:2 | G3:2 C4:2 | C4:2 F3:2 | C4:2 G3:2 | '
              'C4:2 F3:2 | C4:2 G3:2 | C4:2 G3:2 | F3:2 C4:2 | F3:2 C4:2 | G3:2 C4:2', triangle, .25, decay=1),
    ], seconds=120)
    for i in range(len(mix)):
        mix[i] *= 1 - 0.65 * i / len(mix)
    return mix


def game_ball():  # bouncy
    return song(160, [
        voice('G4 - B4 D5 G5 - D5 B4 | C5 - E5 G5 C6 - G5 E5 | D5 - F#5 A5 D6 - A5 F#5 | G5 D5 B4 D5 G5:2', LEAD, .45, each=.5),
        voice('G2 G3 ' * 4 + 'C3 C4 ' * 4 + 'D3 D4 ' * 4 + 'G2 G3 ' * 4, triangle, .6, each=.5, decay=5),
    ], [
        drum(kick, 'x...x...', .7), drum(snare, '..x...x.', .35), drum(hat, 'x.x.x.x.', .15),
    ], seconds=22)


def game_pong():  # arpeggios with a blip each time the ball hits a paddle
    return song(120, [
        voice('A3 C4 E4 A4 ' * 4 + 'F3 A3 C4 F4 ' * 4 + 'C4 E4 G4 C5 ' * 4 + 'G3 B3 D4 G4 ' * 4, LEAD, .3, each=.25, decay=6),
        voice('A2:4 | F2:4 | C3:4 | G2:4', triangle, .6),
        voice('A5:.125 -:1.875 A4:.125 -:1.875', BEEP, .35, gate=1),
    ], [
        drum(kick, 'x...x...', .7), drum(hat, '.x.x.x.x', .15),
    ], seconds=22)


def game_dizzy():  # woozy waltz, the lead wobbles in pitch
    return song(150, [
        voice('G4 F#4 G4 | Eb5:3 | D5 C#5 D5 | C5:3 | Bb4 A4 Bb4 | G5:3 | F5 Eb5 D5 | C5:3', LEAD, .45, vibrato=.4),
        voice('C3 G3 G3 | C3 G3 G3 | G2 D3 D3 | C3 G3 G3 | Bb2 F3 F3 | Eb3 Bb3 Bb3 | G2 D3 D3 | C3 G3 G3', triangle, .6, decay=4),
    ], [
        drum(kick, 'x..', .6, steps=1), drum(hat, '.xx', .12, steps=1),
    ], seconds=22)


def game_dance():  # disco at 120 BPM, the same beat the face hops to
    return song(120, [
        voice('A4:.5 C5:.5 E5:.5 A5:1 G5:.5 E5:1 | D5:.5 E5:.5 C5:.5 A4:1.5 -:1 | '
              'F#5:.5 A5:.5 F#5:.5 E5:1 D5:.5 E5:1 | F#5:.5 E5:.5 D5:.5 B4:1.5 -:1', LEAD, .45),
        voice('A2 A3 ' * 8 + 'D3 D4 ' * 8, triangle, .6, each=.5, decay=4),
    ], [
        drum(kick, 'x.x.x.x.', .8), drum(snare, '..x...x.', .35), drum(hat, '.x.x.x.x', .2),
    ], seconds=22)


def game_peekaboo():  # one bar per 2.5 s peekaboo, quiet while hidden, pop when the eyes come back
    return song(96, [
        voice('-:2 E5:.25 G5:.25 C6:.5 -:1 | -:2 F5:.25 A5:.25 C6:.5 -:1 | -:2 G5:.25 B5:.25 D6:.5 -:1 | -:2 E5:.25 G5:.25 C6:.5 -:1', BEEP, .35),
        voice('C3:.5 -:.5 G3:.5 -:.5 C3:2 | F3:.5 -:.5 C4:.5 -:.5 F3:2 | G3:.5 -:.5 D4:.5 -:.5 G3:2 | C3:.5 -:.5 G3:.5 -:.5 C3:2', triangle, .4, decay=6),
    ], [
        drum(pop, '..x.', .5, steps=1),
    ], seconds=22)


def dance_wiggle():  # wiggle, shuffle, wiggle, spin: one note per move
    return dance(16, 300, 100, [
        voice('F5 C5 F5 C5 | A5 F4 A5 F4 | F5 C5 F5 C5 | C5 F5 A5 C6', LEAD, .45, each=.5),
        voice('F2 C3 F2 C3 F2 C3 F2:.5 A2:.5 C3:.5 E3:.5', triangle, .6),
    ], [
        drum(kick, 'x...', .8), drum(snare, '..x.', .35), drum(hat, 'xxxx', .15),
    ])


def dance_waddle():  # penguin: four waddles, pose, four back, pose
    return dance(12, 250, 120, [
        voice('C4:.5 E4:.5 C4:.5 E4:.5 G4:1 | E4:.5 G4:.5 E4:.5 G4:.5 C5:1', BEEP, .35),
        voice('C3:.5 G2:.5 C3:.5 G2:.5 G2:1 | C3:.5 G2:.5 C3:.5 G2:.5 C3:1', triangle, .7, decay=5),
    ], [
        drum(kick, 'x.x.x.x.x.x.', .7), drum(snare, '....x.....x.', .45),
    ])


def dance_chacha():  # shimmy, cha-cha-cha forward, cha-cha-cha back, spin left
    return dance(30, 150, 100, [
        voice('E5 D#5 E5 D#5 E5 D#5 E5 D#5 | -:.5 | A4:.5 C5:.5 E5 E5 | E5:.5 D5:.5 C5 C5 | A5 G5 E5 D5 C5 B4 A4 E4', LEAD, .45, each=.25),
        voice('A2:2 | -:.5 | A2:.5 A2:.5 A2 A2 | E2:.5 E2:.5 E2 E2 | A2:2', triangle, .6, each=.25),
    ], [
        drum(hat, 'xxxxxxxx .. ...... ...... x.x.x.x.', .15, steps=4),
        drum(kick, 'x....... .. x.x.xx ...... x.......', .8, steps=4),
        drum(snare, '........ .. ...... x.x.xx ........', .4, steps=4),
    ])


# (name, music, loudness 0..1), numbered from 1 in this order
TRACKS = [
    ('forward', forward, .85),
    ('backward', backward, .85),
    ('left', left, .85),
    ('right', right, .85),
    ('happy', happy, .85),
    ('sleep', sleep, .55),
    ('game_ball', game_ball, .85),
    ('game_pong', game_pong, .85),
    ('game_dizzy', game_dizzy, .85),
    ('game_dance', game_dance, .85),
    ('game_peekaboo', game_peekaboo, .85),
    ('dance_wiggle', dance_wiggle, .85),
    ('dance_waddle', dance_waddle, .85),
    ('dance_chacha', dance_chacha, .85),
]


def to_mp3(mix, loudness):
    fade = int(0.01 * RATE)  # no click at the end
    for i in range(fade):
        mix[-1 - i] *= i / fade
    scale = loudness * 32767 / (max(map(abs, mix)) or 1)
    pcm = array('h', (int(s * scale) for s in mix))
    encoder = lameenc.Encoder()
    encoder.set_bit_rate(128)
    encoder.set_in_sample_rate(RATE)
    encoder.set_channels(1)
    encoder.set_quality(2)
    return encoder.encode(pcm.tobytes()) + encoder.flush()


def main():
    assert midi('A4') == 69 and midi('Bb2') == 46 and midi('F#5') == 78
    random.seed(1)  # same drums every run
    out = Path(__file__).with_name('sd_card.zip')
    with zipfile.ZipFile(out, 'w') as z:
        for number, (name, music, loudness) in enumerate(TRACKS, 1):
            mix = music()
            path = f'mp3/{number:04d}_{name}.mp3'
            z.writestr(path, to_mp3(mix, loudness))
            print(f'{path}  {len(mix) / RATE:.1f}s')
    print(f'wrote {out}')


if __name__ == '__main__':
    main()
