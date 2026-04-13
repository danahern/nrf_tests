"""Make ``assistant_bridge`` importable when pytest is run from host/."""
from __future__ import annotations

import sys
from pathlib import Path

# host/ contains the assistant_bridge package. Prepend host/ to sys.path so
# ``import assistant_bridge`` works regardless of invocation cwd.
HERE = Path(__file__).resolve().parent
HOST_DIR = HERE.parent
if str(HOST_DIR) not in sys.path:
    sys.path.insert(0, str(HOST_DIR))
