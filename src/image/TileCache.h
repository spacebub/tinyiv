// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_IMAGE_TILECACHE_H
#define TIV_IMAGE_TILECACHE_H


#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "image/Bitmap.h"
#include "image/Tone.h"
#include "image/decode/Decode.h"

namespace tiv {
    // An image as a pyramid of compressed tiles, made by decoding top to bottom. One too large for
    // memory goes in a file on disk and is kept so reopening is instant, one that fits is held in
    // memory and never written. A file that can be decoded from partway, a JPEG with restart markers
    // or a TIFF by its strips or tiles, gives its full resolution itself, and only the smaller levels are stored, as GDAL's
    // external overviews do: https://gdal.org/en/stable/programs/gdaladdo.html
    // Tiles are unpacked on background threads as the view asks for them and cached, so memory
    // follows what the screen shows, whatever the image's size.
    class TileCache {

    public:
        // Pixels a side of a tile, the same as the GPU tiles, so one read fills one texture.
        static constexpr int TILE = 1024;

        // Tiles held in memory at most, as RGBA8.
        static constexpr std::size_t CACHE_BYTES = std::size_t{512} * 1024 * 1024;

        // The files kept in one folder at most, the least recently opened going first.
        static constexpr std::uintmax_t DISK_BYTES = std::uintmax_t{100} * 1024 * 1024 * 1024;

        // A level of this many tiles or fewer is read whole on opening and never let go, so
        // the coarsest is never missing.
        static constexpr int PINNED_TILES = 4;

        static constexpr int READERS = 2;

        enum class Store : std::uint8_t {
            Disk,
            Memory,
        };

        struct Level {
            int width = 0;
            int height = 0;
            int columns = 0;
            int rows = 0;
        };

        struct Key {
            int level = 0;
            int column = 0;
            int row = 0;
        };

        // The image file, read for the finest level.
        class Source {

        public:
            Source() = default;
            virtual ~Source() = default;

            Source(const Source &) = delete;
            Source(Source &&) = delete;
            Source &operator=(const Source &) = delete;
            Source &operator=(Source &&) = delete;

            [[nodiscard]] virtual int width() const = 0;
            [[nodiscard]] virtual int height() const = 0;
            [[nodiscard]] virtual bool alpha() const = 0;

            // Rows from top, count of them or to the bottom, in sRGB as wide as the image. The top is
            // a multiple of TILE.
            virtual bool read(int top, int count, Bitmap *out) const = 0;

            // What opens the same source again without a pass over the file.
            [[nodiscard]] virtual std::vector<std::uint64_t> index() const = 0;
        };

        // The folder the file's pyramid goes in: the one asked for, taken from the file's folder
        // when relative, else one beside the file. Where that cannot be made, the user's cache.
        [[nodiscard]] static std::filesystem::path location(const std::filesystem::path &file,
                                                            const std::filesystem::path &folder = {});

        // The pyramid made for the file as it is now and shown on the display, if there is one.
        // The display only tells pyramids of an HDR image apart.
        [[nodiscard]] static std::shared_ptr<TileCache> open(const std::filesystem::path &file,
                                                             const std::filesystem::path &folder = {},
                                                             const Tone::Display &display = {});

        // Decodes the file into a new pyramid and opens it. On disk it goes in the folder as
        // location() takes it. Progress goes from 0 to 1.
        [[nodiscard]] static std::shared_ptr<TileCache>
        build(const std::filesystem::path &file, const std::filesystem::path &folder, const Tone::Display &display,
              Store store, std::atomic<float> *progress, std::string *error = nullptr, Decode::Abort *abort = nullptr);

        ~TileCache();

        TileCache(const TileCache &) = delete;
        TileCache(TileCache &&) = delete;
        TileCache &operator=(const TileCache &) = delete;
        TileCache &operator=(TileCache &&) = delete;

        // As stored, of the finest level.
        [[nodiscard]] int width() const { return _levels.front().width; }
        [[nodiscard]] int height() const { return _levels.front().height; }

        // The finest first.
        [[nodiscard]] std::span<const Level> levels() const { return _levels; }

        [[nodiscard]] Bitmap::Encoding encoding() const { return _encoding; }

        // The tile when it is in memory, else null.
        [[nodiscard]] std::shared_ptr<const Bitmap> find(const Key &key) const;

        // The tiles to read, most wanted first. Replaces whatever was asked for before and is
        // not being read yet.
        void want(std::span<const Key> keys) const;

        // Called on a reader thread each time a tile arrives.
        void on_ready(std::function<void()> ready) const;

        // Lets go of every tile in memory but the pinned ones.
        void shrink() const;

        [[nodiscard]] std::size_t cached_bytes() const;

        // The compressed tiles held in memory, none for a pyramid on disk.
        [[nodiscard]] std::size_t stored_bytes() const { return _storedBytes; }

    private:
        struct Slot {
            std::shared_ptr<const Bitmap> bitmap;
            std::list<std::uint64_t>::iterator used;
            bool pinned = false;
        };

        // Where a tile lies in the file, compressed.
        struct Span {
            std::uint64_t offset = 0;
            std::uint64_t bytes = 0;
        };

        TileCache() = default;

        // The pyramid at the path, made for the file.
        bool load(const std::filesystem::path &path, const std::filesystem::path &file);
        // A row of the finest level's tiles from the source, all kept. The band is scratch for the
        // rows. Not under the lock.
        void read_source(int row, bool pinned, Bitmap &band);
        // Reads whole the levels of few enough tiles, so there is always something to draw.
        void pin();
        void start();
        void read_loop();
        // Under the lock, which it lets go of while it reads.
        void read_source_row(std::unique_lock<std::mutex> &hold, int row, Bitmap &band);
        // Tells whoever listens that a tile arrived, outside the lock.
        void announce(std::unique_lock<std::mutex> &hold) const;
        // Under the lock.
        void keep(std::uint64_t id, std::shared_ptr<const Bitmap> bitmap, bool pinned) const;
        [[nodiscard]] static std::uint64_t id_of(const Key &key);
        [[nodiscard]] static Key key_of(std::uint64_t id);

        // Empty for a pyramid in memory.
        std::filesystem::path _path;
        // The tiles of a pyramid in memory, where a Span's offset is the index.
        std::vector<std::vector<std::uint8_t>> _blocks;
        std::size_t _storedBytes = 0;
        int _channels = 4;
        Bitmap::Encoding _encoding = Bitmap::Encoding::Srgb;
        std::vector<Level> _levels;
        // Per level, where each tile lies in the file, row by row.
        std::vector<std::vector<Span>> _spans;
        // Gives the finest level when set, which is then not stored.
        std::unique_ptr<Source> _source;

        mutable std::mutex _guard;
        mutable std::condition_variable _wake;
        mutable std::unordered_map<std::uint64_t, Slot> _cache;
        // The most recently used first.
        mutable std::list<std::uint64_t> _used;
        mutable std::size_t _bytes = 0;
        mutable std::deque<std::uint64_t> _queue;
        mutable std::unordered_set<std::uint64_t> _reading;
        mutable std::function<void()> _ready;
        bool _running = true;
        std::vector<std::thread> _readers;
    };
}


#endif //TIV_IMAGE_TILECACHE_H
