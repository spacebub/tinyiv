# Changelog

## Unreleased

### Added

- A config file, `tinyiv.conf`, with a `cache` setting for where tiles of huge images are kept

### Changed

- Large JPEGs with restart markers decode on several threads at once, about five times faster
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
