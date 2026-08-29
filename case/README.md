# TinyFrame case

A two-part printed case for the CrowPanel ESP32 4.2" board: a picture frame that
holds the panel, and a back that closes it. Four screws, no glue, no supports.

    tinyframe_front.step.py    the visible frame -- window, walls, port openings
    tinyframe_rear.step.py     the back plate -- standoffs, screw channels
    crowpanel.py               every board dimension, and where it came from
    fitcheck.step.py           validation fixture: the case around the real board

`.step`, `.stl` and `.3mf` for both parts are checked in beside their sources.
Print the `.3mf` or `.stl`; regenerate everything from the `.py` if you change a
dimension.

## What it is

    outer          112.5 x 92.5 x 15.8 mm
    window         88 x 74 mm, centred on the panel
    borders        12.25 mm sides, 9.25 mm top and bottom
    fasteners      4 x M2.5 x 12 self-tapping, from the back

The board is captured between four bosses in the front frame and four standoffs
in the back. One screw per corner passes the back plate, its standoff, the PCB,
and threads into the boss, so nothing is visible from the front and the panel
itself is never clamped -- there is a deliberate 0.5 mm air gap over the glass.

Openings are provided for everything on the board's edges: USB-C and the battery
connector on the left, the scroll wheel and both side buttons on the right, the
microSD slot on the bottom, and the RST/BOOT switches through the back.

## Where the numbers came from

Every board dimension in `crowpanel.py` was read out of Elecrow's own STEP model
of the product, not from a spec sheet. Their model even includes the acrylic case
it ships with, which is how the panel module's outline (91 x 77.2, centred on the
PCB) is known exactly: it is the window they cut in their own bezel.

One number is *not* known exactly. The 400x300 image area is 84.8 x 63.6 mm, and
it sits off-centre in the module -- a narrow border along the top edge and about
10 mm along the bottom, where the ribbon exits. Elecrow's bezel frames the module
outline rather than the image, so their CAD does not pin that offset down, and it
could not be measured to better than roughly +/-1 mm from photographs.

So the window is sized off the module outline, which *is* known: 91 x 77 less a
1.5 mm retaining lip all round. That is larger than the image area by a wide
enough margin that no plausible offset can clip it. The cost is cosmetic -- a
sliver of the panel's own white border may show inside the window, mostly along
the bottom edge. If you want a tight mat instead, measure the border above and
below the image on your unit and set `WINDOW_*` in `crowpanel.py` accordingly;
only the front frame needs reprinting.

## Printing

Both parts print without supports.

    front frame    face down on the bed. The window is a through hole, so
                   nothing overhangs; walls and bosses grow upward, and the
                   visible face is the one against the plate.
    rear shell     outside face down, standoffs up.

0.2 mm layers, 3 perimeters, 20% infill. PLA is fine indoors; PETG if it will sit
in a window, since e-paper and sunlight both like that spot. Wall thickness is
2.4 mm and the front plate 3.0 mm, so nothing is fragile.

The four bosses take M2.5 self-tapping (thread-forming) screws directly into a
2.05 mm pilot -- no inserts. If your slicer's hole compensation runs tight, drill
the pilots out to 2.1 mm rather than forcing the screw.

## Assembly

1. Take the board out of the acrylic case if it is in one, and **keep the four
   M2.5 standoffs off** -- the printed shell has its own, in the same places.
2. Drop the board into the front frame, panel first. It lands on the four bosses
   and locates on the walls with 0.35 mm to spare.
3. Set the rear shell in place; its standoffs meet the back of the PCB.
4. Four M2.5 x 12 screws from the back. Snug, not tight -- they are threading
   into plastic.

## Checks that were actually run

- `inspect validate` on both parts: closed, correctly oriented solids, 0 failures.
- Bounding boxes measured against the design intent: front 112.5 x 92.5 x 15.8,
  rear 107.3 x 87.3 x 9.0.
- `inspect interfere` on `fitcheck.step.py`, which places both printed parts
  around Elecrow's model of the board: **no clash involving either printed
  part**. The clashes the report does list are all inside Elecrow's own model
  (it carries a duplicated panel solid, and SMD pads modelled sunk into the PCB).
- Snapshot review of both parts and the assembly: window, port openings, and
  connector alignment checked visually.

Not checked: anything requiring the physical board. Screw thread engagement in
printed plastic, USB-C plug overmold clearance against the wall, and how much of
the panel border the window reveals are all first-article questions.

## Regenerating

The CAD skill from the `text-to-cad` plugin owns the toolchain:

    python scripts/gen  case/tinyframe_front.step.py case/tinyframe_rear.step.py --write
    python scripts/export case/tinyframe_front.step.py --stl --3mf

`fitcheck.step.py` needs Elecrow's board model, which is theirs and is not
committed here. Fetch it with:

    git clone --depth 1 https://github.com/Elecrow-RD/CrowPanel-ESP32-4.2-E-paper-HMI-Display-with-400-300 /tmp/crowpanel
    cp "/tmp/crowpanel/3D file/CrowPanel ESP32 4.2” E-paper HMI Display.stp" case/reference/crowpanel_42.stp
