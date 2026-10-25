"""Back visibility gates with zero-scale transforms for the VATS highlight pass."""
from salvatore_shoe_assets import N, GATES, read, write


def instant_transitions(sequence):
    """Use the vanilla zero-duration blend key for this sprite-only actor.

    Scale poses are constant, but the actor animation manager can still ease
    between them. A skeleton blend flag alone did not prevent that in M6.
    Keep every non-blend event (including hit/sound/loop) at its original time.
    One common Blend key avoids conflicting Blend/Blendin/Blendout overrides.
    Reference: https://geckwiki.com/index.php?title=NiTextKeyExtraData
    """
    keys = sequence.text_keys
    assert keys is not None
    events = []
    for key in keys.text_keys:
        lines = [line for line in key.value.splitlines()
                 if line.strip().split(b':', 1)[0].lower()
                 not in (b'blend', b'blendin', b'blendout')]
        if lines:
            # Preserve the original bytes when no line needed removal.
            value = key.value if len(lines) == len(key.value.splitlines()) else b'\r\n'.join(lines)
            events.append((key.time, value))
    assert events and any(v.strip().lower() == b'start' for _, v in events)
    events.append((sequence.start_time, b'Blend: 0'))
    events.sort(key=lambda item: item[0])
    keys.num_text_keys = len(events)
    keys.text_keys.update_size()
    for key, (stamp, value) in zip(keys.text_keys, events):
        key.time, key.value = stamp, value


def apply(out):
    folder = out / 'meshes/creatures/salvatoreganacci'
    skeleton = read(folder / 'skeleton.nif')
    for gate in skeleton.roots[0].children:
        if gate is None or gate.name not in GATES:
            continue
        gate.scale = 1.0 if gate.name == GATES[0] else 0.0
        controller = N.NiTransformController()
        controller.flags = 108
        controller.frequency = 1.0
        controller.start_time = 0.0
        controller.stop_time = 6.666667
        blend = N.NiBlendTransformInterpolator()
        blend.unknown_short = 515  # manager-controlled; highest weight only, same as visibility
        blend.unknown_int = 0
        controller.interpolator = blend
        gate.add_controller(controller)
    write(folder / 'skeleton.nif', skeleton)
    count = 0
    for path in sorted(folder.rglob('*.kf')):
        data = read(path)
        sequence = data.roots[0]
        instant_transitions(sequence)
        visibility = [link for link in sequence.controlled_blocks
                      if link.node_name in GATES and link.controller_type == b'NiVisController']
        assert len(visibility) == 4
        states = [(link.node_name, bool(link.interpolator.bool_value), link.priority) for link in visibility]
        assert sum(v for _, v, _ in states) == 1
        start = sequence.num_controlled_blocks
        sequence.num_controlled_blocks += 4
        sequence.controlled_blocks.update_size()
        for index, (name, visible, priority) in enumerate(states, start):
            link = sequence.controlled_blocks[index]
            link.node_name = name
            link.controller_type = b'NiTransformController'
            link.priority = priority
            pose = N.NiTransformInterpolator()
            pose.scale = 1.0 if visible else 0.0
            link.interpolator = pose
        checked = write(path, data).roots[0]
        assert [item.interpolator.scale for item in checked.controlled_blocks[-4:]] == [float(v) for _, v, _ in states]
        count += 1
    return {'method': 'managed zero-scale transforms plus original visibility', 'sequences': count,
            'transition': 'vanilla Blend: 0; constant scale poses',
            'status': 'M6 single-sprite VATS user PASS; zero-duration transition gameplay pending'}
