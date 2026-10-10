"""Compatibility entry point; implementation lives in the public GECK bridge."""
from workshop_bridge import dispatch, export_module

if __name__ == "__main__":
    dispatch('install_plugin.py')
else:
    globals().update(vars(export_module('install_plugin.py', __name__)))
