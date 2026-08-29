"""TinyFrame rear shell -- the back plate that closes the frame.

A flat plate that drops into the back of the front frame, with four standoffs
that reach forward to the PCB. Each standoff is a screw channel: an M2.5 x 12
enters from the back, passes the plate, the standoff and the PCB, and threads
into the matching boss in the front frame, so one screw clamps the whole stack
and nothing is visible from the front.

Origin and orientation are crowpanel.py's: origin at the centre of the PCB
front face, +Z out of the screen. Print it outside-face down; the standoffs
grow upward and need no support.
"""

from build123d import *

import crowpanel as cp


def gen_step():
    # Back plate, sized to sit inside the front frame's walls.
    profile = fillet(
        Rectangle(cp.BACK_PLATE_W, cp.BACK_PLATE_H).vertices(), cp.BACK_PLATE_R
    )
    part = extrude(Plane.XY.offset(cp.BACK_OUTER_Z) * profile, amount=cp.BACK_T)

    # Standoffs, reaching from the plate to the back face of the PCB.
    standoff_h = cp.PCB_BACK_Z - cp.CAVITY_FLOOR_Z
    for x, y in cp.MOUNT_XY:
        part += Pos(x, y, cp.CAVITY_FLOOR_Z + standoff_h / 2) * Cylinder(
            cp.BOSS_D / 2, standoff_h
        )

    # Screw channels: clearance the whole way, counterbored from the back so
    # the heads sit flush with the outside face.
    for x, y in cp.MOUNT_XY:
        part -= Pos(x, y, (cp.BACK_OUTER_Z - 1.0 + cp.PCB_BACK_Z) / 2) * Cylinder(
            cp.SCREW_CLEAR_D / 2, cp.PCB_BACK_Z - cp.BACK_OUTER_Z + 1.0
        )
        part -= Pos(
            x, y, cp.BACK_OUTER_Z + cp.SCREW_HEAD_DEPTH / 2 - 0.5
        ) * Cylinder(cp.SCREW_HEAD_D / 2, cp.SCREW_HEAD_DEPTH + 1.0)

    # Access holes for the RST/BOOT switches on the back of the board.
    for x, y in cp.REAR_BUTTON_XY:
        part -= Pos(x, y, cp.BACK_OUTER_Z + cp.BACK_T / 2) * Cylinder(
            cp.REAR_BUTTON_D / 2, cp.BACK_T + 2.0
        )

    # Optional opening over the 2x10 GPIO header. Off by default.
    if cp.HEADER_OPENING:
        part -= Pos(
            (cp.HEADER_X0 + cp.HEADER_X1) / 2,
            (cp.HEADER_Y0 + cp.HEADER_Y1) / 2,
            cp.BACK_OUTER_Z + cp.BACK_T / 2,
        ) * Box(
            cp.HEADER_X1 - cp.HEADER_X0,
            cp.HEADER_Y1 - cp.HEADER_Y0,
            cp.BACK_T + 2.0,
        )

    part.label = "tinyframe_rear_shell"
    return part
