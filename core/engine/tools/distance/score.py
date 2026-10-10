# score.py <work> <results dir>: word errors per recording, and how much of
# each voice's speech reached the final transcript, from scribatic_replay output.
import json, os, re, sys, unicodedata

refs = json.load(open(os.path.join(sys.argv[1], 'refs.json'), encoding='utf-8'))
results = sys.argv[2]

def words(text):
    text = unicodedata.normalize('NFKC', text).lower()
    text = re.sub(r"[^\w\s']", ' ', text)
    return [w.strip("'") for w in text.split() if w.strip("'")]

def final(path):
    out = open(path, encoding='utf-8', errors='replace').read()
    if '== final' not in out:
        return None
    lines = out.split('== final', 1)[1].splitlines()[1:]
    return ' '.join(re.sub(r'^\s*[\d.]+-\s*[\d.]+\s+(S\d+\s+)?', '', l)
                    for l in lines if re.match(r'^\s*[\d.]+-', l))

def align(ref, hyp):
    """Edit distance, and which reference words were matched exactly."""
    n, m = len(ref), len(hyp)
    d = [[0] * (m + 1) for _ in range(n + 1)]
    for i in range(n + 1): d[i][0] = i
    for j in range(m + 1): d[0][j] = j
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            d[i][j] = min(d[i-1][j] + 1, d[i][j-1] + 1, d[i-1][j-1] + (ref[i-1] != hyp[j-1]))
    hit, i, j = [False] * n, n, m
    while i > 0 and j > 0:
        if d[i][j] == d[i-1][j-1] + (ref[i-1] != hyp[j-1]):
            hit[i-1] = ref[i-1] == hyp[j-1]; i, j = i - 1, j - 1
        elif d[i][j] == d[i-1][j] + 1: i -= 1
        else: j -= 1
    return d[n][m], hit

order = {'alone': 0, 'mixed': 1, 'quiet': 2}
rows = []
for name in os.listdir(results):
    m = re.match(r'(alone|mixed|quiet)-(\d+)(m|db)\.txt$', name)
    if not m:
        continue
    kind, size, unit = m.group(1), int(m.group(2)), m.group(3)
    hyp = final(os.path.join(results, name))
    if kind == 'alone':
        voices = [('B', words(t)) for t in refs['B']]
    else:
        voices = [v for pair in zip([('A', words(t)) for t in refs['A']],
                                    [('B', words(t)) for t in refs['B']]) for v in pair]
    label = f'{kind} {size}{"m" if unit == "m" else " dB quieter"}'
    if hyp is None:
        rows.append((order[kind], size, f'{label:20s} replay failed')); continue
    ref = [w for _, ws in voices for w in ws]
    who = [v for v, ws in voices for _ in ws]
    errors, hit = align(ref, words(hyp))
    heard = lambda v: 100 * sum(h for h, w in zip(hit, who) if w == v) / max(1, who.count(v))
    near = f'near voice heard {heard("A"):5.1f}%  ' if kind != 'alone' else ' ' * 25
    rows.append((order[kind], size, f'{label:20s} WER {100 * errors / len(ref):5.1f}%  {near}far voice heard {heard("B"):5.1f}%'))

for _, _, line in sorted(rows):
    print(line)
