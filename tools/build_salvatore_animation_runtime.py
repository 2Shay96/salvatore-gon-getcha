"""Rebuild Salvatore base assets, then apply its accepted vanilla animation selectors.

Use this instead of the base asset builder for the integrated character. Inputs
are project-owned frames; Bethesda-derived outputs stay in ignored build/.
"""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time
import wave

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
time.clock = time.perf_counter
from pyffi.formats.nif import NifFormat as N
import build_sprite_assets as assets


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('config', type=Path)
    ap.add_argument('--install', action='store_true')
    ap.add_argument('--reuse-build', action='store_true', help='reuse just-built base/prototype assets for path rebinding')
    args = ap.parse_args()
    if os.environ.get('PYTHONHASHSEED') != '0':
        return subprocess.call([sys.executable, *sys.argv], env=dict(os.environ, PYTHONHASHSEED='0'))
    config_path = args.config.resolve()
    config = json.loads(config_path.read_text())
    media = (config_path.parent / config['media']).resolve()
    base_assets = (config_path.parent / config['assets']).resolve()
    data_dir = Path(config['gameData'])
    # Always regenerate vanilla base first: do not append selectors to an already-patched skeleton.
    if not args.reuse_build:
        subprocess.run([sys.executable, str(ROOT / 'tools/build_sprite_assets.py'), str(base_assets),
                        '--data', str(data_dir)], check=True)
        subprocess.run([sys.executable, str(ROOT / 'tools/salvatore_animation_probe.py'),
                        '--media', str(media)] +
                       (['--main-ping-pong'] if config.get('mainPlayback') == 'ping-pong' else []), check=True)
    out = ROOT / 'build/salvatore_ganacci'
    probe = ROOT / 'build/salvatore_animation_probe'
    manifest_path = out / 'salvatore_ganacci.build.json'
    manifest = json.loads(manifest_path.read_text())
    preserve = {p: v['sha256'] for p, v in manifest['files'].items()
                if not p.startswith('meshes/creatures/salvatoreganacci/')}
    # Rebind node labels and paths away from the prototype namespace.
    folder = probe / 'meshes/creatures/salvatoreanimationprobe'
    for source in sorted(folder.rglob('*')):
        if source.suffix.lower() not in ('.kf', '.nif'):
            continue
        d = N.Data()
        d.read(io.BytesIO(source.read_bytes()))
        for b in d.blocks:
            if isinstance(b, N.NiObjectNET) and b.name.startswith(b'SalvatoreProbe_'):
                b.name = b.name.replace(b'SalvatoreProbe_', b'SalvatoreGanacciClip_', 1)
            if hasattr(b, 'file_name'):
                b.file_name = b.file_name.replace(b'salvatoreanimationprobe', b'salvatoreganacci')
            if isinstance(b, N.NiControllerSequence):
                for link in b.controlled_blocks:
                    if link.node_name.startswith(b'SalvatoreProbe_'):
                        link.node_name = link.node_name.replace(b'SalvatoreProbe_', b'SalvatoreGanacciClip_', 1)
        target = out / 'meshes/creatures/salvatoreganacci' / source.relative_to(folder)
        stream = io.BytesIO()
        d.write(stream)
        target.write_bytes(stream.getvalue())
        check = N.Data()
        check.read(io.BytesIO(target.read_bytes()))
        if source.suffix == '.kf':
            assert all(link.node_name.startswith(b'SalvatoreGanacciClip_')
                       for link in check.roots[0].controlled_blocks[-3:])
        else:
            gates = [b for b in check.blocks if isinstance(b, N.NiNode)
                     and b.name in [b'SalvatoreGanacciClip_idle', b'SalvatoreGanacciClip_main',
                                    b'SalvatoreGanacciClip_attack']]
            assert len(gates) == 3 and all(g.scale == 1 for g in gates)
            assert all(isinstance(g.controller, N.NiVisController) for g in gates)
            assert not any(b'salvatoreanimationprobe' in b.file_name for b in check.blocks
                           if hasattr(b, 'file_name'))
    for clip in ('idle', 'main', 'attack'):
        target = out / 'textures/salvatoreganacci' / (clip + '.dds')
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(probe / 'textures/salvatoreanimationprobe' / (clip + '.dds'), target)
    assert all(sha(out / path) == value for path, value in preserve.items())
    # Script-owned one-shot banks are separate from the base builder's fixed cue slots.
    for cue, item in config.get('speechAudio', {}).items():
        source = (config_path.parent / item['file']).resolve()
        relative = Path(item['install'])
        assert not relative.is_absolute() and '..' not in relative.parts
        assert relative.as_posix().startswith('sound/fx/salvatore/')
        with wave.open(str(source)) as audio:
            assert (audio.getnchannels(), audio.getsampwidth(), audio.getframerate(),
                    audio.getcomptype()) == (1, 2, 44100, 'NONE'), cue
            audio.readframes(audio.getnframes())
        target = out / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        assert sha(source) == sha(target)
    if config.get('themeEmitter'):
        # A render root without geometry/collision owns the scripted ambient loop.
        emitter = N.Data()
        emitter.read(io.BytesIO((out / 'meshes/salvatore/salvatore_down.nif').read_bytes()))
        node = N.NiNode()
        node.name = b'SalvatoreThemeEmitter'
        node.flags = 14
        emitter.roots = [node]
        target = out / 'meshes/salvatore/theme_emitter.nif'
        with target.open('wb') as stream:
            emitter.write(stream)
        check = N.Data()
        check.read(io.BytesIO(target.read_bytes()))
        assert len(check.blocks) == 1 and isinstance(check.roots[0], N.NiNode)
        assert check.roots[0].num_children == 0 and check.roots[0].collision_object is None
    manifest['files'] = {p.relative_to(out).as_posix().lower(): {'size': p.stat().st_size, 'sha256': sha(p)}
                         for p in assets.installable(out)}
    manifest['animationRuntime'] = {'config': str(config_path), 'fps': 24, 'selector': 'managed-visibility',
                                    'idleMode': 'ping-pong', 'idleLoopSeconds': 2.75,
                                    'mainMode': config.get('mainPlayback', 'forward'),
                                    'mainLoopSeconds': 5.75 if config.get('mainPlayback') == 'ping-pong' else 70 / 24,
                                    'sourceHashes': {p.relative_to(media).as_posix(): sha(p)
                                                     for p in media.rglob('*.png')},
                                    'status': 'integrated; main-mod-game-test-pending'}
    manifest_path.write_text(json.dumps(manifest, indent=1) + '\n')
    result = {'ok': True, 'files': len(manifest['files']), 'manifest': str(manifest_path),
              'defeatedSoundIconPreserved': True}
    if args.install:
        done = assets.install(out, manifest, data_dir, ROOT / 'state')
        result['install'] = {k: len(v) if isinstance(v, list) else v for k, v in done.items()}
    print(json.dumps(result))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
