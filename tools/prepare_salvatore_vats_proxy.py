"""Salvatore v1.2 candidate3: hidden lit VATS proxy under the torso Part Node (offline; never installs).

FNV's VATS limb occlusion query (VATSEffect 0x800F30, render mode 8) cannot count Salvatore's sprites:
  - before drawing it AppCulls every direct child of the target root that holds no skinned geometry
    (0x801DA0 / 0x4910D0), which includes all four clip gates; 0x8022C0 later un-culls them again;
  - mode 8 only draws BSSM_CONSTALPHA passes, which lit shaders create and NoLighting sprites never do.
It then draws the torso Part Node (BPNN) subtree on its own (0x801450), starting below those culled children.
So this tool:
  - adds NiBillboardNode SalvatoreVatsProxyBillboard (AppCulled) with one lit two-faced slab under
    'Bip01 NonAccum' (not a direct root child, so the engine's cull/un-cull loop never touches it);
  - changes only the torso BPTD BPNN from 'Scene Root' to 'Bip01 NonAccum' (still the ancestor of every
    ragdoll bone). The v1.2 DLL un-culls the billboard only during the two query passes.
Details: projects/salvatore/handoff/V1.2-VATS-RUNTIME-TRACE-20261027.md.
"""
import hashlib
import io
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import zipfile

# pyffi orders the NIF string table by set iteration, which follows Python's randomised hashing.
if __name__ == '__main__' and os.environ.get('PYTHONHASHSEED') != '0':
    sys.exit(subprocess.call([sys.executable, *sys.argv], env={**os.environ, 'PYTHONHASHSEED': '0'}))

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))
from workshop_bridge import activate  # noqa: E402
activate()
from geck_mcp.esp import codec  # noqa: E402
from geck_mcp.esp.validate import validate  # noqa: E402
from salvatore_shoe_assets import N, GATES  # noqa: E402  (also sets the pyffi time.clock shim)

RELEASE = ROOT / 'build/release/SalvatoreGanacci-v1.1.zip'
RELEASE_SHA = '35e33789e7f3f625d487a9e29674eab2b2cf31c5764cbc87aa4c5e77d6c7f5ec'
SKELETON = 'meshes/creatures/salvatoreganacci/skeleton.nif'
SKELETON_SHA = '8099e95b2398c2538ecfb8c975b0b83583cc3d7f9d3c5fc306d05fde961b5e45'
ESP = 'SalvatoreGanacci.esp'
ESP_SHA = '1dee0949afa531747ea2469d570a837d9342de7b957f132dc01c6c69c7c0126e'
OUT = ROOT / 'build/salvatore-vats-proxy3'
PART_NODE = b'Bip01 NonAccum'
BILLBOARD = b'SalvatoreVatsProxyBillboard'
PROXY = b'SalvatoreVatsProxy'
# Body-sized slab relative to 'Bip01 NonAccum' (bind pose 60.2 above the root): world z ~5..190,
# two faces 16 units apart so one is always in front of the sprite plane whichever way the billboard faces.
X_MIN, X_MAX, Z_MIN, Z_MAX, HALF_DEPTH = -33.0, 23.0, -55.0, 130.0, 8.0
# Vanilla textures (meshes/clutter/ashtray/ashtray01.nif): always present, never seen on the culled proxy.
TEXTURES = [b'textures\\clutter\\ashtray\\ashtray01.dds', b'textures\\clutter\\ashtray\\ashtray01_n.dds', b'', b'', b'', b'']


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def dump(block):
    return re.sub(r' at 0x[0-9A-Fa-f]+', '', str(block))


def encode(data):
    stream = io.BytesIO()
    data.write(stream)
    return stream.getvalue()


def slab():
    data = N.NiTriShapeData()
    corners = [(X_MIN, Z_MIN), (X_MAX, Z_MIN), (X_MAX, Z_MAX), (X_MIN, Z_MAX)]
    uvs = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
    faces = [-HALF_DEPTH, HALF_DEPTH]
    data.num_vertices = 8
    data.has_vertices = True
    data.vertices.update_size()
    data.has_normals = True
    data.normals.update_size()
    data.num_uv_sets = 1
    data.has_uv = True
    data.uv_sets.update_size()
    for f, y in enumerate(faces):
        for c, ((x, z), (u, v)) in enumerate(zip(corners, uvs)):
            i = 4 * f + c
            data.vertices[i].x, data.vertices[i].y, data.vertices[i].z = x, y, z
            data.normals[i].x, data.normals[i].y, data.normals[i].z = 0.0, -1.0 if y < 0 else 1.0, 0.0
            data.uv_sets[0][i].u, data.uv_sets[0][i].v = u, v
    triangles = []
    for b in (0, 4):  # each face double-sided, like the sprites
        triangles += [(b, b + 1, b + 2), (b, b + 2, b + 3), (b + 2, b + 1, b), (b + 3, b + 2, b)]
    data.set_triangles(triangles)
    data.update_center_radius()

    shape = N.NiTriShape()
    shape.name = PROXY
    shape.flags = 14  # as the sprites (selective update + transforms), not culled itself
    shape.data = data
    material = N.NiMaterialProperty()
    material.glossiness = 80.0
    material.alpha = 1.0
    material.emit_multi = 1.0
    textures = N.BSShaderTextureSet()
    textures.num_textures = len(TEXTURES)
    textures.textures.update_size()
    for i, path in enumerate(TEXTURES):
        textures.textures[i] = path
    lit = N.BSShaderPPLightingProperty()
    lit.shader_type = 1                       # SHADER_DEFAULT, as vanilla rigid clutter
    lit.shader_flags.sf_z_buffer_test = 1     # vanilla 0x80000001 minus specular
    lit.shader_flags_2.sf_2_z_buffer_write = 1
    lit.environment_map_scale = 1.0
    lit.texture_clamp_mode = 3
    lit.texture_set = textures
    shape.num_properties = 2
    shape.properties.update_size()
    shape.properties[0] = material
    shape.properties[1] = lit
    shape.update_tangent_space(as_extra=False)
    return shape


