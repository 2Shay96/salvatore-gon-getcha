"""Build a separate, uninstalled vanilla sprite-selection experiment.

The game's existing Mr Handy KF sequences drive three added skeleton nodes.
Each node contains a free-running 24 fps UV flipbook; managed bool visibility selects a clip.
This is a hypothesis awaiting game testing, not an accepted runtime feature.
Bethesda-derived NIF/KF outputs stay in ignored build/, never in source control.
"""
import argparse
import copy
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
time.clock = time.perf_counter
from pyffi.formats.nif import NifFormat as N
import sprite_flipbook as flipbook

GATES = {k: ('SalvatoreProbe_' + k).encode() for k in ('idle', 'main', 'attack')}


def read(path):
    data = N.Data()
    data.read(io.BytesIO(path.read_bytes()))
    return data


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    stream = io.BytesIO()
    data.write(stream)
    path.write_bytes(stream.getvalue())
    check = read(path)
    return check


def selected(path, sequence):
    name = sequence.name.decode('ascii').lower()
    # AimDown is one of the spin attack variants; preserve filename information.
    if 'attack' in path.stem.lower():
        return 'attack'
    if path.stem.lower() == 'mtidle' or name == 'idle' or 'idleanims' in path.parts:
        return 'idle'
    return 'main'


def pose(visible):
    interpolator = N.NiBoolInterpolator()
    interpolator.bool_value = visible
    return interpolator


def ping_pong_order(count):
    """No endpoint duplicate: 0..N-1, N-2..1, then wrap to 0."""
    if count < 2:
        raise ValueError('ping-pong requires at least two frames')
    return list(range(count)) + list(range(count - 2, 0, -1))


