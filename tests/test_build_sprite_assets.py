"""tools/build_sprite_assets.py: input checks, tool command lines and the install step (no PyFFI needed)."""
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import wave
import zlib

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import build_sprite_assets as bsa  # noqa: E402


def png(path, w, h):
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    raw = b''.join(b'\0' + b'\0\0\0\0' * w for _ in range(h))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def wav(path, rate=44100, width=2, channels=1, trailing=b''):
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(width)
        w.setframerate(rate)
        w.writeframes(b'\0' * width * channels * 100)
    if trailing:
        path.write_bytes(path.read_bytes() + trailing)


class Fixture:
    def __init__(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / 'S').mkdir()
        for i in range(3):
            png(self.dir / 'S' / ('%04d.png' % i), 8, 12)
        wav(self.dir / 'music.wav')
        (self.dir / 'icon.dds').write_bytes(b'DDS ' + b'\0' * 124)
        self.spec = {
            'schema': 1, 'kind': 'sprite_character_assets',
            'character': {'id': 'test_guy', 'displayName': 'Test Guy', 'editorPrefix': 'TestGuy'},
            'clips': {'dance': {'frames': 'S', 'fps': 24}},
            'stateClips': {'hostile': 'dance', 'defeated': 'dance'},
            'sprites': {
                'body': {'state': 'hostile', 'name': 'creatures/testguy/body', 'frameUnits': 195, 'prn': 'Scene Root'},
                'down': {'state': 'defeated', 'name': 'testguy/down', 'frameUnits': 42, 'reuseTextureOf': 'body',
                         'collision': {'template': 'meshes\\clutter\\ashtray\\ashtray01.nif', 'box': [10, 10, 8],
                                       'material': 'organic'}}},
            'skeleton': {'source': 'creatures\\mistergutsy', 'name': 'TestGuy',
                         'replaceSound': {'NPCRobotMrHandyAttackSaw': 'FXSwingMedium'}, 'hitSound': 'WPNPowerFistFire3D'},
            'audio': {'music_loop': {'file': 'music.wav', 'install': 'sound/fx/testguy/music.wav'}},
            'icon': {'file': 'icon.dds', 'install': 'textures/interface/icons/pipboyimages/weapons/testguy.dds'}}

    def write(self, **changes):
        spec = json.loads(json.dumps(self.spec))
        for dotted, value in changes.items():
            node, keys = spec, dotted.split('.')
            for k in keys[:-1]:
                node = node[k]
            node[keys[-1]] = value
        path = self.dir / 'assets.json'
        path.write_text(json.dumps(spec))
        return path


class LoadTests(unittest.TestCase):
    def setUp(self):
        self.fx = Fixture()

    def tearDown(self):
        self.fx.tmp.cleanup()

    def test_good_spec(self):
        plan = bsa.load(self.fx.write())
        self.assertEqual(plan['clips']['dance']['count'], 3)
        self.assertEqual(plan['clips']['dance']['size'], [8, 12])
        self.assertEqual(plan['audio']['music_loop']['info']['rate'], 44100)
        self.assertEqual(plan['sprites']['down']['clip'], 'dance')

    def test_rejects(self):
        cases = {
            'clips.dance.view': 'N',
            'clips.dance.fps': 23.976,
            'stateClips.dancing': 'dance',
            'sprites.down.reuseTextureOf': 'nope',
            'sprites.body.name': 'Creatures/Body',
            'character.id': 'Test Guy',
            'audio.bad_slot': {'file': 'music.wav', 'install': 'sound/x.wav'},
            'audio.music_loop.install': 'music.wav',
            'sprites.down.collision.material': 'soft glass!',
            'sprites.down.collision.static': 'yes',
        }
        for dotted, value in cases.items():
            with self.subTest(dotted):
                with self.assertRaises(bsa.AssetError):
                    bsa.load(self.fx.write(**{dotted: value}))

    def test_frame_sizes_must_match(self):
        png(self.fx.dir / 'S' / '0003.png', 9, 12)
        with self.assertRaisesRegex(bsa.AssetError, 'differ in size'):
            bsa.load(self.fx.write())

    def test_wav_checks(self):
        wav(self.fx.dir / 'music.wav', width=1)
        with self.assertRaisesRegex(bsa.AssetError, '16-bit'):
            bsa.load(self.fx.write())
        wav(self.fx.dir / 'music.wav', trailing=b'c2pa' + struct.pack('<I', 4) + b'abcd')
        self.assertEqual(bsa.load(self.fx.write())['audio']['music_loop']['info']['trailing'], ['c2pa'])


