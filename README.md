# tinyiv

A small, very fast, GPU rendered image viewer. It decodes on background threads, keeps the
image on the GPU as tiles of a mipmap pyramid, and prefetches the folder around the file, so
stepping is instant and gigapixel files pan without a stutter. An image too large for memory
streams from a pyramid of tiles written to disk on first open, so it opens at full resolution
whatever its size. A JPEG with restart markers, a TIFF read by its strips or tiles, or with
`cache_mode = small` a PNG, gives its full resolution straight from the file, so only the smaller
levels are stored. One window, one folder, one image. No configuration, no editing, no extra
features.

Every format libvips reads is shown. JPEG, PNG, WebP, JPEG XL, BMP, ICO and ICNS go through
their own decoders, with a cheap DCT scaled preview for large JPEGs. ICO and ICNS show their
largest icon. SVG and PDF render again at the zoom on screen once the view rests, so they
stay sharp at any zoom. Animated GIF and WebP play in a loop, with a bar along the bottom to
pause and seek. A file nothing can read says so in place of the image.

Supports HDR images directly and via tone mapping.

Runs on x86-64-v3 CPUs (AVX2, BMI2), such as Intel Haswell and AMD Excavator or newer.
Building on Linux and Windows is described in [COMPILE.md](COMPILE.md).

## Keybinds

| Input                   | Action                        |
|-------------------------|-------------------------------|
| Wheel  ←  →             | Previous, next image          |
| Home  End               | First, last image             |
| R                       | Reload from disk              |
| Left drag               | Pan                           |
| Right drag ↑ ↓          | Zoom in, out                  |
| ↑  ↓                    | Zoom in, out a step           |
| 0  1  2                 | Center, fit, actual size      |
| F  F11  Double click    | Fullscreen                    |
| [  ]                    | Rotate left, right            |
| ;  '                    | Flip vertically, horizontally |
| Ctrl+S                  | Save rotation and flips       |
| S                       | Streaming mode                |
| I                       | Show, hide the status bar     |
| Space                   | Play, pause animation         |
| ,  .                    | Previous, next frame          |
| Click, drag on play bar | Play, pause, seek             |
| Ctrl+H                  | Show, hide the keybinds       |
| Esc  Q  Ctrl+D          | Quit                          |

Dropping a file on the window opens its folder. Started without a file, the window waits
for one.

Rotating and flipping only change how the image is drawn. Ctrl+S writes the result into the
file's orientation tag, for JPEG, PNG, WebP and TIFF, and leaves the pixel data byte for
byte: rotating back and saving again gives back the identical file.

## Configuration

Settings live in `tinyiv.conf`, in `$XDG_CONFIG_HOME/tinyiv` (`~/.config/tinyiv`) on Linux and
beside `tinyiv.exe` on Windows. The first start writes one listing every setting. Each line is
`key = value`, and a line starting with `#` is a comment.

| Key          | Value                                                                               |
|--------------|-------------------------------------------------------------------------------------|
| `cache` | Where the tiles of images too large for memory go. Empty keeps them in a `tinyiv-cache` folder beside each image. A relative path is taken from the image's folder, and `~` is the home folder. |
| `cache_mode` | `fast` or `small`. Fast keeps every tile of a huge PNG, so it pans at once. Small keeps only places to begin decoding the file again instead of its full resolution tiles, about a third of the room overall, but panning at full resolution waits a few tens of milliseconds for each band of rows. |

A folder that cannot be written falls back to the user's cache folder.

## License

GPL-3.0-or-later.
