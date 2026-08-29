# TinyFrame

An e-paper photo player for the Elecrow CrowPanel ESP32 4.2" (400x300 mono).

Photos go in, a short black-and-white clip comes out, and it is baked into the
firmware and played on a loop on the panel.

## Layout

    make_eink_video.py     photo -> short black-and-white clip
    epaper_video/          the player sketch (the interesting one)
    hello_epaper/          hello-world sketch, also a board-revision check

## Making a clip

Two API calls to [Replicate](https://replicate.com) per photo, then one local
ffmpeg encode:

1. `flux-kontext-pro` turns the photo into a 4:3 black-and-white ink drawing
2. `p-video` animates that still into a five-second clip
3. ffmpeg reduces it to 400x300 at 1 bit per pixel, which is what the panel is

Doing the black and white first is deliberate: it is one image edit rather than
a pass over every frame, so it is cheap, easy to look at, and easy to redo. The
video model then animates artwork that is already bold, so the style is settled
before anything moves.

    cp .env.example .env          # add your Replicate token
    pip install replicate
    mkdir -p pictures             # drop photos in
    python3 make_eink_video.py

Outputs land in `bw/` (the still) and `videos/` (the clip and the panel
master). Anything already there is reused, so a re-run never pays twice.

## Putting a clip on the panel

Bake the master into a header, then flash:

    cd epaper_video
    python3 tools/make_video_header.py ../videos/<name>.mp4 --name beach --caption "Beach"
    ./flash.sh

`flash.sh` takes `[invert] [settle_ms] [port]`. `settle_ms` is an exposure, not
a timeout -- see the comment at the top of `epaper_video.ino` before changing
it. Buttons: OK switches clip, EXIT parks the current frame and sleeps so the
cord can be pulled.

## Board revisions

Elecrow shipped two revisions with different display controllers. The drivers
for both are in `driver_variants/`; `flash.sh` copies the right one into place.
`epaper_video` is pinned to the V1.2A (green sticker, UC8176) board.
`hello_epaper/flash.sh 0` builds for the older no-sticker (SSD1683) board.
