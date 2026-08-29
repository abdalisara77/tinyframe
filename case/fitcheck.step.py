"""Fit check: the two printed parts around Elecrow's own model of the board.

This is a validation fixture, not a deliverable. It places the real CrowPanel
assembly in the frame's coordinate system so `inspect interfere` can answer the
only question that matters before printing: does anything on the board collide
with anything in the case?

The board STEP is Elecrow's, and is not committed -- see case/README.md for the
one command that fetches it.

Elecrow model -> frame coordinates is a +90 deg rotation about X (their Y is the
board normal, ours is the screen's "up") plus a 0.5 mm shift that puts the
origin on the PCB front face.
"""

import importlib.util
import sys
from pathlib import Path

from build123d import *

HERE = Path(__file__).parent
BOARD_STEP = HERE / "reference" / "crowpanel_42.stp"


def _load(name):
    # The CLI restores sys.path after loading this module, so the children's
    # own `import crowpanel` needs this directory put back before they execute.
    if str(HERE) not in sys.path:
        sys.path.insert(0, str(HERE))
    path = HERE / name
    spec = importlib.util.spec_from_file_location(path.stem, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def gen_step():
    front = _load("tinyframe_front.step.py").gen_step()
    rear = _load("tinyframe_rear.step.py").gen_step()

    board = import_step(str(BOARD_STEP))

    # Elecrow's model ships the board wearing their acrylic case: three cover
    # plates and the six M2.5 standoffs that hold them. That hardware comes off
    # before this case goes on, so it is dropped here -- left in, it clashes
    # with our own walls and standoffs and drowns the real result.
    kit = {"42-TOP_COVER", "42-MIDDLE-COVER", "42-BOTTOM-COVER", "M2_5X3_5"}
    bare = [child for child in board.children if child.label not in kit]

    board = Pos(0, 0, 0.5) * Rotation(90, 0, 0) * Compound(children=bare)
    board.label = "crowpanel_board"

    assembly = Compound(children=[front, rear, board])
    assembly.label = "tinyframe_fitcheck"
    return assembly
