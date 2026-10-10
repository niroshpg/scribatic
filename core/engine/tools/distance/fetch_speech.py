# Fetches English speech with human transcripts: the first clips of FLEURS'
# en_us dev split (CC BY 4.0), streamed so only what is used is downloaded.
# Writes <work>/fleurs/<id>.wav and <work>/fleurs/refs.tsv.
import csv, io, os, sys, tarfile, urllib.request

WORK = sys.argv[1]
WANT = 26
BASE = 'https://huggingface.co/datasets/google/fleurs/resolve/main/data/en_us'
out = os.path.join(WORK, 'fleurs')
os.makedirs(out, exist_ok=True)
if os.path.exists(os.path.join(out, 'refs.tsv')):
    sys.exit(0)

tsv = urllib.request.urlopen(f'{BASE}/dev.tsv', timeout=60).read().decode('utf-8')
refs = {}
for row in csv.reader(io.StringIO(tsv), delimiter='\t', quoting=csv.QUOTE_NONE):
    if len(row) > 3:
        refs[row[1]] = row[2]          # file name -> transcript as spoken

kept = {}
with urllib.request.urlopen(f'{BASE}/audio/dev.tar.gz', timeout=120) as stream:
    with tarfile.open(fileobj=stream, mode='r|gz') as tar:
        for member in tar:
            name = os.path.basename(member.name)
            if not member.isfile() or name not in refs or name in kept:
                continue
            with open(os.path.join(out, name), 'wb') as f:
                f.write(tar.extractfile(member).read())
            kept[name] = refs[name]
            if len(kept) >= WANT:
                break

with open(os.path.join(out, 'refs.tsv'), 'w', encoding='utf-8') as f:
    for name, text in kept.items():
        f.write(f'{name}\t{text}\n')
print(f'{len(kept)} clips in {out}')
