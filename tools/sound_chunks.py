"""Cut a seamless loop WAV into N chunks that a script can chain with PlaySound3D.

    python3 tools/sound_chunks.py LOOP.wav OUTDIR PREFIX N [--overlap-ms 80] [--gain-db 0]

Why: FNV has no vanilla way to pause/resume a looping sound on a moving actor. A script that plays
short one-shot chunks on the actor (PlaySound3D) gets all of that for free: the sound comes from the
actor, it stops with the game in menus, and the next chunk continues where the music left off.

Chunk k holds loop samples [k*L/N, (k+1)*L/N) plus OVERLAP of the following audio (wrapping round the
loop) that fades out linearly, and its first OVERLAP fades in linearly, so two chunks started
exactly L/N seconds apart crossfade with constant gain (the overlapping parts are the same audio).
That absorbs a frame or two of script timing jitter. Prints JSON with the chunk period to use.
"""
import argparse
import json
from pathlib import Path
import wave

import numpy as np

ap = argparse.ArgumentParser()
ap.add_argument('loop', type=Path)
ap.add_argument('outdir', type=Path)
ap.add_argument('prefix')
ap.add_argument('count', type=int)
ap.add_argument('--overlap-ms', type=float, default=80)
ap.add_argument('--gain-db', type=float, default=0)
a = ap.parse_args()
w = wave.open(str(a.loop))
p = w.getparams()
if p.sampwidth != 2:
    raise SystemExit('16-bit PCM only')
x = np.frombuffer(w.readframes(p.nframes), dtype='<i2').reshape(-1, p.nchannels).astype(np.float64)
x *= 10 ** (a.gain_db / 20)
total = len(x)
ov = int(round(a.overlap_ms / 1000 * p.framerate))
a.outdir.mkdir(parents=True, exist_ok=True)
files = []
for k in range(a.count):
    start = int(round(k * total / a.count))
    end = int(round((k + 1) * total / a.count))
    idx = np.arange(start, end + ov) % total
    chunk = x[idx].copy()
    ramp = np.linspace(0, 1, ov, endpoint=False)[:, None]
    chunk[:ov] *= ramp
    chunk[-ov:] *= ramp[::-1]
    out = a.outdir / ('%s_%02d.wav' % (a.prefix, k + 1))
    o = wave.open(str(out), 'wb')
    o.setnchannels(p.nchannels); o.setsampwidth(2); o.setframerate(p.framerate)
    o.writeframes(np.clip(np.round(chunk), -32768, 32767).astype('<i2').tobytes())
    o.close()
    files.append(out.name)
print(json.dumps({'source': str(a.loop), 'loopSeconds': total / p.framerate, 'count': a.count,
                  'periodSeconds': total / a.count / p.framerate, 'overlapMs': a.overlap_ms, 'files': files}))
