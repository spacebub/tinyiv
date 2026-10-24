# tinyiv

A small, very fast, GPU rendered image viewer. It decodes on background threads and prefetches
the images around the current one, so stepping through a folder is instant. Images live on the
GPU as tiles of a mipmap pyramid, so gigapixel files pan smoothly. One window, one folder, one
image, with no editing and no extra features.

Images too large for memory are streamed. The first time one opens, tinyiv builds a pyramid of
tiles and caches it on disk. The image then shows at full resolution at any size and opens
instantly the next time. Some files are read straight at full size: JPEGs with restart markers,
most TIFFs, and HEIF or AVIF grids. Only their smaller levels are cached. With
`cache_mode = small`, huge PNGs work the same way.

Every format libvips can read is supported. JPEG, PNG, WebP, JPEG XL, TIFF, BMP, ICO and ICNS
are decoded directly. Large JPEGs get a quick DCT scaled preview first. ICO and ICNS show their
largest icon. SVG and PDF are rendered again at the current zoom once the view settles, so they
stay sharp. Animated GIF and WebP loop, with a bar along the bottom to pause and seek. When a
file can't be read, tinyiv says so in place of the image.

HDR images are shown in HDR on HDR displays and tone mapped everywhere else.

Runs on x86-64-v3 CPUs (AVX2, BMI2), such as Intel Haswell or AMD Excavator and newer.
Building on Linux and Windows is described in [COMPILE.md](COMPILE.md).

## Keybinds

| Input                   | Action                        |
|-------------------------|-------------------------------|
| Wheel  ←  →             | Previous, next image          |
| Home  End               | First, last image             |
| O                       | Sort by name, date, size      |
| R                       | Reload from disk              |
| Shift+R                 | Rebuild the tile cache        |
| Left drag               | Pan                           |
| Right drag ↑ ↓          | Zoom in, out                  |
| ↑  ↓                    | Zoom in, out a step           |
| 0  1  2                 | Center, fit, actual size      |
| F  F11  Double click    | Fullscreen                    |
| [  ]                    | Rotate left, right            |
| ;  '                    | Flip vertically, horizontally |
| Ctrl+S                  | Save rotation and flips       |
| S                       | Toggle streaming mode         |
| I                       | Show, hide the bars           |
| Space                   | Play, pause animation         |
| ,  .                    | Previous, next frame          |
| Click, drag on play bar | Play, pause, seek             |
| Ctrl+H                  | Show, hide the keybinds       |
| Esc  Q  Ctrl+D          | Quit                          |

Drop a file on the window to open its folder. Without a file, tinyiv starts empty and waits for
one.

Rotating and flipping only change how the image is drawn. For JPEG, PNG, WebP and TIFF, Ctrl+S
writes the result to the orientation tag in the file. The pixel data stays untouched, so
rotating back and saving again restores the identical file.

## Configuration

Settings are kept in `tinyiv.conf`. On Linux it lives in `$XDG_CONFIG_HOME/tinyiv`, usually
`~/.config/tinyiv`. On Windows it sits next to `tinyiv.exe`. tinyiv creates the file on first
start and later adds any missing setting with its default value.

| Setting           | Meaning                                                                                                                                                                                                                                                                                                                                                                                                                         |
|-------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `cache`           | Where tile caches are kept. Empty keeps each cache in a `tinyiv-cache` folder next to the image. A relative path starts from the folder of the image, and `~` means your home folder.                                                                                                                                                                                                                                           |
| `cache_mode`      | `fast` or `small`. Fast caches every tile of a huge PNG, so panning is instant. Small caches only the points where decoding can resume. It takes about a third of the space, but panning at full size waits a few tens of milliseconds for each band of rows. The mode applies to new caches only. Existing caches keep working in either mode. Shift+R rebuilds the cache of the image on screen in the current mode.          |
| `streaming_mode`  | `on` or `off`. Whether streaming mode is on when tinyiv starts. S switches it while tinyiv runs. Images too large for memory always stream.                                                                                                                                                                                                                                                                                     |
| `persist_streams` | `on` or `off`. Whether streamed images that fit in memory also keep their tiles on disk. Off caches only images too large for memory. On caches every streamed image, so it opens at once the next time.                                                                                                                                                                                                                        |
| `memory_mode`     | `speed` or `footprint`. Speed decodes the images around the current one ahead of time, so stepping through a folder is instant. Footprint keeps only what is on screen. Every image streams, and tiles leave memory once drawn. PNGs, TIFFs and non-progressive JPEGs are read again from the file at full size instead of kept. Nothing is written to disk unless streaming mode is on. Each step and zoom waits for decoding. |
| `sort`            | `a-z`, `z-a`, `newest`, `oldest` or `size`. The order of the images in a folder. Newest and oldest go by the last modified time. Size puts the smallest first. O switches the order while tinyiv runs and leaves the file unchanged.                                                                                                                                                                                            |

If the cache folder can't be written, tinyiv falls back to your user cache folder.

A streamed image uses any cache that already exists. The settings, the cache mode and the
system that made the cache do not matter, so a cache made on Linux opens on Windows and the
other way around. tinyiv looks for the cache in three places: the `cache` folder, the
`tinyiv-cache` folder next to the image and your user cache folder. Shift+R rebuilds the cache
of the streamed image on screen with the current settings.

## License

GPL-3.0-or-later.
