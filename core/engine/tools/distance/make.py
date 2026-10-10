# Distance test recordings: speech with known transcripts, placed in a simulated
# meeting room at a known distance from the phone (pyroomacoustics).
#   alone-<d>m.wav  : one talker (voice B) at d metres
#   mixed-<d>m.wav  : voice A at 1 m and voice B at d metres, taking turns
#   quiet-<x>db.wav : voice A at 1 m, voice B at 3 m and a further x dB quieter
#                     (a soft talker, or a room that absorbs more)
# Writes <work>/recordings/*.wav and <work>/refs.json, the words each voice said.
import json, os, sys
import numpy as np, soundfile as sf, pyroomacoustics as pra

WORK = sys.argv[1]
FL = os.path.join(WORK, 'fleurs')
OUT = os.path.join(WORK, 'recordings')
os.makedirs(OUT, exist_ok=True)
RATE = 16000
refs = dict(l.rstrip('\n').split('\t', 1) for l in open(os.path.join(FL, 'refs.tsv'), encoding='utf-8'))
clips = sorted(refs)
A, B = clips[0:12:2][:5], clips[1:12:2][:5]          # two sets of voices, five clips each

def load(name):
    x, rate = sf.read(os.path.join(FL, name), dtype='float32')
    assert rate == RATE, name
    return x / (np.sqrt(np.mean(x ** 2)) + 1e-9) * 0.05  # every clip equally loud at the source

ROOM = [10.0, 6.0, 3.0]                               # a meeting room, metres
PHONE = [1.0, 3.0, 0.8]                               # on the table, near one end
absorption, max_order = pra.inverse_sabine(0.5, ROOM)  # office-like: RT60 about 0.5 s

def place(signal, distance):
    room = pra.ShoeBox(ROOM, fs=RATE, materials=pra.Material(absorption), max_order=max_order)
    room.add_source([PHONE[0] + distance, PHONE[1], 1.2], signal=signal)   # a seated talker's mouth
    room.add_microphone(PHONE)
    room.simulate()
    return room.mic_array.signals[0][: len(signal) + RATE // 2]

GAP = np.zeros(int(0.6 * RATE), dtype=np.float32)

def turns(voices):
    """voices: [(clip, distance, gain dB)], one after another with short gaps."""
    parts = []
    for name, d, db in voices:
        parts += [10 ** (db / 20) * place(load(name), d), GAP]
    return np.concatenate(parts)

# One calibration for every file: a voice at 1 m comes out at -26 dBFS RMS, a
# typical phone level for a person across the table. Further away is quieter
# as the room makes it; nothing is normalised per file.
cal = turns([(A[0], 1.0, 0)])
scale = 10 ** (-26 / 20) / np.sqrt(np.mean(cal[cal != 0] ** 2))
rng = np.random.default_rng(7)

def write(name, x):
    x = x * scale + rng.normal(0, 10 ** (-65 / 20), len(x))   # quiet room plus microphone noise
    sf.write(os.path.join(OUT, name), np.clip(x, -1, 1).astype(np.float32), RATE, subtype='PCM_16')

def alternate(far):
    return [v for pair in zip([(a, 1.0, 0) for a in A], far) for v in pair]

for d in [1, 2, 3, 5, 8]:
    write(f'alone-{d}m.wav', turns([(b, d, 0) for b in B]))
    write(f'mixed-{d}m.wav', turns(alternate([(b, d, 0) for b in B])))
for x in [6, 12, 18, 24]:
    write(f'quiet-{x}db.wav', turns(alternate([(b, 3.0, -x) for b in B])))
json.dump({'A': [refs[a] for a in A], 'B': [refs[b] for b in B]},
          open(os.path.join(WORK, 'refs.json'), 'w'), indent=1)
print(f'{len(os.listdir(OUT))} recordings in {OUT}')