class CommandTests(unittest.TestCase):
    def test_skeleton_then_sprites_with_proven_flags(self):
        fx = Fixture()
        try:
            plan = bsa.load(fx.write())
            cmds = bsa.commands(plan, Path('out'), Path('Data'), Path('build'), python='py')
        finally:
            fx.tmp.cleanup()
        skeleton, body, down = cmds
        self.assertTrue(skeleton[1].endswith('creature_skeleton.py'))
        self.assertIn('NPCRobotMrHandyAttackSaw=FXSwingMedium', skeleton)
        self.assertEqual(skeleton[skeleton.index('--hit-sound') + 1], 'WPNPowerFistFire3D')
        self.assertEqual(body[body.index('--frame-units') + 1], '195')
        self.assertEqual(body[body.index('--prn') + 1], 'Scene Root')
        self.assertEqual(body[body.index('--offset-z') + 1], '0')
        self.assertNotIn('--collision-template', body)
        self.assertEqual(down[down.index('--reuse-texture') + 1], 'creatures/testguy/body')
        self.assertEqual(down[down.index('--collision-box') + 1], '10,10,8')
        self.assertEqual(down[down.index('--collision-material') + 1], 'organic')
        self.assertNotIn('--collision-material', body)
        self.assertNotIn('--collision-static', down)
        self.assertTrue(down[down.index('--collision-template') + 1].replace('\\', '/').endswith(
            'build/_vanilla/meshes/clutter/ashtray/ashtray01.nif'))


    def test_static_collision_flag(self):
        fx = Fixture()
        try:
            plan = bsa.load(fx.write(**{'sprites.down.collision.static': True}))
            down = bsa.commands(plan, Path('out'), Path('Data'), Path('build'), python='py')[2]
        finally:
            fx.tmp.cleanup()
        self.assertIn('--collision-static', down)


class InstallTests(unittest.TestCase):
    def test_backs_up_different_files_and_skips_identical(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            out, data, state = tmp / 'out', tmp / 'Data', tmp / 'state'
            for rel, body in (('meshes/a/x.nif', b'new'), ('sound/fx/a/y.wav', b'same')):
                (out / rel).parent.mkdir(parents=True, exist_ok=True)
                (out / rel).write_bytes(body)
            (data / 'meshes/a').mkdir(parents=True)
            (data / 'meshes/a/x.nif').write_bytes(b'old')
            (data / 'sound/fx/a').mkdir(parents=True)
            (data / 'sound/fx/a/y.wav').write_bytes(b'same')
            files = {p.relative_to(out).as_posix(): {'sha256': bsa.sha(p)} for p in bsa.installable(out)}
            done = bsa.install(out, {'character': {'id': 't'}, 'files': files}, data, state, log=lambda *_: None)
            self.assertEqual(done['installed'], ['meshes/a/x.nif'])
            self.assertEqual(done['unchanged'], ['sound/fx/a/y.wav'])
            self.assertEqual((data / 'meshes/a/x.nif').read_bytes(), b'new')
            self.assertEqual((Path(done['backupFolder']) / 'meshes/a/x.nif').read_bytes(), b'old')
            self.assertEqual(len(json.loads((state / 'installs' / 't-assets.json').read_text())), 1)


if __name__ == '__main__':
    unittest.main()
