"""Compatibility entry point; implementation lives in the public GECK bridge."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from workshop_bridge import dispatch, export_module

if __name__ == "__main__":
    dispatch('tools/creature_skeleton.py')
else:
    globals().update(vars(export_module('tools/creature_skeleton.py', __name__)))
