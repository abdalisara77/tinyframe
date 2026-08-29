# TinyFrame

An e-paper photo player for the Elecrow CrowPanel ESP32 4.2" (400x300, 1 bit).

Photos go in, short black-and-white clips come out, and they are baked into the
firmware and played on the panel.

## Layout

    make_eink_video.py     photos -> generated clip data
    epaper_video/          the player sketch

## Making clips

The photograph is kept as a photograph. There is no stylising step, because the
look comes from the halftone at the end -- a photo dithered to one bit already
reads as a picture, and anything a model redraws first is detail the dither
then has to invent tone for.

For every photo in `pictures/`, one API call and one local encode:

1. **video** -- photo to a 3-second clip (Replicate, `xai/grok-imagine-video-1.5`, costs money)
2. **panel master** -- clip to 400x300 at 1 bit, 8fps, ordered Bayer dither (ffmpeg, local, free)

then every master is packed into one generated header:

3. **clips** -- all masters to `epaper_video/clips.h`

`clips.h` holds the frame data *and* the `CLIPS[]` table, so the sketch contains
nothing clip-specific and adding a photo never means editing firmware. It is
generated and gitignored -- no picture's byte data is ever a source file.

    cp .env.example .env          # add your Replicate token
    pip install replicate python-dotenv
    mkdir -p pictures             # drop photos in
    python3 make_eink_video.py

Needs `ffmpeg` on PATH. Outputs:

    videos/raw/<name>.mp4    what the video model returned
    videos/<name>.mp4        the panel master -- watch this to see what ships
    epaper_video/clips.h     generated, gitignored

Whatever already exists is reused, so a re-run never pays for the same call
twice. Delete a file to redo that step.

## Putting the clips on the panel

    cd epaper_video
    ./flash.sh

`flash.sh` takes `[invert] [settle_ms] [port]`, defaulting to `1` (white artwork
on black), `100`, and the first `/dev/cu.usbserial-*` it finds. Needs
`arduino-cli`.

`settle_ms` is an **exposure, not a timeout** -- this panel keeps driving for as
long as you give it, so too little leaves frames faint and too much saturates
them to black (measured: 100 and 500 both correct, 4000 solid black). Read the
comment at the top of `epaper_video.ino` before changing it.

Playback runs every baked-in clip once, in order, and then sleeps. OK (middle
button) cuts the current clip short and moves to the next. EXIT parks the
current frame and sleeps, so the cord can be pulled.

## Board revisions

Elecrow shipped two revisions with different display controllers. Both drivers
are in `epaper_video/driver_variants/`, and `flash.sh` copies the right one into
place. This is pinned to the V1.2A (green sticker, UC8176); the older no-sticker
board is SSD1683.

## Known rough edges

- **The parked frame keeps a faint ghost of the previous picture.** What has
  been tried and ruled out, all measured on the glass, is written up at the top
  of `epaper_video.ino`.
- **The park busy-wait always burns its full `PARK_MAX_WAIT_MS`.** It samples
  BUSY's polarity immediately after `EPD_Display_Fast()` to `EPD_Update()` to
  `EPD_ReadBusy()`, and that vendor loop cannot return until BUSY reads 0 -- so
  the level sampled as "busy" is fixed by the loop above it, whichever polarity
  is actually true. Underneath it, the driver mixes UC8276C and SSD16xx register
  sets; the real fix is a driver written for the part.
