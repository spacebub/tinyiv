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
    // An image too large for memory, as a pyramid of tiles in a file on disk, made once by decoding
    // top to bottom and kept so reopening is instant. Tiles are read on background threads as the
    // view asks for them and cached, so memory follows what the screen shows, whatever the image's size.
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

        // The folder the file's pyramid goes in: the one asked for, taken from the file's folder
        // when relative, else one beside the file. Where that cannot be made, the user's cache.
        [[nodiscard]] static std::filesystem::path location(const std::filesystem::path &file,
                                                            const std::filesystem::path &folder = {});

        // The pyramid made for the file as it is now and shown on the display, if there is one.
        // The display only tells pyramids of an HDR image apart.
        [[nodiscard]] static std::shared_ptr<TileCache> open(const std::filesystem::path &file,
                                                             const std::filesystem::path &folder = {},
                                                             const Tone::Display &display = {});

        // Decodes the file into a new pyramid on disk and opens it. Progress goes from 0 to 1.
        [[nodiscard]] static std::shared_ptr<TileCache>
        build(const std::filesystem::path &file, const std::filesystem::path &folder, const Tone::Display &display,
              std::atomic<float> *progress, std::string *error = nullptr, Decode::Abort *abort = nullptr);

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

        bool load(const std::filesystem::path &path);
        void start();
        void read_loop();
        // Under the lock.
        void keep(std::uint64_t id, std::shared_ptr<const Bitmap> bitmap, bool pinned) const;
        [[nodiscard]] static std::uint64_t id_of(const Key &key);
        [[nodiscard]] static Key key_of(std::uint64_t id);

        std::filesystem::path _path;
        int _channels = 4;
        Bitmap::Encoding _encoding = Bitmap::Encoding::Srgb;
        std::vector<Level> _levels;
        // Per level, where each tile lies in the file, row by row.
        std::vector<std::vector<Span>> _spans;

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
