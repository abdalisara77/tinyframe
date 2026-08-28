#!/usr/bin/env python3
"""
Photos in, flashable firmware data out.

    export REPLICATE_API_TOKEN=r8_...     (or put it in .env)
    python3 make_eink_video.py
    cd epaper_video && ./flash.sh

For every photo in pictures/, two API calls and a local encode:

    1. black and white   photo -> 4:3 ink drawing   (Replicate, costs money)
    2. video             drawing -> 3 second clip   (Replicate, costs money)
    3. panel master      clip -> 400x300, 1 bit     (ffmpeg, local, free)

then every master is packed into one generated header:

    4. clips             all masters -> epaper_video/clips.h

clips.h holds the frame data *and* the CLIPS[] table, so the sketch contains no
clip-specific anything and adding a photo never means editing firmware. It is
generated, gitignored, and rebuilt from pictures/ on demand -- no picture's
byte data is ever a source file.

    bw/<name>.jpg            the black-and-white still
    videos/raw/<name>.mp4    what the video model returned
    videos/<name>.mp4        the panel master -- watch this to see what ships
    epaper_video/clips.h     generated, gitignored

Whatever already exists is reused, so a re-run never pays for the same call
twice. Delete a file to redo that step.
"""
import base64
import mimetypes
import os
import re
import subprocess
import sys
from dotenv import load_dotenv

import replicate

load_dotenv(override=True)

PICTURES = "pictures"
BW = "bw"
OUT = "videos"
RAW = os.path.join(OUT, "raw")
SKETCH = "epaper_video"
CLIPS_H = os.path.join(SKETCH, "clips.h")

EPD_W, EPD_H = 400, 300
FRAME_BYTES = EPD_W * EPD_H // 8          # 15000
FPS = 8
DURATION = 3                              # seconds; 3 * 8 = 24 frames = 360KB raw

# Measured, not guessed. arduino-cli reports the huge_app partition as exactly
# 3,145,728 bytes, and this sketch with one 331KB clip came to 671,712 -- so
# the code, the Arduino core and the 140KB font are about 340KB between them,
# leaving roughly 2.8MB for frame data. 2.5MB keeps a margin for the sketch
# growing. At 360KB for an uncompressed 3s clip that is six clips.
#
# It turns "the link failed with 200 lines of linker output" into one sentence
# you can act on, before anything is compiled.
FLASH_BUDGET = 2_500_000

EXTS = (".jpg", ".jpeg", ".png", ".webp", ".bmp", ".tif", ".tiff", ".heic")


# --------------------------------------------------------------------------
# 1. photo -> black-and-white still
# --------------------------------------------------------------------------

MODEL_BW = "google/nano-banana-2"

BW_PROMPT = (
"Convert this photograph to a detailed and realistic black " 
"and white watercolor drawing illustration. Bold clean linework,"
" simple flat shapes. Keep the same people, the same faces and "
" the same composition. Do not add any other people or objects. "
"Keep the people pretty."
)


# --------------------------------------------------------------------------
# 2. still -> video
# --------------------------------------------------------------------------

MODEL_VIDEO = "xai/grok-imagine-video-1.5"

# This model has no negative_prompt field, so everything -- including what we
# do *not* want -- has to be said here, positively. The faces come first
# because that is the thing most worth keeping: an image-to-video model is free
# to redraw a face a little more each frame, and by the end it is someone else.
# Restating the style matters too, or the model drifts back towards photograph.
VIDEO_PROMPT = (
    "black and white watercolor illustration, unchanged drawing style, "
    "the same people, faces unchanged and clearly recognisable, "
    "slow gentle camera drift, subtle natural movement, "
    "steady locked-off framing."
)


# --------------------------------------------------------------------------
# 3. -> the panel master
# --------------------------------------------------------------------------

