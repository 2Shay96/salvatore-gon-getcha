"""Locate and delegate to the public GECK bridge; never use an archived local core."""
import ast
import importlib.util
import os
from pathlib import Path
import runpy
import sys

WORKSHOP = Path(__file__).resolve().parent


def bridge_root():
    override = os.environ.get('GECK_BRIDGE_ROOT')
    if override:
        candidates = [Path(override).expanduser().resolve()]
    else:
        candidates = [WORKSHOP.parent / 'geck-mcp-release' / 'GECK-MCP-for-New-Vegas' / 'bridge']
        # Managed/workspace worktrees live outside fnv; still use the one public checkout.
        candidates += [parent / 'fnv' / 'geck-mcp-release' / 'GECK-MCP-for-New-Vegas' / 'bridge'
                       for parent in WORKSHOP.parents]
    for candidate in candidates:
        if (candidate / 'geck_mcp' / 'service.py').is_file() and (candidate / 'recover.py').is_file():
            tree = ast.parse((candidate / 'geck_mcp' / '__init__.py').read_text(encoding='utf-8'))
            version = next((ast.literal_eval(node.value) for node in tree.body
                            if isinstance(node, ast.Assign) and
                            any(isinstance(t, ast.Name) and t.id == '__version__' for t in node.targets)), '0.0.0')
            if tuple(int(part) for part in version.split('.')) < (0, 2, 1):
                raise RuntimeError('Public GECK bridge 0.2.1 or newer is required (found %s). '
                                   'Update the public checkout first; the workshop requires static collision support.' % version)
            return candidate.resolve()
    raise RuntimeError('Public GECK bridge not found. Clone 2Shay96/geck-mcp-nv beside the workshop '
                       'at ../geck-mcp-release/GECK-MCP-for-New-Vegas, or set GECK_BRIDGE_ROOT '
                       'to its bridge folder. Archived workshop code is never used.')


def activate():
    root = bridge_root()
    if str(root) in sys.path:
        sys.path.remove(str(root))
    sys.path.insert(0, str(root))
    return root


def command_args(script, args):
    """Preserve workshop output defaults, but require an explicit editor profile."""
    args = list(args)
    help_requested = any(arg in ('--help', '-h') for arg in args)
    has = lambda flag: any(arg == flag or arg.startswith(flag + '=') for arg in args)
    if script == 'build_plugin.py' and not has('--project') and not has('--state'):
        args += ['--state', str(WORKSHOP / 'state')]
    if script == 'esm_index.py' and not has('--out'):
        args += ['--out', str(WORKSHOP / 'state' / 'index')]
    profile_flag = '--project-config' if script == 'install_plugin.py' else '--project'
    if script in ('install_plugin.py', 'build_helper.py', 'photo_cli.py', 'run_mcp.py'):
        if not help_requested and not has(profile_flag):
            raise ValueError('Pass %s with a workshop profile, for example projects/salvatore_mod.windows.json. '
                             'No public example profile will be used by default.' % profile_flag)
    return args


def dispatch(script):
    root = activate()
    target = root / script
    if not target.is_file():
        raise RuntimeError('Required public bridge command is missing: %s' % target)
    try:
        args = command_args(script, sys.argv[1:])
    except ValueError as error:
        raise SystemExit(str(error)) from error
    sys.argv = [str(target), *args]
    runpy.run_path(str(target), run_name='__main__')


def export_module(script, name):
    """Preserve imports such as sprite_flipbook and bridge_lock without a copied implementation."""
    target = activate() / script
    spec = importlib.util.spec_from_file_location(name, target)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module
