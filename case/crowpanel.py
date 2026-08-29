"""Measured facts about the CrowPanel ESP32 4.2" board, and the frame that wraps it.

Every number here was read out of Elecrow's own STEP model of the product
(`3D file/CrowPanel ESP32 4.2" E-paper HMI Display.stp` in
github.com/Elecrow-RD/CrowPanel-ESP32-4.2-E-paper-HMI-Display-with-400-300),
not from a spec sheet or a photo. Provenance is noted per block so a number can
be re-checked against the source model rather than taken on trust.

COORDINATE SYSTEM
    Origin  centre of the PCB, on its FRONT face (the face the panel sits on).
    +X      right, viewed from the front
    +Y      up, viewed from the front
    +Z      out of the screen, toward the viewer

Elecrow's model uses a different frame (Y is the board normal, and the product
is upright along -Z). The mapping applied throughout this file is

    X = x_elecrow      Y = -z_elecrow      Z = y_elecrow + 0.5

The +0.5 puts the origin on the PCB front face; the Y flip makes +Y "up" as the
product is actually used -- Elecrow model that upside down relative to the way
the ribbon hangs. Orientation was confirmed against Elecrow's product photo:
the panel's FPC ribbon exits the BOTTOM edge, which fixes every other feature.
"""

# --- PCB ------------------------------------------------------------------
# Elecrow solid "4_2_PCB": 107.0 x 1.6 x 87.0, centred on the model origin.
PCB_W = 107.0
PCB_H = 87.0
PCB_T = 1.6

# --- E-paper module -------------------------------------------------------
# Elecrow solid "4_2LCD": x +/-45.5, glass body z +/-38.5, front face at
# y_e = +1.2 -> Z = +1.7. Cross-checked against the acrylic bezel window
# (91.2 x 77.2 centred at z=0), which frames the module outline exactly.
MODULE_W = 91.0
MODULE_H = 77.0
MODULE_FRONT_Z = 1.7          # glass sits 1.7 mm proud of the PCB front face

# The 400x300 image area is 84.8 x 63.6 (400 and 300 pixels at 0.212 mm pitch).
# It is NOT centred in the module: the border is narrow on the top edge and
# ~10 mm on the ribbon (bottom) edge. Elecrow's acrylic frames the module
# outline rather than the image, so their CAD does not pin the offset down, and
# it could not be measured to better than about +/-1 mm remotely. The window
# below is therefore sized off the MODULE outline, which is known exactly, and
# is deliberately larger than the image area so no offset can ever clip it.
ACTIVE_W = 84.8
ACTIVE_H = 63.6

# --- Mounting holes -------------------------------------------------------
# Elecrow's six M2.5 standoffs sit at (+/-49.5, +/-39.5) and (+/-30, 0).
# Only the four corners are usable from the front: the two mid-edge holes at
# x = +/-30 lie under the panel module (|x| < 45.5), so nothing can reach the
# PCB front face there. Four corners is plenty for a frame.
MOUNT_XY = [(49.5, 39.5), (-49.5, 39.5), (49.5, -39.5), (-49.5, -39.5)]
MOUNT_SCREW = 2.5

# --- Edge-mounted parts, as openings the case has to provide ---------------
# Each entry: (centre along its edge, extent along the edge, Z centre, Z extent)
# taken from the Elecrow solids named in the comments, then padded for clearance
# where noted. Z is depth: negative is behind the PCB front face.

# LEFT edge (X = -53.5)
USB_C_Y, USB_C_Z = -20.53, -2.71      # "CUT-EXTRUDE5": 8.34 wide x 3.59 tall
USB_C_OPEN_W, USB_C_OPEN_H = 11.0, 6.0

BAT_Y, BAT_Z = 0.0, -2.9              # "BREP_77": 4.0 wide x 3.4 tall
BAT_OPEN_W, BAT_OPEN_H = 7.0, 5.5

