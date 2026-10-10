# Distance test

Whether people further from the phone make it into the transcript. Speech with
human transcripts (FLEURS English, CC BY 4.0) is placed in a simulated meeting
room — 10 × 6 × 3 m, reverberation about 0.5 s, phone on the table — and run
through the app's own pipeline with `scribatic_replay`: live pass, accurate
pass, speakers.

```bash
cmake --build build/core --target scribatic_replay
core/engine/tools/distance/run.sh            # results labelled "now"
core/engine/tools/distance/run.sh before /path/to/older/scribatic_replay
```

It prints, per recording, the word error rate and how many of each voice's
words reached the final transcript:

- **alone 1–8 m**: one talker at that distance.
- **mixed 1–8 m**: one talker at 1 m taking turns with one at that distance.
- **quiet 6–24 dB**: the far talker at 3 m and that much quieter — a soft
  voice, or a room that absorbs more. In a reverberant room distance alone
  costs little level; this is the case that loses words.

Needs Python 3 and network access the first time (a virtual environment and
26 clips are set up in `build/distance/`). The simulation does not model
the phone's own microphone processing, so a check in a real room still matters.

Results on 2026-10-10: distance alone and mixed kept 83–89% of the far voice's
words at every distance. A voice 24 dB under the nearest one kept 83% without
an initial prompt and 27% with the verbatim one, which is why the accurate pass
has none (EngineImpl.cpp).
