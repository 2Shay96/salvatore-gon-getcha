"""Fixed-size, alpha-tested clip selection; alternative to failed scale gates.

This native material-controller route still needs FNV/VATS acceptance. No
geometry transform is animated for selection. Alpha blending is disabled so
intermediate material alpha cannot produce a translucent crossfade.
"""
from salvatore_shoe_assets import N, GATES, read, write


def apply(out, *, alpha_only=False):
    folder = out / 'meshes/creatures/salvatoreganacci'
    skeleton = read(folder / 'skeleton.nif')
    targets = {}
    for gate in skeleton.roots[0].children:
        if gate is None or gate.name not in GATES:
            continue
        assert gate.scale == 1.0
        assert not any(isinstance(c, N.NiTransformController) for c in gate.get_controllers())
        shapes = [b for b in gate.tree() if isinstance(b, N.NiTriShape)]
        assert len(shapes) == 1
        shape = shapes[0]
        targets[gate.name] = shape.name
        material = next(p for p in shape.properties if isinstance(p, N.NiMaterialProperty))
        alpha = next(p for p in shape.properties if isinstance(p, N.NiAlphaProperty))
        shader = next(p for p in shape.properties if isinstance(p, N.BSShaderNoLightingProperty))
        alpha.flags = int(alpha.flags) & ~1  # Disable blending; retain GREATER alpha test.
        alpha.threshold = 128
        # Without this flag the engine may use transparency multisampling
        # instead of the explicit alpha-test threshold. PyFFI misspells it.
        shader.shader_flags_2.sf_2_no_transparecny_multisampling = 1
        assert int(shader.shader_flags_2) & (1 << 22)
        material.alpha = float(gate.name == GATES[0])
        controller = N.NiAlphaController()
        controller.flags = 108
        controller.frequency = 1.0
        controller.start_time = 0.0
        controller.stop_time = 6.666667
        blend = N.NiBlendFloatInterpolator()
        blend.unknown_short = 515  # manager controlled, highest weight only, capacity2
        blend.unknown_int = 0  # weight threshold0
        blend.float_value = -3.402823466e38  # invalid until supplied by manager
        controller.interpolator = blend
        material.add_controller(controller)
        if alpha_only:
            gate.flags = int(gate.flags) & ~1
    assert set(targets) == set(GATES)
    write(folder / 'skeleton.nif', skeleton)
    count = 0
    for path in sorted(folder.rglob('*.kf')):
        data = read(path)
        sequence = data.roots[0]
        visibility = [link for link in sequence.controlled_blocks
                      if link.node_name in GATES and link.controller_type == b'NiVisController']
        assert len(visibility) == 4
        assert not any(link.node_name in GATES and link.controller_type == b'NiTransformController'
                       for link in sequence.controlled_blocks)
        states = [(link.node_name, bool(link.interpolator.bool_value), link.priority) for link in visibility]
        assert sum(v for _, v, _ in states) == 1
        if alpha_only:
            # One selector owns visibility. Independent bool/float switches can
            # disagree during idle/movement transitions and hide every clip.
            for link in visibility:
                link.interpolator.bool_value = True
        start = sequence.num_controlled_blocks
        sequence.num_controlled_blocks += 4
        sequence.controlled_blocks.update_size()
        for index, (name, visible, priority) in enumerate(states, start):
            link = sequence.controlled_blocks[index]
            link.node_name = targets[name]
            link.property_type = b'NiMaterialProperty'
            link.controller_type = b'NiAlphaController'
            link.priority = priority
            pose = N.NiFloatInterpolator()
            pose.float_value = float(visible)
            link.interpolator = pose
        checked = write(path, data).roots[0]
        assert [link.interpolator.float_value for link in checked.controlled_blocks[-4:]] == [float(v) for _, v, _ in states]
        count += 1
    return {'method': 'fixed-size alpha-tested material gates' +
                      ('; bool gates always visible' if alpha_only else ' plus original visibility'),
            'alphaOnlySelection': alpha_only,
            'sequences': count, 'alphaThreshold': 128, 'blending': False,
            'status': 'offline verified; normal and VATS rendering acceptance pending'}
