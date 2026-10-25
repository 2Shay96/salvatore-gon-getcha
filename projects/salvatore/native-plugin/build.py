"""Reproducible x86 DLL + host-side unit checks; no game installation."""
import argparse, hashlib, json, pathlib, subprocess

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--zig', type=pathlib.Path, required=True)
args = parser.parse_args()
zig = args.zig.resolve(strict=True)
version = subprocess.check_output([str(zig), 'version'], text=True).strip()
if version != '0.14.1':
    raise SystemExit('Use the pinned Zig 0.14.1 compiler')
out = ROOT / 'build/salvatore-native/candidate1'
out.mkdir(parents=True, exist_ok=True)
commands = []
base = [str(zig), 'c++', '-target', 'x86-windows-gnu', '-std=c++17', '-O2',
        '-Wall', '-Wextra', '-Werror', '-nostdlib++']
for name, flags in [('SalvatoreGanacci.dll', ['-shared']),
                    ('unit-tests.exe', ['-DSG_UNIT_TEST'])]:
    cmd = base + flags + [str(HERE/'SalvatoreGanacci.cpp'), '-o', str(out/name)]
    commands.append(cmd)
    subprocess.run(cmd, check=True)
result = subprocess.check_output([str(out/'unit-tests.exe')], text=True)
print(result)
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
receipt = {'compiler': version, 'commands': commands, 'host_tests': result.strip(),
           'runtime_tested': False, 'mode': 'deployment candidate1',
           'sources': {p.name:sha(p) for p in [HERE/'SalvatoreGanacci.cpp', HERE/'nvse_abi.h', HERE/'build.py']},
           'dll_sha256': sha(out/'SalvatoreGanacci.dll')}
(out/'build.json').write_text(json.dumps(receipt, indent=2))
print(json.dumps(receipt, indent=2))