def ping_pong_uv(sprite, count, fps=24):
    order = ping_pong_order(count)
    controllers = [b for b in sprite.blocks if isinstance(b, N.NiTextureTransformController)]
    assert len(controllers) == 2
    for controller in controllers:
        keys = controller.interpolator.data.data.keys
        assert len(keys) == count * 2 + 1, 'Expected original forward-only atlas track'
        forward_values = [keys[i * 2].value for i in range(count)]
        controller.stop_time = len(order) / fps
        controller.interpolator = flipbook._float_track(
            N, [forward_values[i] for i in order], fps, controller.stop_time)
    return order


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--media', type=Path, required=True)
    parser.add_argument('--main-ping-pong', action='store_true', help='also reverse the main clip at its endpoints')
    parser.add_argument('--reuse-clips', action='store_true', help='reuse already-built flipbook assets')
    args = parser.parse_args()
    if os.environ.get('PYTHONHASHSEED') != '0':
        return subprocess.call([sys.executable, *sys.argv], env=dict(os.environ, PYTHONHASHSEED='0'))
    dest = ROOT / 'build/salvatore_animation_probe'
    base = ROOT / 'build/salvatore_ganacci/meshes/creatures/salvatoreganacci'
    meshdir = dest / 'meshes/creatures/salvatoreanimationprobe'
    meshdir.mkdir(parents=True, exist_ok=True)
    counts = {}
    for clip in GATES:
        folder = args.media / ('salvatore_' + clip + '_sprites')
        counts[clip] = len(list(folder.glob('*.png')))
        if args.reuse_clips:
            assert (dest / 'meshes/salvatoreanimationprobe' / (clip + '.nif')).exists()
            assert (dest / 'textures/salvatoreanimationprobe' / (clip + '.dds')).exists()
            continue
        subprocess.run([sys.executable, str(ROOT / 'sprite_flipbook.py'), '--frames', str(folder),
                        '--out', str(dest), '--name', 'salvatoreanimationprobe/' + clip,
                        '--fps', '24', '--frame-units', '195', '--max-side', '1024',
                        '--animation', 'uv', '--root-name', 'Probe_' + clip], check=True)
    skeleton = read(base / 'skeleton.nif')
    root = skeleton.roots[0]
    for clip, name in GATES.items():
        gate = N.NiNode()
        gate.name = name
        gate.flags = 14 if clip == 'idle' else 15  # Bit 0: hidden, not a small visible copy.
        gate.scale = 1.0
        controller = N.NiVisController()
        controller.flags = 108  # Managed visibility, as in canonical sprite builder.
        controller.frequency = 1.0
        controller.start_time = 0.0
        controller.stop_time = 6.666667
        blend = N.NiBlendBoolInterpolator()
        # niftools/nifxml: flag byte bit0 manager-controlled, bit1 highest-weight-only;
        # PyFFI packs that flag byte and array-size byte in unknown_short.
        blend.unknown_short = 515  # 0x0203: flags 3, array size 2.
        blend.unknown_int = 0
        blend.bool_value = 2  # Invalid until the animation manager supplies a value.
        controller.interpolator = blend
        gate.add_controller(controller)
        sprite = read(dest / 'meshes/salvatoreanimationprobe' / (clip + '.nif'))
        if clip == 'idle' or (clip == 'main' and args.main_ping_pong):
            ping_pong_uv(sprite, counts[clip])
        # Embed geometry directly in the skeleton so KF targets exist before sequence binding.
        for block in sprite.blocks:
            if isinstance(block, N.NiAVObject):
                block.name = name + b'_' + block.name
        for child in sprite.roots[0].children:
            if child is not None:
                gate.add_child(child)
        root.add_child(gate)
    checked = write(meshdir / 'skeleton.nif', skeleton)
    assert all(sum(b.name == name for b in checked.blocks if isinstance(b, N.NiNode)) == 1
               for name in GATES.values())
    assert sum(isinstance(b, N.NiTextureTransformController) for b in checked.blocks) == 6
    idle_gate = next(b for b in checked.blocks if isinstance(b, N.NiNode) and b.name == GATES['idle'])
    idle_tracks = [b for b in idle_gate.tree() if isinstance(b, N.NiTextureTransformController)]
    assert len(idle_tracks) == 2
    for track in idle_tracks:
        assert abs(track.stop_time - (2 * counts['idle'] - 2) / 24) < 1e-6
        assert len(track.interpolator.data.data.keys) == (2 * counts['idle'] - 2) * 2 + 1
    if args.main_ping_pong:
        main_gate = next(b for b in checked.blocks if isinstance(b, N.NiNode) and b.name == GATES['main'])
        tracks = [b for b in main_gate.tree() if isinstance(b, N.NiTextureTransformController)]
        assert len(tracks) == 2
        for track in tracks:
            assert abs(track.stop_time - (2 * counts['main'] - 2) / 24) < 1e-6
            assert len(track.interpolator.data.data.keys) == (2 * counts['main'] - 2) * 2 + 1
    sequences = []
    for source in sorted(base.rglob('*.kf')):
        data = read(source)
        seq = data.roots[0]
        choice = selected(source, seq)
        original = seq.num_controlled_blocks
        seq.num_controlled_blocks += 3
        seq.controlled_blocks.update_size()
        for i, (clip, name) in enumerate(GATES.items(), start=original):
            link = seq.controlled_blocks[i]
            link.node_name = name
            link.controller_type = b'NiVisController'
            link.priority = {'idle': 0, 'main': 20, 'attack': 127}[choice]
            link.interpolator = pose(clip == choice)
        target = meshdir / source.relative_to(base)
        checked = write(target, data).roots[0]
        assert checked.num_controlled_blocks == original + 3
        assert [b.node_name for b in checked.controlled_blocks[-3:]] == list(GATES.values())
        assert sum(bool(b.interpolator.bool_value) for b in checked.controlled_blocks[-3:]) == 1
        sequences.append({'file': str(source.relative_to(base)), 'sequence': seq.name.decode(), 'clip': choice})
    source_spec = json.loads((ROOT / 'projects/salvatore/salvatore.spec.json').read_text())
    names = ['SalvatoreGanacciCreature', 'SalvatoreGanacciFist',
             'SalvatoreGanacciEmbeddedWeapons', 'SalvatoreGanacciVoice']
    spec = {k: copy.deepcopy(source_spec[k]) for k in ('schema', 'author', 'masters')}
    spec.update(project_id='salvatore_animation_probe', plugin='SalvatoreAnimationProbe.esp',
                description='Temporary vanilla sprite selection test; not the release mod.',
                records={k: source_spec['records'][k] for k in names}, cells={})
    fields = spec['records']['SalvatoreGanacciCreature']['fields']
    fields[:] = [f for f in fields if f.get('tag') not in {'SCRI', 'PKID', 'NIFZ'}
                 and 'insert' not in f]
    # Keep the proven Ganacci Fist inventory entry as well as its embedded-weapon list.
    # Initial stripped probe omitted CNTO and spawned hostile but did not attack (9 Oct game report).
    fields += [{'tag': tag, 'remove': True} for tag in ('SCRI', 'PKID', 'NIFZ', 'CSCR', 'CSDT', 'CSDI', 'CSDC')]
    for f in fields:
        if f.get('tag') == 'MODL':
            f['text'] = r'Creatures\SalvatoreAnimationProbe\Skeleton.nif'
        if f.get('tag') == 'FULL':
            f['text'] = 'Salvatore Animation Probe'
    # Explicitly remove inherited body parts; this prototype embeds all three directly in skeleton.
    spec = json.loads(json.dumps(spec).replace('SalvatoreGanacci', 'SalvatoreProbe'))
    projectdir = ROOT / 'projects/salvatore/animation-probe'
    projectdir.mkdir(exist_ok=True)
    (projectdir / 'probe.spec.json').write_text(json.dumps(spec, indent=1) + '\n')
    profile = {'schema_version': 1, 'project_id': 'salvatore_animation_probe', 'launcher': 'native',
               'game_root': 'C:/Program Files (x86)/Steam/steamapps/common/Fallout New Vegas',
               'plugin': spec['plugin'], 'records': [], 'state_dir': '../../../state',
               'helper': '../../../../geck-mcp-release/GECK-MCP-for-New-Vegas/bridge/Probe.Mcp.exe',
               'spec': 'probe.spec.json'}
    (projectdir / 'probe.windows.json').write_text(json.dumps(profile, indent=1) + '\n')
    files = {str(f.relative_to(dest)).replace('\\', '/'): {'sha256': hashlib.sha256(f.read_bytes()).hexdigest()}
             for f in dest.rglob('*') if f.suffix.lower() in ('.nif', '.kf', '.dds') and
             not str(f.relative_to(dest)).replace('\\', '/').startswith('meshes/salvatoreanimationprobe/')}
    manifest = {'character': {'id': 'salvatore_animation_probe'}, 'files': files, 'sequences': sequences,
                'frames': counts, 'fps': 24, 'status': 'offline-checked; runtime-unverified',
                'idlePlayback': {'mode': 'ping-pong', 'frameOrder': ping_pong_order(counts['idle']),
                                 'loopSeconds': (2 * counts['idle'] - 2) / 24},
                'limitations': ['Free UV phases do not restart on state entry.',
                                'Managed visibility binding/priority still needs game acceptance.',
                                'Animation attack multiplier may affect KF gates but not free UV speed.']}
    (dest / 'probe.build.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps({'ok': True, 'files': len(files), 'sequences': len(sequences), 'frames': counts,
                      'spec': str(projectdir / 'probe.spec.json'), 'status': manifest['status']}))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
