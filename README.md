# tinyiv

A small, very fast, GPU rendered image viewer. It decodes on background threads, keeps the
image on the GPU as tiles of a mipmap pyramid and prefetches the images around the current one,
so stepping through a folder is instant and gigapixel files pan smoothly. One window, one
folder, one image, with no editing and no extra features.

Images too large for memory are streamed. The first time one opens, tinyiv builds a pyramid of
tiles and caches it on disk, so it shows at full resolution whatever its size, and opens
instantly the next time. JPEGs with restart markers and most TIFFs are read straight from the
file at full size, so only their smaller levels are cached. With `cache_mode = small`, huge PNGs
work the same way.

Every format libvips can read is supported. JPEG, PNG, WebP, JPEG XL, TIFF, BMP, ICO and ICNS
are decoded directly, with a quick DCT scaled preview for large JPEGs. ICO and ICNS show their
largest icon. SVG and PDF are rendered again at the current zoom once the view settles, so they
stay sharp. Animated GIF and WebP loop, with a bar along the bottom to pause and seek. A file
that can't be read says so in place of the image.

HDR images are shown in HDR on HDR displays and tone mapped everywhere else.

Runs on x86-64-v3 CPUs (AVX2, BMI2), such as Intel Haswell or AMD Excavator and newer.
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
| S                       | Toggle streaming mode         |
| I                       | Show, hide the status bar     |
| Space                   | Play, pause animation         |
| ,  .                    | Previous, next frame          |
| Click, drag on play bar | Play, pause, seek             |
| Ctrl+H                  | Show, hide the keybinds       |
| Esc  Q  Ctrl+D          | Quit                          |

Drop a file on the window to open its folder. Started without a file, tinyiv waits for one.

Rotating and flipping only change how the image is drawn. Ctrl+S saves the result to the
file's orientation tag for JPEG, PNG, WebP and TIFF without touching the pixel data, so
rotating back and saving again gives back the identical file.

## Configuration

Settings are kept in `tinyiv.conf`, in `$XDG_CONFIG_HOME/tinyiv` (usually `~/.config/tinyiv`)
on Linux and next to `tinyiv.exe` on Windows. tinyiv creates the file on first start and adds
any setting it is missing with its default value.

| Setting      | Meaning                                                                                                                                                                                                                  |
|--------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `cache`      | Where tile caches are kept. Empty keeps each cache in a `tinyiv-cache` folder next to its image. A relative path starts from the image's folder, and `~` means your home folder.                                        |
| `cache_mode` | `fast` or `small`. Fast caches every tile of a huge PNG, so panning is instant. Small caches only the points decoding can resume from, which takes about a third of the space, but panning at full size waits a few tens of milliseconds for each band of rows. |

If the cache folder can't be written, tinyiv falls back to your user cache folder.

## License

GPL-3.0-or-later.
