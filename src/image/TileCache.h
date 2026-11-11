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
// An image as a pyramid of compressed tiles, unpacked on background threads as the view asks. Memory follows
// what the screen shows. One too large for memory stays on disk, so reopening is instant. A file that decodes from
// partway gives the finest level, and only the rest is stored: https://gdal.org/en/stable/programs/gdaladdo.html
class TileCache {

public:
    // Pixels a side of a tile, the same as the GPU tiles, so one read fills one texture.
    static constexpr int TILE = 1024;

    // Tiles held in memory at most, as RGBA8.
    static constexpr std::size_t CACHE_BYTES = std::size_t{512} * 1024 * 1024;
    // Tiles held in memory at most when lean, where each goes once on the GPU. Never under two rows
    // of the finest level, which the source reads whole.
    static constexpr std::size_t LEAN_CACHE_BYTES = std::size_t{64} * 1024 * 1024;

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
        [[nodiscard]] virtual Bitmap::Encoding encoding() const { return Bitmap::Encoding::Srgb; }

        // Count rows from top, or up to the bottom, as wide as the image and in the source encoding.
        // Top is a multiple of TILE.
        virtual bool read(int top, int count, Bitmap *out) const = 0;
        // The levels it reads itself, each half the one before, the finest first.
        [[nodiscard]] virtual int levels() const { return 1; }
        // As read(), for a level below levels(), in rows of that level.
        virtual bool read_level(const int level, const int top, const int count, Bitmap *out) const {
            return level == 0 && read(top, count, out);
        }

        // Rows of the image from y, as RGBA.
        using Take = std::function<bool(int y, int count, std::span<const std::uint8_t> pixels)>;
        // Keeps bytes with the tiles and says where they went.
        using Keep = std::function<std::uint64_t(std::span<const std::uint8_t> bytes)>;
        // Bytes kept before, read into the scratch or found where they lie. Empty when unreadable.
        using Fetch = std::function<std::span<const std::uint8_t>(
                std::uint64_t at, std::uint64_t bytes, std::vector<std::uint8_t> &scratch)>;

        // The whole image once, top to bottom, in bands of TILE rows, keeping what read() will want.
        virtual bool pass(const Take &take, const Keep &keep, const Decode::Abort *abort);

        // Says where to find what pass() kept. Called before the first read().
        virtual void found(const Fetch & /*fetch*/) {}

        // What opens the same source again without a pass over the file.
        [[nodiscard]] virtual std::vector<std::uint8_t> index() const = 0;
        // Cuts short a read() on another thread, as the tile cache closes.
        virtual void cancel() {}
    };

    // Roughly the memory a pyramid of the image takes when held in memory. It counts the compressed levels
    // below the finest. The finest counts too when no source reads the format from the file.
    [[nodiscard]] static std::uint64_t memory_bytes(Decode::Format kind, std::int64_t pixels);

    // The folder for the pyramid of the file: the folder asked for, relative to the folder of the file, else
    // one beside the file. Where that cannot be made, it is the user's cache. Without create, it is the first
    // folder that already exists, or empty.
    [[nodiscard]] static std::filesystem::path
    location(const std::filesystem::path &file, const std::filesystem::path &folder = {}, bool create = true);

    // The pyramid made for the file as it is now, in any folder location() may pick and either cache
    // mode. An HDR display falls back to one made for another headroom, then to an SDR one.
    // Lean holds as few tiles in memory as the view allows, with one reader.
    [[nodiscard]] static std::shared_ptr<TileCache> open(const std::filesystem::path &file,
                                                         const std::filesystem::path &folder = {},
                                                         const Tone::Display &display = Tone::Display(),
                                                         bool lean = false);

    // Decodes the file into a new pyramid and opens it. On disk it goes where location() picks. Progress is 0 to 1.
    // Small keeps the full size of a PNG as points to resume inflating. That takes a fraction of the room, but
    // panning there decodes whole bands of rows. Lean in memory rereads the finest level when the decoder streams.
    [[nodiscard]] static std::shared_ptr<TileCache> build(const std::filesystem::path &file,
                                                          const std::filesystem::path &folder,
                                                          const Tone::Display &display,
                                                          Store store,
                                                          bool small,
                                                          std::atomic<float> *progress,
                                                          std::string *error = nullptr,
                                                          Decode::Abort *abort = nullptr,
                                                          bool lean = false);

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

    // Made for another display than the one open() was given, so the tones differ from a fresh
    // decode until Shift+R rebuilds it.
    [[nodiscard]] bool stale() const { return _stale; }

    // The tile when it is in memory, else null.
    [[nodiscard]] std::shared_ptr<const Bitmap> find(const Key &key) const;

    // The tiles to read, most wanted first. Replaces whatever was asked for before and is
    // not being read yet.
    void want(std::span<const Key> keys) const;

    // Called on a reader thread each time a tile arrives.
    void on_ready(std::function<void()> ready) const;

    // Tiles asked for that have not arrived yet.
    [[nodiscard]] bool reading() const;

    // Lets go of every tile in memory but the pinned ones, and hands them back so the caller
    // chooses the thread that frees them.
    [[nodiscard]] std::vector<std::shared_ptr<const Bitmap>> shrink() const;

    // The tile is on the GPU, so a lean tile cache lets go of it unless pinned.
    void uploaded(const Key &key) const;

    [[nodiscard]] std::size_t cached_bytes() const;

    // The compressed tiles held in memory, none for a pyramid on disk.
    [[nodiscard]] std::size_t stored_bytes() const { return _storedBytes; }

    // Empty for one in memory.
    [[nodiscard]] const std::filesystem::path &path() const { return _path; }

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

    [[nodiscard]] static std::shared_ptr<TileCache> open_at(const std::filesystem::path &path,
                                                            const std::filesystem::path &file,
                                                            const Tone::Display &display,
                                                            bool lean);
    // The pyramid at the path, made for the file and shown on the display.
    bool load(const std::filesystem::path &path, const std::filesystem::path &file, const Tone::Display &display);
    // A row of the level's tiles from the source, all kept. The band is scratch for the rows. Not
    // under the lock.
    void read_source(int index, int row, bool pinned, Bitmap &band);
    // Tells the source where what it kept is found.
    void attach_source();
    // Reads whole the levels of few enough tiles, so there is always something to draw.
    void pin();
    void start();
    void read_loop();
    // Under the lock, which it lets go of while it reads.
    void read_source_row(std::unique_lock<std::mutex> &hold, int index, int row, Bitmap &band);
    // Tells whoever listens that a tile arrived, outside the lock.
    void announce(std::unique_lock<std::mutex> &hold) const;
    // Under the lock.
    void keep(std::uint64_t id, std::shared_ptr<const Bitmap> bitmap, bool pinned) const;
    // Bytes of tiles not pinned held at most.
    [[nodiscard]] std::size_t budget() const;
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
    // Levels the source gives, the finest first, none stored.
    int _sourced = 0;
    // Pins only the coarsest level, drops tiles once uploaded and reads on one thread.
    bool _lean = false;
    bool _stale = false;

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
