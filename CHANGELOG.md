# Changelog

## 1.3.0 - 2026-09-29

### Added

- A config file, `tinyiv.conf`. Settings missing from it are added with their default values
- `cache` setting to choose where tile caches are kept
- `cache_mode` setting. `small` caches huge PNGs in about a third of the space, at the cost of slower panning at full size
- `streaming_mode` setting to start tinyiv with streaming mode on
- `sort` setting to choose the order a folder starts in
- `O` steps through the sort orders: name A to Z, name Z to A, newest first, oldest first and smallest first. The status bar always shows the order in use

### Changed

- Large JPEGs with restart markers decode on all cores
- 8 bit TIFFs decode on all cores through libtiff
- JPEGs with restart markers, most TIFFs and HEIF or AVIF grids are streamed straight from the file at full size, so only the smaller levels are cached. Their caches take about a quarter of the space they used to
- In streaming mode, images that fit in memory stay in memory instead of being written to disk
- Tile caches are 15 to 55% smaller and just as fast to read
- Huge images open faster the first time, as tiles are compressed while the next rows decode
- While tiles are being made, the folder they go to is shown
- Tile cache folders are tagged with CACHEDIR.TAG, so backup tools skip them
- Writing tile caches on Linux 6.14 or newer no longer pushes other files out of the page cache
- Pressing S on an image too large for memory says it is always streamed, instead of switching streaming mode off
- The status bar no longer repeats the tile progress shown in the middle of the window

### Fixed

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
