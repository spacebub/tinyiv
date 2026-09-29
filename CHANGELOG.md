# Changelog

## Unreleased

### Added

- A config file, `tinyiv.conf`, with a `cache` setting for where tiles of huge images are kept

### Changed

- Large JPEGs with restart markers decode on several threads at once, about five times faster
- 8 bit TIFFs decode their strips or tiles on several threads at once through libtiff, about seven times faster, and stream their full resolution from the file
- A streamed JPEG with restart markers reads its full resolution from the file and stores only the smaller levels, a quarter of the room it took
- Tiles are stored as planes, red and blue less green, each row filtered up or by gradient, and tiles of 256 colours or fewer as a palette: 15 to 55% smaller, and as quick to read
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
