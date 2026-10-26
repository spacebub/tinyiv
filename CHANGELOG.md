# Changelog

## Unreleased

### Added

- `prevent_sleep` setting: `always`, `fullscreen` or `never`. Windowed tinyiv no longer prevents sleep unless set to `always`

## 1.3.3 - 2026-10-26

### Changed

- The images kept ready around the one on screen take a share of the memory the system has free, instead of a fixed amount, so they give way when another program needs it
- JPEG XL images decode with far less wasted CPU, and the one on screen comes up sooner
- Dependencies updated, libvips to 8.18.7

### Fixed

- Going fullscreen no longer freezes tinyiv on an assertion from the GPU debug layer
- Quitting, or switching streaming mode, no longer waits for the JPEG XL images being decoded ahead to finish

## 1.3.2 - 2026-10-24

### Changed

- Tile caches build with less wasted work, and huge PNGs in small mode build faster

### Fixed

- Rotated JPEG XL images no longer show sideways
- Error messages no longer get mixed up or blank when several images fail to open at once
- Native vips files show their format

## 1.3.1 - 2026-10-23

### Added

- `memory_mode` setting. `footprint` keeps only what the image on screen shows in memory, reading large PNGs, TIFFs and non-progressive JPEGs again from their file instead of holding them

### Changed

- Tile caches build faster

## 1.3.0 - 2026-09-30

### Added

- A config file, `tinyiv.conf`. Settings missing from it are added with their default values
- `cache` setting to choose where tile caches are kept
- `cache_mode` setting. `small` caches huge PNGs in about a third of the space, at the cost of slower panning at full size
- `streaming_mode` setting to start tinyiv with streaming mode on
- `persist_streams` setting to cache every streamed image on disk, not only those too large for memory
- `sort` setting to choose the order a folder starts in
- Shift+R rebuilds the tile cache of the streamed image on screen with the current settings
- `O` steps through the sort orders: name A to Z, name Z to A, newest first, oldest first and smallest first. The top bar always shows the order in use

### Changed

- A streamed image uses the tile cache that is already there, whatever the settings, the cache mode or the system it was made with, so caches made on Linux open on Windows and the other way around. tinyiv looks for it next to the image and in the user cache folder as well as in the `cache` folder
- A tile cache made in the other cache mode is no longer rebuilt, and one tinyiv can't read is left in place instead of deleted
- Large JPEGs with restart markers decode on all cores
- 8 bit TIFFs decode on all cores through libtiff
- JPEGs with restart markers, most TIFFs and HEIF or AVIF grids are streamed straight from the file at full size, so only the smaller levels are cached. Their caches take about a quarter of the space they used to
- In streaming mode, images that fit in memory stay in memory instead of being written to disk
- Tile caches are 15 to 55% smaller and just as fast to read
- Huge images open faster the first time, as tiles are compressed while the next rows decode
- While tiles are being made, the folder they go to is shown
- Tile cache folders are tagged with CACHEDIR.TAG, so backup tools skip them
- Writing tile caches on Linux 6.14 or newer no longer pushes other files out of the page cache
- Pressing S on an image too large for memory switches streaming mode without opening the image again, and says it is always streamed
- The status bar no longer repeats the tile progress shown in the middle of the window
- The file name and its place in the folder moved to a top bar, the name centred, with the sort order on its right
- The status bar shows whether streaming mode is on and, apart from it, whether the image on screen is too large for memory and always streams

### Fixed

- HDR images on Windows were drawn in SDR, as SDL's Direct3D 12 backend never enabled HDR output. tinyiv now draws through Vulkan
- Updating on Windows left behind the DLLs an older version shipped that are no longer needed, those are now cleaned
- Writing a tile cache could fill the drive. It now stops while 1 GB is still free

## 1.2.0 - 2026-09-28

### Added

- HDR support
- Tile caching on disk for images too large to fit in memory
- Streaming mode, toggled with `S`, which shows images from their tile cache. Images too large for memory always stream

### Changed

- Zooming and panning are no longer limited to the screen, and the mouse can be dragged as far as you like

## 1.1.0 - 2026-09-27

### Added

- Rotating, flipping, view modes and saving

### Fixed

- Lower memory use

## 1.0.0 - 2026-09-27

- First release