# Fit the whole picture inside 400x300 and pad out the remainder, rather than
# filling the frame and cropping. Padding can never cut a face off the edge;
# cropping can. For a 4:3 source there is nothing to pad and the two are the
# same thing, which is the usual case here -- both models above are asked for
# 4:3 -- so this only ever costs anything on an odd-shaped photo, and what it
# costs there is a bar rather than somebody's head.
#
# The padding is white, which INVERT=1 on the device turns black along with the
# rest of the background, so the bars stay consistent with the artwork.
FIT = ("scale=%d:%d:force_original_aspect_ratio=decrease:flags=area,"
       "pad=%d:%d:(ow-iw)/2:(oh-ih)/2:color=white"
       % (EPD_W, EPD_H, EPD_W, EPD_H))

# This is the *only* place the picture is fitted, toned and reduced to one bit.
# Step 4 below reads the finished master straight through without touching any
# of it: re-scaling a 1-bit image would put greys back in, and re-thresholding
# those greys is how you get speckle along every edge. Fit once.
# How much grey the panel gets to pretend to have.
#
# The glass is one bit: a pixel is black or it is white, and there is no
# EPD_Update_4Gray to call -- the vendor header declares one but no driver in
# this repo defines it, and a real 4-grey mode would want 2 bits per pixel and
# a multi-second waveform per frame, which is not a thing you can play at 8fps.
#
# So grey here is halftone: a mix of black and white pixels that averages to
# the tone you wanted. Two knobs decide how much of it survives.
#
# DITHER_SCALE is ffmpeg's bayer_scale, and it reads backwards from what you
# would guess. 0 is the FINEST pattern and gives the most apparent grey; 5 is
# the coarsest and collapses the picture towards flat black and white. Fine
# dithering costs flash -- alternating pixels defeat run-length encoding, so a
# clip stops compressing and stores raw at 360KB.
DITHER_SCALE = 0

# Nearly linear. The old curve was a hard S that pushed the midtones out to the
# rails before the dither ever saw them, which is exactly the tone the halftone
# needed to work with. Steepen it if the result looks flat on the glass.
CURVE = "curves=all='0/0 0.3/0.28 0.7/0.72 1/1'"

# Some acutance helps at 400x300, but the old 0.8 was fighting the point of
# this section by driving edge pixels to the rails.
SHARPEN = 0.5

# 256 pixels of pure black and pure white -- the size paletteuse insists on.
# Built as a lavfi input so there is no palette file to write and delete, and
# trimmed to one frame because color= is an endless source and ffmpeg would
# otherwise sit generating palette frames forever.
_BW_PX = "if(lt(X,8),0,255)"
PALETTE = ("color=c=black:s=16x16,format=rgb24,"
           "geq=r='%s':g='%s':b='%s',trim=end_frame=1"
           % (_BW_PX, _BW_PX, _BW_PX))

# Ordered dither against that two-entry palette. The Bayer threshold is a
# function of x,y alone, so an unchanged pixel decides the same way every frame
# and the halftone sits still instead of crawling -- error diffusion cannot
# promise that, and a crawling halftone is a screenful of needless panel swings
# per frame.
FILTERS = ("[0:v]fps=%d,%s,format=gray,unsharp=5:5:%g:5:5:0,%s,"
           "format=rgb24[v];"
           "[v][1:v]paletteuse=dither=bayer:bayer_scale=%d:new=0,format=gray"
           % (FPS, FIT, SHARPEN, CURVE, DITHER_SCALE))

# -qp 0 is lossless, so a decoded pixel is the byte that went in; yuvj444p
# keeps full chroma at full range, where the usual yuv420p would throw away
# half the pixels and interpolate grey back in on the way out.
ENCODE = ["-r", str(FPS), "-fps_mode", "cfr",
          "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuvj444p", "-an"]


# --------------------------------------------------------------------------

def data_uri(path):
    """Inline a file as a data: URI, rather than handing over a file handle.

    Given a handle, the replicate library uploads it to Replicate's own files
    API and passes that URL. Whether that works is up to the model: it needs
    Replicate's credentials to read it back. grok-imagine cannot, and reports
    the auth failure it gets as "Invalid image format. Supported formats:
    .jpeg, .jpg, .png, .webp" -- about a URL ending in .png, which sends you
    looking at the file instead of at the fetch.

    A data URI carries the bytes in the request, so there is no second fetch to
    fail and no model-by-model difference to remember.
    """
    mime = mimetypes.guess_type(path)[0] or "application/octet-stream"
    with open(path, "rb") as fh:
        return "data:%s;base64,%s" % (mime, base64.b64encode(fh.read()).decode())


