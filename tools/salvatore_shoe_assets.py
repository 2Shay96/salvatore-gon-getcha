"""Add a shoe visibility layer to the existing single-actor animation runtime.

The special-idle sequences preserve native idle bone bindings plus a visibility
override. No root-motion edits, collision, AI or health changes.
Called by build_salvatore_animation_runtime after its normal three-clip build.
"""
import io
import json
from pathlib import Path
import subprocess
import sys
import time

time.clock = time.perf_counter
from pyffi.formats.nif import NifFormat as N

ROOT = Path(__file__).resolve().parents[1]
GATES = [b'SalvatoreGanacciClip_' + name for name in (b'idle', b'main', b'attack', b'shoe')]


def read(path):
    data = N.Data()
    data.read(io.BytesIO(path.read_bytes()))
    return data


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    stream = io.BytesIO()
    data.write(stream)
    path.write_bytes(stream.getvalue())
    return read(path)


def pose(value):
    result = N.NiBoolInterpolator()
    result.bool_value = value
    return result


def link(sequence, index, gate, visible, priority):
    item = sequence.controlled_blocks[index]
    item.node_name = gate
    item.controller_type = b'NiVisController'
    item.priority = priority
    item.interpolator = pose(visible)


def apply(config, media, out):
    settings = config['shoe']
    frames = media / settings['folder']
    count = len(list(frames.glob('*.png')))
    assert count > 1
    subprocess.run([sys.executable, str(ROOT / 'sprite_flipbook.py'), '--frames', str(frames),
                    '--out', str(out), '--name', 'salvatoreganacci/shoe', '--fps', str(config['fps']),
                    '--frame-units', str(settings['frameUnits']), '--max-side', str(config['maxSide']),
                    '--animation', 'uv', '--root-name', 'ShoeSprite'], check=True)
    folder = out / 'meshes/creatures/salvatoreganacci'
    skeleton = read(folder / 'skeleton.nif')
    assert not any(isinstance(b, N.NiNode) and b.name == GATES[-1] for b in skeleton.blocks), 'Rebuild base runtime before shoe'
    gate = N.NiNode()
    gate.name = GATES[-1]
    gate.flags = 15
    gate.scale = 1
    controller = N.NiVisController()
    controller.flags = 108
    controller.frequency = 1
    controller.start_time = 0
    controller.stop_time = count / config['fps']
    blend = N.NiBlendBoolInterpolator()
    blend.unknown_short = 515
    blend.unknown_int = 0
    blend.bool_value = 2
    controller.interpolator = blend
    gate.add_controller(controller)
    sprite = read(out / 'meshes/salvatoreganacci/shoe.nif')
    if settings.get('playback') == 'ping-pong':
        from salvatore_animation_probe import ping_pong_uv
        ping_pong_uv(sprite, count, config['fps'])
        # The ACTI uses this standalone model; the actor embeds the same data.
        write(out / 'meshes/salvatoreganacci/shoe.nif', sprite)
    for block in sprite.blocks:
        if isinstance(block, N.NiAVObject):
            block.name = GATES[-1] + b'_' + block.name
    for child in sprite.roots[0].children:
        if child is not None:
            gate.add_child(child)
    skeleton.roots[0].add_child(gate)
    write(folder / 'skeleton.nif', skeleton)
    changed = []
    for path in sorted(folder.rglob('*.kf')):
        if path.stem in ('specialidle_shoeride', 'specialidle_shoeexit'):
            continue
        data = read(path)
        seq = data.roots[0]
        assert not any(item.node_name == GATES[-1] for item in seq.controlled_blocks)
        size = seq.num_controlled_blocks
        seq.num_controlled_blocks += 1
        seq.controlled_blocks.update_size()
        link(seq, size, GATES[-1], False, seq.controlled_blocks[size-1].priority)
        write(path, data)
        changed.append(path.relative_to(out).as_posix())
    for name, shoe in [('ShoeRide', True), ('ShoeExit', False)]:
        data = read(folder / 'mtidle.kf')
        seq = data.roots[0]
        seq.name = ('SpecialIdle_' + name).encode()
        seq.start_time = 0
        seq.stop_time = count / config['fps'] if shoe else .1
        seq.cycle_type = 0 if shoe else 2  # loop / clamp, matching NIF enum
        # Preserve the canonical skeletal animation tracks. A bool-only sequence
        # has not played in the user's game; don't strip the native bone bindings.
        bone_count = seq.num_controlled_blocks - 4  # existing idle/main/attack/shoe gates
        assert all(item.controller_type == b'NiVisController' for item in seq.controlled_blocks[-4:])
        seq.num_controlled_blocks = bone_count + 4
        seq.controlled_blocks.update_size()
        for i, node in enumerate(GATES):
            link(seq, bone_count + i, node, i == (3 if shoe else 0), 255)
        seq.text_keys.num_text_keys = 2
        seq.text_keys.text_keys.update_size()
        for key, (t, value) in zip(seq.text_keys.text_keys, [(0, b'start'), (seq.stop_time, b'end')]):
            key.time, key.value = t, value
        # Clear any old transform-controller bookkeeping inherited by each controlled link.
        for item in seq.controlled_blocks[-4:]:
            item.controller = None
        checked = write(folder / 'idleanims' / ('specialidle_' + name.lower() + '.kf'), data)
        assert len(checked.roots[0].controlled_blocks) == bone_count + 4
        assert all(item.controller_type == b'NiVisController' for item in checked.roots[0].controlled_blocks[-4:])
    positions = 2 * count - 2 if settings.get('playback') == 'ping-pong' else count
    return {'frames': count, 'fps': config['fps'], 'seconds': positions/config['fps'],
            'playback': settings.get('playback', 'forward'),
            'switchUnits': settings['switchUnits'], 'selector': 'single-actor special-idle visibility layer',
            'existingSequencesExtended': len(changed), 'status': 'gameplay-prototype-pending'}