# RIGHT edge (X = +53.5)
WHEEL_Y, WHEEL_Z = 0.0, -2.4          # "XB-TM-2024A" scroll wheel, protrudes
WHEEL_OPEN_W = 17.0                   # to X = 55.94, so it needs a finger slot
WHEEL_OPEN_Z0, WHEEL_OPEN_Z1 = -5.5, 1.0

BUTTON_YS = (21.5, -21.5)             # "BREP_61/62/63", plungers reach X=54.55
BUTTON_Z = -3.34
BUTTON_OPEN_W, BUTTON_OPEN_H = 7.0, 5.0

# BOTTOM edge (Y = -43.5)
SD_X, SD_W = 25.55, 18.1              # "TF": card slot, 16.4 wide
SD_Z0, SD_Z1 = -4.45, 0.05

# BACK face
REAR_BUTTON_XY = [(-16.3, 38.0), (-30.0, 38.0)]   # RST/BOOT tact switches,
REAR_BUTTON_D = 5.0                                # tops 6.5 mm behind the PCB
REAR_DEEPEST_Z = -8.1                              # deepest thing on the back

# Optional: the 2x10 GPIO header, X 7.2..32.8, Y 26.5..42.5, 5.0 mm proud of
# the PCB back. Enclosed by default -- the frame reads better with a clean
# back, and the battery connector on the left edge is already exposed.
HEADER_OPENING = False
HEADER_X0, HEADER_X1 = 6.2, 33.8
HEADER_Y0, HEADER_Y1 = 25.5, 43.5

# --- Case ------------------------------------------------------------------
WALL = 2.4                    # side walls
FACE_T = 3.0                  # front plate
BACK_T = 2.0                  # rear plate
FIT = 0.35                    # clearance around the PCB edge
GLASS_GAP = 0.5               # air gap over the panel: the frame never touches it
REAR_CLEAR = 0.5              # gap behind the tallest rear component

# The window is the module outline less a retaining lip. The lip is what the
# frame shows of itself over the panel; it also hides the module's own border.
WINDOW_LIP = 1.5

CASE_W = PCB_W + 2 * (FIT + WALL)        # 112.5
CASE_H = PCB_H + 2 * (FIT + WALL)        # 92.5
CASE_R = 5.0                             # outer corner radius

WINDOW_W = MODULE_W - 2 * WINDOW_LIP     # 88.0
WINDOW_H = MODULE_H - 2 * WINDOW_LIP     # 74.0
WINDOW_R = 2.0

CAVITY_W = PCB_W + 2 * FIT               # 107.7
CAVITY_H = PCB_H + 2 * FIT               # 87.7
CAVITY_R = CASE_R - WALL                 # 2.6

# Depths, all measured from the PCB front face at Z = 0.
FACE_INNER_Z = MODULE_FRONT_Z + GLASS_GAP        # +2.2
FACE_OUTER_Z = FACE_INNER_Z + FACE_T             # +5.2
CAVITY_FLOOR_Z = REAR_DEEPEST_Z - REAR_CLEAR     # -8.6
BACK_OUTER_Z = CAVITY_FLOOR_Z - BACK_T           # -10.6
PCB_BACK_Z = -PCB_T                              # -1.6

CASE_DEPTH = FACE_OUTER_Z - BACK_OUTER_Z         # 15.8

# Fasteners: one M2.5 x 12 through each corner, from the back. It passes the
# rear plate, its standoff, the PCB, and threads into a boss in the front frame.
BOSS_D = 6.0
BOSS_PILOT_D = 2.05           # thread-forming pilot for M2.5 in printed plastic
BOSS_PILOT_DEPTH = 4.7        # into the plate, leaving 0.5 mm of front face
SCREW_CLEAR_D = 2.9
SCREW_HEAD_D = 5.2
SCREW_HEAD_DEPTH = 1.6

CHAMFER = 0.8                 # front face, outer edge and window edge

BACK_PLATE_W = CAVITY_W - 0.4
BACK_PLATE_H = CAVITY_H - 0.4
BACK_PLATE_R = CAVITY_R - 0.2