def download(output, dst):
    """replicate.run hands back a FileOutput, or a list of them."""
    if isinstance(output, (list, tuple)):
        output = output[0]
    with open(dst, "wb") as fh:
        fh.write(output.read())


def make_black_and_white(photo, dst):
    """API call 1: photo -> 4:3 black-and-white still."""
    output = replicate.run(MODEL_BW, input={
        # image_input, and it is an ARRAY. Pass anything else -- input_image,
        # image -- and the model does not complain: it drops the unknown field
        # and generates from the prompt alone, which looks like a working call
        # that returns a confident picture of complete strangers.
        "image_input": [data_uri(photo)],
        "prompt": BW_PROMPT,
        "aspect_ratio": "4:3",
        # jpg keeps this small, because step 2 has to carry it back up as a
        # data URI. The hard-edge argument for png does not survive the video
        # model in between, which re-encodes far more destructively than jpeg.
        "output_format": "jpg",
    })
    download(output, dst)


def make_video(still, dst):
    """API call 2: still -> short clip, faces and style kept as they are."""
    output = replicate.run(MODEL_VIDEO, input={
        "image": data_uri(still),
        "prompt": VIDEO_PROMPT,
        # the still is already 4:3 and so is the panel, so say so rather than
        # letting the model default to the image's native shape
        "aspect_ratio": "4:3",
        "resolution": "720p",
        "duration": DURATION,
    })
    download(output, dst)


def make_master(src, dst):
    """Local encode: clip -> 400x300, nothing but 0 and 255."""
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-i", src,
                    "-f", "lavfi", "-i", PALETTE,
                    "-filter_complex", FILTERS] + ENCODE + [dst], check=True)


# --------------------------------------------------------------------------
# 4. masters -> one generated header
# --------------------------------------------------------------------------

def read_frames(master):
    """The master, straight through, as the panel's own 1bpp layout.

    monob is MSB-first with bit=1 meaning white, which is exactly the panel
    framebuffer, so nothing here needs bit fiddling. No scaling and no curve:
    the master is already 400x300 and already binary.
    """
    raw = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", master, "-vf", "format=monob",
         "-f", "rawvideo", "-"], capture_output=True, check=True).stdout
    if not raw or len(raw) % FRAME_BYTES:
        sys.exit("%s decoded to %d bytes, not a whole number of %d-byte frames"
                 % (master, len(raw), FRAME_BYTES))
    return [raw[i:i + FRAME_BYTES] for i in range(0, len(raw), FRAME_BYTES)]


def rle(buf):
    """Byte-run encode as (count, value) pairs, count capped at 255."""
    out = bytearray()
    i, n = 0, len(buf)
    while i < n:
        v = buf[i]
        run = 1
        while i + run < n and buf[i + run] == v and run < 255:
            run += 1
        out.append(run)
        out.append(v)
        i += run
    return bytes(out)


def pack(frames):
    """RLE the frames, or fall back to raw when RLE would cost more.

    Densely cross-hatched art defeats run-length encoding outright: when the
    image alternates every pixel, every run is length 1 and each byte becomes
    two. The fallback means a busy clip can never cost more than uncompressed.
    """
    blob, offsets = bytearray(), [0]
    for f in frames:
        blob += rle(f)
        offsets.append(len(blob))
    if len(blob) >= len(frames) * FRAME_BYTES:
        return bytearray(b"".join(frames)), \
               [i * FRAME_BYTES for i in range(len(frames) + 1)], False
    return blob, offsets, True


