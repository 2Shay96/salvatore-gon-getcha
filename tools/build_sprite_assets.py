"""Build a sprite character's loose files (meshes, textures, skeleton, sounds, icon) from an assets.json.

    .venv\\Scripts\\python.exe tools\\build_sprite_assets.py projects\\salvatore\\assets.json
        [--out build\\<id>] [--data "<game>\\Data"] [--install] [--check]

assets.json uses Sprite Forge's Source Pack v6 names (clips, stateClips, audio slots), so a pack
importer can write it later; today it is written by hand. One pipeline for both.

Steps:
  1. Check the inputs: frames exist and share one size (view S only), WAVs are PCM16 mono/stereo,
     the icon is a DDS. --check stops here.
  2. Extract each collision template NIF from the game's BSAs into build\\_vanilla\\ (never installed).
  3. tools\\creature_skeleton.py: own skeleton + KFs under meshes\\creatures\\<name>\\.
  4. sprite_flipbook.py for each sprite (UV flipbook, billboard mode 5, -U/+V; see plan.md).
  5. Copy the WAVs and the icon to their install paths.
  6. Write <out>\\<id>.build.json: every installable file (.nif .kf .dds .wav) with size and SHA-256.
--install copies those files into Data. A different file already there is first moved to
state\\backups\\assets\\<time>\\ (never deleted); the receipt goes to state\\installs\\<id>-assets.json.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

DEFAULT_DATA = Path(r'C:\Program Files (x86)\Steam\steamapps\common\Fallout New Vegas\Data')
# Sprite Forge 0.7 states and audio slots (SF.Simulator.states, SF.Banks.labels).
STATES = ('hostile', 'hostile_attack', 'defeat_transition', 'defeated', 'stored', 'deploying',
          'companion_combat', 'neutral_idle', 'neutral_roam', 'repack')
AUDIO_SLOTS = ('music_loop', 'idle_loop', 'roam_loop', 'neutral_quips', 'idle_entry', 'hostile_entry',
               'companion_entry', 'defeat_sting', 'defeated_loop', 'release', 'attack', 'hit', 'pickup',
               'inventory', 'repack', 'alert', 'victory')
AUDIO_RATES = (22050, 44100, 48000)
INSTALL_EXT = ('.nif', '.kf', '.dds', '.wav')


class AssetError(Exception):
    pass


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def wav_info(path):
    """Parse a RIFF WAVE header. Returns {channels, rate, bits, frames, trailing:[chunk ids after data]}."""
    raw = Path(path).read_bytes()
    if raw[:4] != b'RIFF' or raw[8:12] != b'WAVE':
        raise AssetError('%s: not a RIFF WAVE file' % path)
    pos, fmt, data_size, order = 12, None, None, []
    while pos + 8 <= len(raw):
        cid, size = raw[pos:pos + 4], struct.unpack('<I', raw[pos + 4:pos + 8])[0]
        order.append(cid.decode('latin1'))
        if cid == b'fmt ':
            fmt = struct.unpack('<HHIIHH', raw[pos + 8:pos + 24])
        elif cid == b'data':
            data_size = size
        pos += 8 + size + (size & 1)
    if fmt is None or data_size is None:
        raise AssetError('%s: missing fmt or data chunk' % path)
    tag, channels, rate, _bps, align, bits = fmt
    if tag != 1 or bits != 16:
        raise AssetError('%s: must be PCM 16-bit (format %d, %d bits)' % (path, tag, bits))
    if channels not in (1, 2) or rate not in AUDIO_RATES:
        raise AssetError('%s: %d channels at %d Hz (want mono/stereo, %s)' % (path, channels, rate, AUDIO_RATES))
    trailing = order[order.index('data') + 1:]
    return {'channels': channels, 'rate': rate, 'bits': bits, 'frames': data_size // align,
            'seconds': round(data_size / align / rate, 3), 'trailing': trailing}


def png_size(path):
    raw = Path(path).read_bytes()[:24]
    if raw[:8] != b'\x89PNG\r\n\x1a\n':
        raise AssetError('%s: not a PNG' % path)
    return struct.unpack('>II', raw[16:24])


def load(path):
    """Read and check assets.json. Returns a plan with absolute paths. Raises AssetError."""
    path = Path(path).resolve()
    spec = json.loads(path.read_text(encoding='utf-8'))
    base = path.parent

    def res(rel):
        return (base / rel).resolve()

    if spec.get('schema') != 1 or spec.get('kind') != 'sprite_character_assets':
        raise AssetError('not a sprite_character_assets schema 1 file')
    char = spec.get('character', {})
    if not re.fullmatch(r'[a-z][a-z0-9_]{0,63}', char.get('id', '')):
        raise AssetError('character.id: lower-case letters, digits and underscores')
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9]{0,31}', char.get('editorPrefix', '')):
        raise AssetError('character.editorPrefix: letters and digits, max 32')
    clips = {}
    for cid, clip in spec.get('clips', {}).items():
        if clip.get('view', 'S') != 'S':
            raise AssetError('clips.%s: only view S is supported in v1' % cid)
        folder = res(clip['frames'])
        frames = sorted(folder.glob('*.png'))
        if not frames:
            raise AssetError('clips.%s: no PNG frames in %s' % (cid, folder))
        sizes = {png_size(f) for f in frames}
        if len(sizes) != 1:
            raise AssetError('clips.%s: frames differ in size %s' % (cid, sorted(sizes)))
        fps = clip.get('fps', 24)
        if not isinstance(fps, int) or not 1 <= fps <= 120:
            raise AssetError('clips.%s.fps: whole number 1-120 (the flipbook takes an integer fps)' % cid)
        clips[cid] = {'folder': folder, 'count': len(frames), 'size': list(sizes.pop()), 'fps': fps,
                      'sourceSha256': {f.name: sha(f) for f in frames}}
    states = spec.get('stateClips', {})
    for state, cid in states.items():
        if state not in STATES:
            raise AssetError('stateClips.%s: unknown state (Sprite Forge states: %s)' % (state, ', '.join(STATES)))
        if cid not in clips:
            raise AssetError('stateClips.%s: unknown clip %r' % (state, cid))
    sprites = {}
    for sid, sp in spec.get('sprites', {}).items():
        if sp.get('state') not in states:
            raise AssetError('sprites.%s.state: not in stateClips' % sid)
        if not re.fullmatch(r'[a-z0-9_]+(/[a-z0-9_]+)*', sp.get('name', '')):
            raise AssetError('sprites.%s.name: lower-case path like folder/name' % sid)
        if not float(sp.get('frameUnits', 0)) > 0:
            raise AssetError('sprites.%s.frameUnits: must be > 0' % sid)
        reuse = sp.get('reuseTextureOf')
        if reuse:
            if reuse not in sprites:
                raise AssetError('sprites.%s.reuseTextureOf: must name an earlier sprite' % sid)
            if states[sprites[reuse]['state']] != states[sp['state']]:
                raise AssetError('sprites.%s.reuseTextureOf: %s uses a different clip' % (sid, reuse))
        coll = sp.get('collision')
        if coll and (not coll.get('template', '').lower().endswith('.nif') or len(coll.get('box', [])) != 3):
            raise AssetError('sprites.%s.collision: needs template (.nif in a BSA) and box [x, y, height]' % sid)
        if coll and 'material' in coll and not re.fullmatch(r'[a-z_]+|\d+', str(coll['material']).lower()):
            raise AssetError('sprites.%s.collision.material: a Havok material name (organic, cloth...) or number' % sid)
        if coll and not isinstance(coll.get('static', False), bool):
            raise AssetError('sprites.%s.collision.static: true or false' % sid)
        sprites[sid] = dict(sp, clip=states[sp['state']])
    audio = {}
    for slot, a in spec.get('audio', {}).items():
        if slot not in AUDIO_SLOTS:
            raise AssetError('audio.%s: unknown slot (Sprite Forge slots: %s)' % (slot, ', '.join(AUDIO_SLOTS)))
        install = a.get('install', '').replace('\\', '/').lower()
        if not (install.startswith('sound/') and install.endswith('.wav')):
            raise AssetError('audio.%s.install: sound/....wav' % slot)
        audio[slot] = {'file': res(a['file']), 'install': install, 'info': wav_info(res(a['file']))}
    icon = spec.get('icon')
    if icon:
        install = icon.get('install', '').replace('\\', '/').lower()
        if not (install.startswith('textures/') and install.endswith('.dds')):
            raise AssetError('icon.install: textures/....dds')
        if res(icon['file']).read_bytes()[:4] != b'DDS ':
            raise AssetError('icon.file: not a DDS')
        icon = {'file': res(icon['file']), 'install': install}
    return {'source': str(path), 'character': char, 'clips': clips, 'stateClips': states,
            'sprites': sprites, 'skeleton': spec.get('skeleton'), 'audio': audio, 'icon': icon}


def vanilla_path(build_root, template):
    return Path(build_root) / '_vanilla' / template.lower().replace('\\', '/')


def commands(plan, out, data, build_root, python=sys.executable):
    """The tool runs, in order: skeleton first, then each sprite."""
    cmds = []
    sk = plan['skeleton']
    if sk:
        cmd = [python, str(ROOT / 'tools' / 'creature_skeleton.py'), '--source', sk['source'],
               '--name', sk['name'], '--out', str(out), '--data', str(data)]
        for old, new in sorted(sk.get('replaceSound', {}).items()):
            cmd += ['--replace-sound', '%s=%s' % (old, new)]
        if sk.get('hitSound'):
            cmd += ['--hit-sound', sk['hitSound']]
        for group, mult in sorted(sk.get('speed', {}).items()):
            cmd += ['--speed', '%s=%g' % (group, mult)]
        cmds.append(cmd)
    for sid, sp in plan['sprites'].items():
        clip = plan['clips'][sp['clip']]
        cmd = [python, str(ROOT / 'sprite_flipbook.py'), '--frames', str(clip['folder']), '--out', str(out),
               '--name', sp['name'], '--fps', str(clip['fps']), '--frame-units', '%g' % sp['frameUnits'],
               '--max-side', str(sp.get('maxSide', 2048)), '--root-name', sp.get('rootName', 'SpriteFlipbook'),
               '--offset-z', '%g' % sp.get('offsetZ', 0)]
        if sp.get('prn'):
            cmd += ['--prn', sp['prn']]
        if sp.get('reuseTextureOf'):
            cmd += ['--reuse-texture', plan['sprites'][sp['reuseTextureOf']]['name']]
        if sp.get('collision'):
            cmd += ['--collision-template', str(vanilla_path(build_root, sp['collision']['template'])),
                    '--collision-box', ','.join('%g' % v for v in sp['collision']['box'])]
            if 'material' in sp['collision']:
                cmd += ['--collision-material', str(sp['collision']['material'])]
            if sp['collision'].get('static'):
                cmd += ['--collision-static']
        cmds.append(cmd)
    return cmds


def extract_templates(plan, data, build_root):
    from geck_mcp.esp.assets import bsa_extract, bsa_names
    wanted = {sp['collision']['template'].lower() for sp in plan['sprites'].values() if sp.get('collision')}
    wanted = {w for w in wanted if not vanilla_path(build_root, w).exists()}
    for bsa in sorted(Path(data).glob('*.bsa')):
        if not wanted:
            break
        hits = wanted & bsa_names(bsa)
        for name, raw in bsa_extract(bsa, hits).items():
            target = vanilla_path(build_root, name)
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)
            wanted.discard(name)
    if wanted:
        raise AssetError('collision template(s) not found in any BSA: %s' % sorted(wanted))


def installable(out):
    return sorted(p for p in Path(out).rglob('*') if p.is_file() and p.suffix.lower() in INSTALL_EXT)


def build(plan, out, data, build_root, log=print):
    out = Path(out)
    if out.exists() and any(out.iterdir()):
        old = Path(build_root) / '_old' / ('%s-%s' % (out.name, time.strftime('%Y%m%d-%H%M%S')))
        old.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(out), str(old))            # regenerable build output: moved aside, not deleted
        log('previous build moved to %s' % old)
    out.mkdir(parents=True, exist_ok=True)
    extract_templates(plan, data, build_root)
    steps = []
    # PyFFI writes a NIF's string table in set order; a fixed hash seed makes rebuilds byte-identical.
    env = dict(os.environ, PYTHONHASHSEED='0')
    for cmd in commands(plan, out, data, build_root):
        started = time.time()
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8', errors='replace', env=env)
        steps.append({'command': cmd[1:], 'exit': proc.returncode, 'seconds': round(time.time() - started, 1),
                      'stdout': proc.stdout[-4000:], 'stderr': proc.stderr[-4000:]})
        log('%s exit %d (%.1fs)' % (Path(cmd[1]).name, proc.returncode, time.time() - started))
        if proc.returncode:
            raise AssetError('%s failed:\n%s' % (Path(cmd[1]).name, proc.stderr[-2000:] or proc.stdout[-2000:]))
    for item in list(plan['audio'].values()) + ([plan['icon']] if plan['icon'] else []):
        target = out / item['install']
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(item['file'], target)
    files = {p.relative_to(out).as_posix().lower(): {'size': p.stat().st_size, 'sha256': sha(p)}
             for p in installable(out)}
    manifest = {'schema': 1, 'character': plan['character'], 'assets': plan['source'],
                'built': time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'files': files, 'steps': steps,
                'clips': {k: {kk: (str(vv) if isinstance(vv, Path) else vv) for kk, vv in v.items()}
                          for k, v in plan['clips'].items()},
                'audio': {k: {'install': v['install'], 'info': v['info']} for k, v in plan['audio'].items()}}
    (out / (plan['character']['id'] + '.build.json')).write_text(json.dumps(manifest, indent=1) + '\n', encoding='utf-8')
    return manifest


def install(out, manifest, data, state, log=print):
    """Copy the build's files into Data. Different existing files are moved to a backup first."""
    out, data = Path(out), Path(data)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    backup_root = Path(state) / 'backups' / 'assets' / stamp
    done = {'time': time.strftime('%Y-%m-%dT%H:%M:%S%z'), 'data': str(data), 'installed': [], 'unchanged': [],
            'backedUp': []}
    for rel, info in sorted(manifest['files'].items()):
        src, dst = out / rel, data / rel
        if dst.exists():
            if sha(dst) == info['sha256']:
                done['unchanged'].append(rel)
                continue
            backup = backup_root / rel
            backup.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(dst), str(backup))
            done['backedUp'].append(rel)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        if sha(dst) != info['sha256']:
            raise AssetError('copy check failed for %s' % rel)
        done['installed'].append(rel)
    if done['backedUp']:
        done['backupFolder'] = str(backup_root)
    receipt = Path(state) / 'installs' / (manifest['character']['id'] + '-assets.json')
    history = json.loads(receipt.read_text(encoding='utf-8')) if receipt.exists() else []
    history.append(done)
    receipt.parent.mkdir(parents=True, exist_ok=True)
    receipt.write_text(json.dumps(history, indent=1) + '\n', encoding='utf-8')
    log('installed %d, unchanged %d, backed up %d' % (len(done['installed']), len(done['unchanged']),
                                                      len(done['backedUp'])))
    return done


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('assets', type=Path)
    ap.add_argument('--out', type=Path, help='default build\\<character id>')
    ap.add_argument('--data', type=Path, default=DEFAULT_DATA)
    ap.add_argument('--state', type=Path, default=ROOT / 'state')
    ap.add_argument('--check', action='store_true', help='only check the inputs')
    ap.add_argument('--install', action='store_true', help='copy the built files into Data')
    args = ap.parse_args()
    try:
        plan = load(args.assets)
        summary = {'character': plan['character']['id'],
                   'clips': {k: {'frames': v['count'], 'size': v['size'], 'fps': v['fps']} for k, v in plan['clips'].items()},
                   'audio': {k: dict(v['info'], install=v['install']) for k, v in plan['audio'].items()}}
        warnings = ['%s: chunks after data %s' % (k, v['info']['trailing'])
                    for k, v in plan['audio'].items() if v['info']['trailing']]
        if args.check:
            print(json.dumps(dict(summary, ok=True, warnings=warnings), indent=1))
            return 0
        build_root = ROOT / 'build'
        out = args.out or build_root / plan['character']['id']
        manifest = build(plan, out, args.data, build_root)
        result = dict(summary, ok=True, warnings=warnings, out=str(out), files=len(manifest['files']))
        if args.install:
            done = install(out, manifest, args.data, args.state)
            result['install'] = {k: (len(v) if isinstance(v, list) else v) for k, v in done.items()}
        print(json.dumps(result, indent=1))
        return 0
    except AssetError as error:
        print(json.dumps({'ok': False, 'error': str(error)}))
        return 2


if __name__ == '__main__':
    sys.exit(main())
