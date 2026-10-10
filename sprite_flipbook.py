"""Compatibility entry point; implementation lives in the public GECK bridge."""
from workshop_bridge import dispatch, export_module

if __name__ == "__main__":
    dispatch('sprite_flipbook.py')
else:
    globals().update(vars(export_module('sprite_flipbook.py', __name__)))