def emit_clips(clips, path):
    """Write every clip, and the table the sketch plays them from, as one file.

    The sketch defines `struct Clip` and then includes this, so the table can
    be built here and the firmware needs to know nothing about any clip.
    """
    with open(path, "w") as fh:
        fh.write("// Generated by make_eink_video.py -- do not edit, do not commit.\n")
        fh.write("// Rebuild with: python3 make_eink_video.py\n")
        fh.write("//\n// Included by the sketch *after* struct Clip is defined.\n\n")
        fh.write("#pragma once\n#include <stdint.h>\n\n")

        for i, c in enumerate(clips):
            fh.write("// %s: %d frames @ %g fps, %s, %d bytes\n"
                     % (c["name"], len(c["frames"]), FPS,
                        "RLE" if c["rle"] else "raw (RLE lost)", len(c["blob"])))
            fh.write("static const uint32_t CLIP%d_OFFSETS[%d] = {\n"
                     % (i, len(c["offsets"])))
            for j in range(0, len(c["offsets"]), 12):
                fh.write("  " + ",".join(str(o) for o in c["offsets"][j:j + 12]) + ",\n")
            fh.write("};\n")
            fh.write("static const uint8_t CLIP%d_DATA[%d] = {\n" % (i, len(c["blob"])))
            for j in range(0, len(c["blob"]), 20):
                fh.write("  " + ",".join("0x%02X" % b for b in c["blob"][j:j + 20]) + ",\n")
            fh.write("};\n\n")

        fh.write("static const Clip CLIPS[] = {\n")
        for i, c in enumerate(clips):
            fh.write('  { CLIP%d_DATA, CLIP%d_OFFSETS, %d, %s, "%s", "%s" },\n'
                     % (i, i, len(c["frames"]), "true" if c["rle"] else "false",
                        c["caption"].replace('"', '\\"'), c["name"]))
        fh.write("};\n")
        fh.write("static const uint8_t CLIP_COUNT = %d;\n" % len(clips))


# --------------------------------------------------------------------------

def main():
    if not os.path.isdir(PICTURES):
        raise SystemExit("no %s/ directory -- put your photos there" % PICTURES)
    if not os.environ.get("REPLICATE_API_TOKEN"):
        raise SystemExit("set REPLICATE_API_TOKEN, or put it in .env")

    os.makedirs(BW, exist_ok=True)
    os.makedirs(RAW, exist_ok=True)
    photos = sorted(f for f in os.listdir(PICTURES) if f.lower().endswith(EXTS))
    if not photos:
        raise SystemExit("no photos in %s/" % PICTURES)

    clips = []
    for photo in photos:
        stem = os.path.splitext(photo)[0]
        name = re.sub(r"\W+", "_", stem).strip("_").lower()
        still = os.path.join(BW, name + ".jpg")
        raw = os.path.join(RAW, name + ".mp4")
        master = os.path.join(OUT, name + ".mp4")

        # Nested, not three independent checks: each step is only needed if
        # the thing it feeds is missing. Flat checks would re-run a paid call
        # to rebuild an input that nothing downstream still wants.
        print("%s" % name)
        print(BW_PROMPT)
        if not os.path.exists(master):
            if not os.path.exists(raw):
                if not os.path.exists(still):
                    print("  1/4  black and white")
                    make_black_and_white(os.path.join(PICTURES, photo), still)
                print("  2/4  video")
                make_video(still, raw)
            print("  3/4  panel master")
            make_master(raw, master)

        # An optional pictures/<name>.txt is drawn over the top-left corner.
        sidecar = os.path.join(PICTURES, stem + ".txt")
        caption = open(sidecar).read().strip() if os.path.exists(sidecar) else ""

        frames = read_frames(master)
        blob, offsets, encoded = pack(frames)
        clips.append({"name": name[:24], "caption": caption, "frames": frames,
                      "blob": blob, "offsets": offsets, "rle": encoded})
        print("       %d frames, %d bytes (%s)"
              % (len(frames), len(blob), "RLE" if encoded else "raw, RLE lost"))

    total = sum(len(c["blob"]) for c in clips)
    print("\n%d clip(s), %d bytes of frame data (budget %d)"
          % (len(clips), total, FLASH_BUDGET))
    if total > FLASH_BUDGET:
        raise SystemExit(
            "that will not fit in the app partition.\n"
            "Drop a photo from %s/, shorten DURATION (now %ds), lower FPS (now %d),\n"
            "or give the app more than huge_app's ~3MB with a custom partition table."
            % (PICTURES, DURATION, FPS))

    emit_clips(clips, CLIPS_H)
    print("wrote %s -- now: cd %s && ./flash.sh" % (CLIPS_H, SKETCH))


if __name__ == "__main__":
    main()
