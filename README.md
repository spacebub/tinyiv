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

Runs on x86-64-v3 CPUs (AVX2, BMI2), such as Intel Haswell and AMD Excavator or newer.
Building on Linux and Windows is described in [COMPILE.md](COMPILE.md).

## Keys

| Input                   | Action                        |
|-------------------------|-------------------------------|
| Wheel  ←  →             | Previous, next image          |
| Home  End               | First, last image             |
| R                       | Reload from disk              |
| Left drag               | Pan                           |
| Right drag ↑ ↓          | Zoom in, out                  |
| ↑  ↓                    | Zoom in, out a step           |
| 1  2                    | Fit, actual size              |
| F  F11  Double click    | Fullscreen                    |
| [  ]                    | Turn left, right              |
| ;  '                    | Flip vertically, horizontally |
| Ctrl+S                  | Save turns and flips          |
| Space                   | Play, pause animation         |
| ,  .                    | Previous, next frame          |
| Click, drag on play bar | Play, pause, seek             |
| Ctrl+H                  | Show, hide the keys           |
| Esc  Q  Ctrl+D          | Quit                          |

Dropping a file on the window opens its folder. Started without a file, the window waits
for one. Stepping a frame pauses the animation.

Turning and flipping only change how the image is drawn. Ctrl+S writes the result into the
file's orientation tag, for JPEG, PNG, WebP and TIFF, and leaves the pixel data byte for
byte: turning back and saving again gives back the identical file.

## License

GPL-3.0-or-later.
