# Changelog

## Unreleased

### Added

- A config file, `tinyiv.conf`, with a `cache` setting for where tiles of huge images are kept
- A `cache_mode` setting: `small` keeps a huge PNG as places to begin decoding it again instead of every tile, about a third of the room

### Changed

- Large JPEGs with restart markers decode on several threads at once, about five times faster
- 8 bit TIFFs decode their strips or tiles on several threads at once through libtiff, about seven times faster, and stream their full resolution from the file
- A streamed JPEG with restart markers reads its full resolution from the file and stores only the smaller levels, a quarter of the room it took
- Tiles are stored as planes, red and blue less green, each row filtered up or by gradient, and tiles of 256 colours or fewer as a palette: 15 to 55% smaller, and as quick to read
- Tile folders tinyiv makes carry a CACHEDIR.TAG, so backup tools pass them by
- While tiles are written to disk, the status says which folder they go to
- Writing tiles watches the drive and stops before it fills, and on Linux 6.14 or newer keeps them out of the page cache
- Tile bands are cut and compressed while the next band decodes, making huge images about 15% faster to open the first time
- Streaming mode keeps the tiles of an image that fits in memory in memory, and writes only images too large for it to disk

## 1.2.0 - 2026-09-28

### Added

- HDR support
- Tile caching on disk for huge images that cannot fit in RAM
- A streaming mode that can be toggled with `s` that takes advantage of the tile caching. Enabled by default on large enough images

### Changed

- Zooming and panning are no longer bounded by the screen. The mouse can now be dragged infinitely

## 1.1.0 - 2026-09-27

### Added

- Rotate, flip, view modes and save

### Fixed

- Reduce memory consumption

## 1.0.0 - 2026-09-27

- First release
