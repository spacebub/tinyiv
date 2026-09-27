# tinyiv

A small, very fast, GPU rendered image viewer. It decodes on background threads, keeps the
image on the GPU as tiles of a mipmap pyramid, and prefetches the folder around the file, so
stepping is instant and gigapixel files pan without a stutter. One window, one folder, one
image. No configuration, no editing, no extra features.

Every format libvips reads is shown. JPEG, PNG, WebP, JPEG XL, BMP, ICO and ICNS go through
their own decoders, with a cheap DCT scaled preview for large JPEGs. ICO and ICNS show their
largest icon. SVG and PDF render again at the zoom on screen once the view rests, so they
stay sharp at any zoom. Animated GIF and WebP play in a loop, with a bar along the bottom to
pause and seek. A file nothing can read says so in place of the image.

Building on Linux and Windows is described in [COMPILE.md](COMPILE.md).

## Keys

| Input                   | Action                     |
|-------------------------|----------------------------|
| Wheel, Left, Right      | Previous, next image       |
| Home, End               | First, last image          |
| Left drag               | Pan                        |
| Right drag up, down     | Zoom in, out               |
| Double click, F, F11    | Fullscreen                 |
| Space                   | Play, pause animation      |
| [, ]                    | Previous, next frame       |
| Click, drag on play bar | Play, pause, seek          |
| Escape, Q               | Quit                       |

Dropping a file on the window opens its folder. Started without a file, the window waits
for one. Stepping a frame pauses the animation.

## License

GPL-3.0-or-later.
