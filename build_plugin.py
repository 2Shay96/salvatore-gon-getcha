"""Compatibility entry point; implementation lives in the public GECK bridge."""
from workshop_bridge import dispatch, export_module

if __name__ == "__main__":
    dispatch('build_plugin.py')
else:
    globals().update(vars(export_module('build_plugin.py', __name__)))
