"""Compatibility entry point; implementation lives in the public GECK bridge."""
from workshop_bridge import dispatch, export_module

if __name__ == "__main__":
    dispatch('compile_scripts.py')
else:
    globals().update(vars(export_module('compile_scripts.py', __name__)))