def build_skeleton(raw):
    data = N.Data()
    data.read(io.BytesIO(raw))
    reread = N.Data()
    reread.read(io.BytesIO(encode(data)))
    assert [dump(b) for b in data.blocks] == [dump(b) for b in reread.blocks], 'pyffi round-trip changes content'
    before = {id(b): dump(b) for b in data.blocks}
    root = data.roots[0]
    nonaccum = next(b for b in data.blocks if isinstance(b, N.NiNode) and b.name == PART_NODE)
    assert nonaccum not in root.children, 'part node must not be a direct root child'
    sprite_billboard = next(b for b in data.blocks if isinstance(b, N.NiBillboardNode))
    billboard = N.NiBillboardNode()
    billboard.name = BILLBOARD
    billboard.flags = sprite_billboard.flags | 1  # AppCulled except while the DLL runs a VATS query pass
    billboard.billboard_mode = sprite_billboard.billboard_mode
    billboard.add_child(slab())
    nonaccum.add_child(billboard)
    out = encode(data)

    check = N.Data()
    check.read(io.BytesIO(out))
    blocks = list(check.blocks)
    named = {b.name: b for b in blocks if isinstance(b, N.NiAVObject)}
    assert named[BILLBOARD].flags & 1 and not named[PROXY].flags & 1
    assert named[BILLBOARD] in named[PART_NODE].children and named[PROXY] in named[BILLBOARD].children
    assert any(isinstance(p, N.BSShaderPPLightingProperty) for p in named[PROXY].properties)
    sprites = [b for b in blocks if isinstance(b, N.NiTriShape) and b.name != PROXY]
    assert len(sprites) == 4 and all(not s.skin_instance for s in sprites)
    # Only the part node (gained one child) and its ancestors (their dumps include children) may differ.
    differing = [b.name.decode() for b in data.blocks if id(b) in before and dump(b) != before[id(b)]]
    ancestors = {PART_NODE.decode(), 'Bip01', root.name.decode()}
    assert set(differing) <= ancestors, differing
    return out, {'partNode': PART_NODE.decode(), 'billboard': BILLBOARD.decode(), 'billboardFlags': named[BILLBOARD].flags,
                 'billboardMode': int(named[BILLBOARD].billboard_mode), 'proxy': PROXY.decode(),
                 'proxyVertices': named[PROXY].data.num_vertices, 'proxyTriangles': named[PROXY].data.num_triangles,
                 'slab': {'x': [X_MIN, X_MAX], 'z': [Z_MIN, Z_MAX], 'halfDepth': HALF_DEPTH},
                 'blocks': {'before': len(before), 'after': len(blocks)},
                 'originalBlocksDifferingInDump': differing, 'otherOriginalBlocksIdentical': len(before) - len(differing)}


def build_esp(raw):
    old = codec.parse(raw)
    plugin = codec.parse(raw)
    body = next(r for r, _ in plugin.records() if r.editor_id == 'SalvatoreGanacciTorsoBodyPartData')
    assert [s.data for s in body.subrecords if s.tag == 'BPNN'] == [b'Scene Root\0']
    body.subrecords = [codec.Subrecord('BPNN', PART_NODE + b'\0') if s.tag == 'BPNN' else s for s in body.subrecords]
    out = codec.write(plugin)
    before = {r.form_id: codec.encode_record(r) for r, _ in old.records()}
    after = {r.form_id: codec.encode_record(r) for r, _ in codec.parse(out).records()}
    assert before.keys() == after.keys()
    changed = [f'{k:08X}' for k in before if before[k] != after[k]]
    assert changed == [f'{body.form_id:08X}'], changed
    check = validate(out, masters_expected=['FalloutNV.esm'])
    assert check['ok'], check
    return out, {'changedRecords': changed, 'bpnn': PART_NODE.decode(), 'validate': 'ok'}


def main():
    raw_zip = RELEASE.read_bytes()
    assert sha(raw_zip) == RELEASE_SHA, 'released v1.1 archive changed'
    release = zipfile.ZipFile(io.BytesIO(raw_zip))
    skeleton_raw, esp_raw = release.read(SKELETON), release.read(ESP)
    assert sha(skeleton_raw) == SKELETON_SHA and sha(esp_raw) == ESP_SHA
    skeleton, skeleton_report = build_skeleton(skeleton_raw)
    esp, esp_report = build_esp(esp_raw)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / 'skeleton.nif').write_bytes(skeleton)
    (OUT / ESP).write_bytes(esp)
    receipt = {'input': {'zip': str(RELEASE), 'zipSha256': RELEASE_SHA, 'skeletonSha256': SKELETON_SHA, 'espSha256': ESP_SHA},
               'skeleton': str(OUT / 'skeleton.nif'), 'skeletonSha256': sha(skeleton), 'skeletonReport': skeleton_report,
               'esp': str(OUT / ESP), 'espSha256': sha(esp), 'espReport': esp_report,
               'releasedSkeletonRoundTrip': 'content-identical (string table reordered by pyffi)',
               'runtime': 'UNTESTED; billboard is AppCulled and needs the v1.2 DLL toggle'}
    (OUT / 'build.json').write_text(json.dumps(receipt, indent=2))
    print(json.dumps(receipt, indent=2))


if __name__ == '__main__':
    main()
