"""TinyFrame front frame -- the visible picture frame.

A shallow tray: a face plate with the display window, full-depth side walls
carrying every edge opening, and four bosses the PCB lands on. The board drops
in from the back, the rear shell closes it, and four M2.5 screws pull the whole
stack together.

Origin and orientation are crowpanel.py's: origin at the centre of the PCB
front face, +Z out of the screen.

Print it face down on the bed. The window is a through hole, so nothing
overhangs; the walls and bosses grow upward off the flat front face, which is
also the surface that wants the best finish.
"""

from build123d import *

import crowpanel as cp


def _rounded_slab(width, height, radius, z0, z1):
    """A rounded rectangle from z0 to z1, centred on the origin in X and Y."""
    profile = Rectangle(width, height)
    if radius > 0:
        profile = fillet(profile.vertices(), radius)
    return extrude(Plane.XY.offset(z0) * profile, amount=z1 - z0)


def _edge_cut(axis, along, z0, z1, through):
    """A box that cuts an opening in one wall.

    `axis` is the wall's outward direction ("x" or "y"); `along` is the
    opening's span across that wall's face; `through` spans the wall itself,
    overshooting both faces so the boolean never meets a coplanar face.
    """
    span = (along[1] - along[0], through[1] - through[0], z1 - z0)
    mid = (
        (along[0] + along[1]) / 2,
        (through[0] + through[1]) / 2,
        (z0 + z1) / 2,
    )
    if axis == "x":
        span = (span[1], span[0], span[2])
        mid = (mid[1], mid[0], mid[2])
    return Pos(*mid) * Box(*span)


def gen_step():
    # Base block, front face down to the back edge of the walls.
    part = _rounded_slab(
        cp.CASE_W, cp.CASE_H, cp.CASE_R, cp.BACK_OUTER_Z, cp.FACE_OUTER_Z
    )

    # Hollow it out: everything behind the face plate becomes the cavity.
    part -= _rounded_slab(
        cp.CAVITY_W,
        cp.CAVITY_H,
        cp.CAVITY_R,
        cp.BACK_OUTER_Z - 1.0,
        cp.FACE_INNER_Z,
    )

    # The display window through the face plate.
    part -= _rounded_slab(
        cp.WINDOW_W,
        cp.WINDOW_H,
        cp.WINDOW_R,
        cp.FACE_INNER_Z - 1.0,
        cp.FACE_OUTER_Z + 1.0,
    )

    # Bosses: the PCB front face lands on these, and the screws thread into them.
    for x, y in cp.MOUNT_XY:
        part += Pos(x, y, cp.FACE_INNER_Z / 2) * Cylinder(
            cp.BOSS_D / 2, cp.FACE_INNER_Z
        )

    # Pilot holes run on into the plate, stopping short of the front face.
    for x, y in cp.MOUNT_XY:
        part -= Pos(x, y, cp.BOSS_PILOT_DEPTH / 2 - 1.0) * Cylinder(
            cp.BOSS_PILOT_D / 2, cp.BOSS_PILOT_DEPTH + 2.0
        )

    # --- Edge openings ----------------------------------------------------
    left = (-cp.CASE_W / 2 - 1.5, -cp.CAVITY_W / 2 + 1.5)
    right = (cp.CAVITY_W / 2 - 1.5, cp.CASE_W / 2 + 1.5)
    bottom = (-cp.CASE_H / 2 - 1.5, -cp.CAVITY_H / 2 + 1.5)

    # USB-C and the battery connector, left edge.
    part -= _edge_cut(
        "x",
        along=(cp.USB_C_Y - cp.USB_C_OPEN_W / 2, cp.USB_C_Y + cp.USB_C_OPEN_W / 2),
        z0=cp.USB_C_Z - cp.USB_C_OPEN_H / 2,
        z1=cp.USB_C_Z + cp.USB_C_OPEN_H / 2,
        through=left,
    )
    part -= _edge_cut(
        "x",
        along=(cp.BAT_Y - cp.BAT_OPEN_W / 2, cp.BAT_Y + cp.BAT_OPEN_W / 2),
        z0=cp.BAT_Z - cp.BAT_OPEN_H / 2,
        z1=cp.BAT_Z + cp.BAT_OPEN_H / 2,
        through=left,
    )

    # Scroll wheel and the two side buttons, right edge.
    part -= _edge_cut(
        "x",
        along=(cp.WHEEL_Y - cp.WHEEL_OPEN_W / 2, cp.WHEEL_Y + cp.WHEEL_OPEN_W / 2),
        z0=cp.WHEEL_OPEN_Z0,
        z1=cp.WHEEL_OPEN_Z1,
        through=right,
    )
    for y in cp.BUTTON_YS:
        part -= _edge_cut(
            "x",
            along=(y - cp.BUTTON_OPEN_W / 2, y + cp.BUTTON_OPEN_W / 2),
            z0=cp.BUTTON_Z - cp.BUTTON_OPEN_H / 2,
            z1=cp.BUTTON_Z + cp.BUTTON_OPEN_H / 2,
            through=right,
        )

    # microSD, bottom edge.
    part -= _edge_cut(
        "y",
        along=(cp.SD_X - cp.SD_W / 2, cp.SD_X + cp.SD_W / 2),
        z0=cp.SD_Z0,
        z1=cp.SD_Z1,
        through=bottom,
    )

    # --- Finish -----------------------------------------------------------
    # Chamfer the front face: its edges are the outer perimeter and the window.
    front_face = part.faces().filter_by(Plane.XY).group_by(Axis.Z)[-1]
    part = chamfer(front_face.edges(), cp.CHAMFER)

    part.label = "tinyframe_front_frame"
    return part
