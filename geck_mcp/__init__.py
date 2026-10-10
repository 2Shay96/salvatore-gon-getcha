"""Compatibility import: all GECK core modules come from the public bridge."""
from workshop_bridge import activate
from pathlib import Path
_public = activate() / 'geck_mcp'
__path__ = [str(_public)]
__file__ = str(_public / '__init__.py')
if __spec__ is not None:
    __spec__.submodule_search_locations = __path__
exec(compile(Path(__file__).read_text(encoding='utf-8'), __file__, 'exec'))
