// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <SDL3/SDL.h>
#include <zstd.h>

#include "image/Bitmap.h"
#include "image/Channels.h"
#include "image/Mapped.h"
#include "image/Pyramid.h"
#include "image/TileCache.h"
#include "image/TileCodec.h"
#include "image/decode/Decode.h"
#include "image/decode/HeifBands.h"
#include "image/decode/JpegBands.h"
#include "image/decode/PngBands.h"
#include "image/decode/Support.h"
#include "image/decode/TiffBands.h"

// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic): tiles are cut from rows by offset.
namespace tiv {
    namespace {
        constexpr std::array<char, 8> MAGIC{'T', 'I', 'V', 'T', 'I', 'L', 'E', 'S'};
        // The same layout with PQ tiles.
        constexpr std::array<char, 8> MAGIC_PQ{'T', 'I', 'V', 'P', 'Q', 'T', 'I', 'L'};
        constexpr std::uint32_t VERSION = 6;
        constexpr std::uint32_t MAX_LEVELS = 32;
        // Far past any image, and far enough from the limit of an int that sums of sides stay within it.
        constexpr std::uint32_t MAX_SIDE = std::uint32_t{1} << 30U;
        // Tiles asked for beyond this many are dropped, the view having moved on by the time they would come.
        constexpr std::size_t MAX_QUEUE = 64;
        // Left free on the drive beyond the pyramid, so making one never fills it.
        constexpr std::uintmax_t SPARE_DISK = std::uintmax_t{1024} * 1024 * 1024;
        constexpr std::size_t WRITE_BUFFER = std::size_t{16} * 1024 * 1024;
        constexpr std::string_view SUFFIX = ".tiles";
        constexpr std::string_view PART = ".part";
        constexpr int MAX_ENCODERS = 16;
        // Guesses the tiles at half their raw size, to know how much room to make before any are
        // written. Photographs come to half or more, so the drive is watched as they are written too.
        constexpr std::uint64_t EXPECTED_RATIO = 2;
        // How often, in bytes written, the drive's free room is looked at.
        constexpr std::uint64_t ROOM_CHECK_BYTES = std::uint64_t{256} * 1024 * 1024;
        // What of an image is read to tell it apart: its ends, where the headers and the last
        // of the data sit, and blocks spread evenly between them. A file this small is read whole.
        constexpr std::uint64_t EDGE_BYTES = std::uint64_t{64} * 1024;
        constexpr std::uint64_t SAMPLE_BYTES = std::uint64_t{4} * 1024;
        constexpr std::uint64_t SAMPLES = 32;
        constexpr std::uint64_t FNV_BASIS = 14695981039346656037ULL;

        // Little endian on every machine tinyiv runs on, so written as it lies in memory.
        struct Header {
            std::array<char, 8> magic{};
            std::uint32_t version = 0;
            std::uint32_t channels = 0;
            std::uint32_t tile = 0;
            std::uint32_t levels = 0;
            std::uint64_t sourceSize = 0;
            std::uint64_t sourceContents = 0;
            // Where the level sizes start, and after them every tile's offset.
            std::uint64_t index = 0;
            // Where the source's own index lies, when the finest level is read from the image
            // itself: a count, then that many words. Zero when it is stored.
            std::uint64_t source = 0;
        };

        static_assert(sizeof(Header) == 56);

        // Where a tile lies in the file, as the index keeps it.
        struct Placed {
            std::uint64_t offset = 0;
            std::uint64_t bytes = 0;
        };

        static_assert(sizeof(Placed) == 16);

        struct LevelSize {
            std::uint32_t width = 0;
            std::uint32_t height = 0;
        };

        void fail(std::string *error, const std::filesystem::path &file, const std::string_view why) {
            if (error != nullptr) {
                *error = file.string() + ": " + std::string(why);
            }
        }

        // FNV-1a, carried on from what came before.
        std::uint64_t hash(const std::span<const std::byte> data, std::uint64_t held = FNV_BASIS) {
            for (const std::byte c : data) {
                held ^= static_cast<std::uint8_t>(c);
                held *= 1099511628211ULL;
            }

            return held;
        }

        // A PQ word cannot lose its alpha a byte at a time.
        int channels_for(const bool alpha, const Bitmap::Encoding encoding) {
            return alpha || encoding == Bitmap::Encoding::Pq ? Bitmap::CHANNELS : 3;
        }

        std::array<char, 8> magic_for(const Bitmap::Encoding encoding) {
            return encoding == Bitmap::Encoding::Pq ? MAGIC_PQ : MAGIC;
        }

        std::u8string utf8(const std::string_view text) {
            return {text.begin(), text.end()};
        }

        std::u8string pq_tag(const Tone::Display &display) {
            return display.hdr() ? utf8(std::format("|pq|{:.2f}", display.headroom)) : std::u8string{};
        }

        std::filesystem::path from_utf8(const char *text) {
            return utf8(text);
        }

        std::filesystem::path user_cache() {
#ifdef _WIN32
            const char *local = SDL_getenv("LOCALAPPDATA");

            return local != nullptr ? from_utf8(local) / "tinyiv" / "cache" : std::filesystem::path{};
#else
            if (const char *xdg = SDL_getenv("XDG_CACHE_HOME"); xdg != nullptr && *xdg != '\0') {
                return from_utf8(xdg) / "tinyiv";
            }

            const char *home = SDL_getenv("HOME");

            return home != nullptr ? from_utf8(home) / ".cache" / "tinyiv" : std::filesystem::path{};
#endif
        }

        // Marks the folder as a cache, which backup tools such as borg, restic and tar's --exclude-caches
        // then pass by: https://bford.info/cachedir/
        void tag(const std::filesystem::path &dir) {
            const std::filesystem::path file = dir / "CACHEDIR.TAG";
            std::error_code failure;

            if (std::filesystem::exists(file, failure) || failure) {
                return;
            }

            std::ofstream out(file, std::ios::binary);

            out << "Signature: 8a477f597d28d172789f06886806bc55\n"
                   "# This file is a cache directory tag created by tinyiv.\n"
                   "# For information about cache directory tags, see https://bford.info/cachedir/\n";
        }

        // The folder there to take pyramids, made first when create is set. Only a folder tinyiv owns
        // or made is tagged, never one the user pointed it at, which may hold the images themselves.
        bool usable(const std::filesystem::path &dir, const bool create, const bool owned) {
            std::error_code failure;
            const bool made = create && std::filesystem::create_directories(dir, failure);

            if (failure || !std::filesystem::is_directory(dir, failure)) {
                return false;
            }
#ifndef _WIN32
            // A folder another user made on a shared drive is there but cannot take our files.
            if (::access(dir.c_str(), W_OK | X_OK) != 0) {
                return false;
            }
#endif
            if (create && (made || owned)) {
                tag(dir);
            }

            return true;
        }

        // Reads at an offset, so each reader thread has its own handle and none waits on another's seek.
        class Reader {

        public:
            explicit Reader(const std::filesystem::path &file)
#ifdef _WIN32
                // Shared for writing too, as the image it reads to identify may be open in an editor.
                : _handle(CreateFileW(file.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_RANDOM_ACCESS, nullptr))
#else
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic in C.
                : _fd(::open(file.c_str(), O_RDONLY | O_CLOEXEC))
#endif
            {
            }

            ~Reader() {
#ifdef _WIN32
                if (_handle != INVALID_HANDLE_VALUE) {
                    CloseHandle(_handle);
                }
#else
                if (_fd >= 0) {
                    ::close(_fd);
                }
#endif
            }

            Reader(const Reader &) = delete;
            Reader(Reader &&) = delete;
            Reader &operator=(const Reader &) = delete;
            Reader &operator=(Reader &&) = delete;

            [[nodiscard]] bool valid() const {
#ifdef _WIN32
                return _handle != INVALID_HANDLE_VALUE;
#else
                return _fd >= 0;
#endif
            }

            bool read(std::uint64_t offset, void *out, std::size_t bytes) const {
                auto *at = static_cast<std::uint8_t *>(out);

                while (bytes > 0) {
                    constexpr std::size_t MAX_CHUNK = std::size_t{1} << 30U;
                    const std::size_t chunk = std::min(bytes, MAX_CHUNK);
#ifdef _WIN32
                    OVERLAPPED where{};
                    DWORD got = 0;

                    where.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFU);
                    where.OffsetHigh = static_cast<DWORD>(offset >> 32U);

                    if (ReadFile(_handle, at, static_cast<DWORD>(chunk), &got, &where) == 0 || got == 0) {
                        return false;
                    }
#else
                    const ssize_t got = ::pread(_fd, at, chunk, static_cast<off_t>(offset));

                    if (got <= 0) {
                        return false;
                    }
#endif
                    at += got;
                    offset += static_cast<std::uint64_t>(got);
                    bytes -= static_cast<std::size_t>(got);
                }

                return true;
            }

        private:
#ifdef _WIN32
            HANDLE _handle = INVALID_HANDLE_VALUE;
#else
            int _fd = -1;
#endif
        };

        // Which image a pyramid was made from, by its contents alone, so a pyramid beside the image
        // serves it after any copy, move or rename, and on any system that mounts the drive.
        struct Identity {
            std::uint64_t size = 0;
            std::uint64_t contents = 0;
        };

        bool identify(const std::filesystem::path &file, Identity *out) {
            std::error_code failure;
            const std::uint64_t size = std::filesystem::file_size(file, failure);
            const Reader reader(file);

            if (failure || !reader.valid()) {
                return false;
            }

            std::vector<std::byte> block;
            std::uint64_t held = FNV_BASIS;

            const auto take = [&](const std::uint64_t offset, const std::uint64_t bytes) {
                block.resize(static_cast<std::size_t>(bytes));

                if (!reader.read(offset, block.data(), block.size())) {
                    return false;
                }

                held = hash(block, held);

                return true;
            };

            bool read = true;

            if (size <= (2 * EDGE_BYTES) + (SAMPLES * SAMPLE_BYTES)) {
                read = take(0, size);
            } else {
                const std::uint64_t between = size - (2 * EDGE_BYTES) - SAMPLE_BYTES;

                read = take(0, EDGE_BYTES);

                for (std::uint64_t i = 0; read && i < SAMPLES; ++i) {
                    read = take(EDGE_BYTES + (between * i / (SAMPLES - 1)), SAMPLE_BYTES);
                }

                read = read && take(size - EDGE_BYTES, EDGE_BYTES);
            }

            out->size = size;
            out->contents = held;

            return read;
        }

        // Named for the image's contents, so a changed image never finds an old pyramid, and for
        // the headroom an HDR one was mapped into.
        std::filesystem::path name_of(const Identity &identity, const Tone::Display &display) {
            const std::u8string text =
                    utf8(std::format("{}|{:016x}", identity.size, identity.contents)) + pq_tag(display);

            return std::format("{:016x}{}", hash(std::as_bytes(std::span(text))), SUFFIX);
        }

        // A pyramid of the same image made for another display's headroom, found by its header. Its
        // highlights roll off where that display's did, but it saves a pass over the whole image.
        std::filesystem::path made_for(const std::filesystem::path &dir, const Identity &identity,
                                       const Tone::Display &display) {
            std::error_code failure;

            for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(dir, failure)) {
                if (!entry.is_regular_file(failure) || entry.path().extension() != SUFFIX) {
                    continue;
                }

                const Reader reader(entry.path());
                Header header;

                if (!reader.valid() || !reader.read(0, &header, sizeof header)) {
                    continue;
                }

                const bool pq = header.magic == MAGIC_PQ;

                if (header.version == VERSION && header.sourceSize == identity.size
                    && header.sourceContents == identity.contents && pq == display.hdr()) {
                    return entry.path();
                }
            }

            return {};
        }

        // Where a Builder puts the tiles it makes.
        class Sink {

        public:
            Sink() = default;
            virtual ~Sink() = default;

            Sink(const Sink &) = delete;
            Sink(Sink &&) = delete;
            Sink &operator=(const Sink &) = delete;
            Sink &operator=(Sink &&) = delete;

            // Returns what finds the tile again, as a Span's offset.
            virtual std::uint64_t put(std::span<const std::uint8_t> bytes) = 0;
            [[nodiscard]] virtual bool ok() const = 0;
        };

        // Gathers writes into a large buffer and puts each down at its offset, as the Reader reads.
        // Stops once the drive holding the folder has no more than its spare room left.
        class Writer final : public Sink {

        public:
            Writer(const std::filesystem::path &file, std::filesystem::path dir)
                : _dir(std::move(dir)),
#ifdef _WIN32
                  _handle(CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN,
                                      nullptr))
#else
                  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): open() is variadic in C.
                  _fd(::open(file.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644))
#endif
            {
                _buffer.reserve(WRITE_BUFFER);
            }

            ~Writer() override { close(); }

            Writer(const Writer &) = delete;
            Writer(Writer &&) = delete;
            Writer &operator=(const Writer &) = delete;
            Writer &operator=(Writer &&) = delete;

            [[nodiscard]] bool ok() const override { return valid() && _ok; }
            [[nodiscard]] std::uint64_t at() const { return _at; }
            [[nodiscard]] bool full() const { return _full; }

            std::uint64_t put(const std::span<const std::uint8_t> bytes) override {
                const std::uint64_t offset = _at;

                write(bytes.data(), bytes.size());

                return offset;
            }

            void write(const void *data, const std::size_t bytes) {
                const auto *from = static_cast<const std::uint8_t *>(data);

                _at += bytes;

                if (_buffer.size() + bytes > WRITE_BUFFER) {
                    flush();
                }

                if (bytes >= WRITE_BUFFER) {
                    put(_flushed, from, bytes);
                    _flushed += bytes;
                } else {
                    _buffer.insert(_buffer.end(), from, from + bytes);
                }
            }

            // Writes over the start of the file, once the rest is down.
            void patch(const void *data, const std::size_t bytes) {
                flush();
                put(0, static_cast<const std::uint8_t *>(data), bytes);
            }

            bool close() {
                flush();
#ifdef _WIN32
                if (_handle != INVALID_HANDLE_VALUE) {
                    _ok = CloseHandle(_handle) != 0 && _ok;
                    _handle = INVALID_HANDLE_VALUE;
                }
#else
                if (_fd >= 0) {
                    _ok = ::close(_fd) == 0 && _ok;
                    _fd = -1;
                }
#endif
                return _ok;
            }

        private:
            [[nodiscard]] bool valid() const {
#ifdef _WIN32
                return _handle != INVALID_HANDLE_VALUE;
#else
                return _fd >= 0;
#endif
            }

#ifndef _WIN32
            // A pyramid of many gigabytes would push everything else out of the page cache, so from
            // Linux 6.14 its pages go once written: https://lwn.net/Articles/998783/
            // An older kernel or a file system without it takes plain writes.
            ssize_t write_at(const std::uint8_t *from, const std::size_t bytes, const std::uint64_t offset) {
#ifdef RWF_DONTCACHE
                if (_uncached) {
                    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast): iovec takes what it only reads, minus the const.
                    const iovec part{.iov_base = const_cast<std::uint8_t *>(from), .iov_len = bytes};
                    const ssize_t done = ::pwritev2(_fd, &part, 1, static_cast<off_t>(offset), RWF_DONTCACHE);

                    if (done >= 0 || (errno != EOPNOTSUPP && errno != EINVAL)) {
                        return done;
                    }

                    _uncached = false;
                }
#endif
                return ::pwrite(_fd, from, bytes, static_cast<off_t>(offset));
            }
#endif

            void flush() {
                put(_flushed, _buffer.data(), _buffer.size());
                _flushed += _buffer.size();
                _buffer.clear();

                if (_flushed - _checked >= ROOM_CHECK_BYTES) {
                    std::error_code failure;
                    const std::filesystem::space_info space = std::filesystem::space(_dir, failure);

                    _checked = _flushed;
                    _full = !failure && space.available < SPARE_DISK;
                    _ok = _ok && !_full;
                }
            }

            void put(std::uint64_t offset, const std::uint8_t *from, std::size_t bytes) {
                while (ok() && bytes > 0) {
                    constexpr std::size_t MAX_CHUNK = std::size_t{1} << 30U;
                    const std::size_t chunk = std::min(bytes, MAX_CHUNK);
#ifdef _WIN32
                    OVERLAPPED where{};
                    DWORD done = 0;

                    where.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFU);
                    where.OffsetHigh = static_cast<DWORD>(offset >> 32U);

                    if (WriteFile(_handle, from, static_cast<DWORD>(chunk), &done, &where) == 0 || done == 0) {
                        _ok = false;

                        return;
                    }
#else
                    const ssize_t done = write_at(from, chunk, offset);

                    if (done <= 0) {
                        _ok = false;

                        return;
                    }
#endif
                    from += done;
                    offset += static_cast<std::uint64_t>(done);
                    bytes -= static_cast<std::size_t>(done);
                }
            }

            std::filesystem::path _dir;
#ifdef _WIN32
            HANDLE _handle = INVALID_HANDLE_VALUE;
#else
            int _fd = -1;
#endif
            std::vector<std::uint8_t> _buffer;
            // Where the buffer goes when next put down.
            std::uint64_t _flushed = 0;
            std::uint64_t _at = 0;
            // Where the room left was last looked at.
            std::uint64_t _checked = 0;
            bool _ok = true;
            bool _full = false;
            bool _uncached = true;
        };

        // Keeps each tile as its own block, for a pyramid of an image that fits in memory, which
        // is never written to disk. Its offsets are the blocks' indices.
        class Blocks final : public Sink {

        public:
            std::uint64_t put(const std::span<const std::uint8_t> bytes) override {
                held.emplace_back(bytes.begin(), bytes.end());

                return held.size() - 1;
            }

            [[nodiscard]] bool ok() const override { return true; }

            std::vector<std::vector<std::uint8_t>> held;
        };

        std::vector<TileCache::Level> level_sizes(int width, int height) {
            std::vector<TileCache::Level> held;

            for (;;) {
                held.push_back({
                        .width = width,
                        .height = height,
                        .columns = (width + TileCache::TILE - 1) / TileCache::TILE,
                        .rows = (height + TileCache::TILE - 1) / TileCache::TILE,
                });

                if (width <= TileCache::TILE && height <= TileCache::TILE) {
                    return held;
                }

                width = (width + 1) / 2;
                height = (height + 1) / 2;
            }
        }

        std::uint64_t pyramid_bytes(const std::span<const TileCache::Level> levels, const int channels) {
            std::uint64_t total = 0;

            for (const TileCache::Level &level : levels) {
                total += static_cast<std::uint64_t>(level.width) * static_cast<std::uint64_t>(level.height)
                         * static_cast<std::uint64_t>(channels);
            }

            return total;
        }

        // Deletes the pyramids opened least recently, and whatever a build left unfinished,
        // until the new one fits under the cap and leaves the drive its spare room. One image
        // larger than the cap on its own still gets made, as long as the drive has the room.
        bool make_room(const std::filesystem::path &dir, const std::uint64_t wanted) {
            struct Kept {
                std::filesystem::path file;
                std::filesystem::file_time_type time;
                std::uintmax_t size = 0;
            };

            std::vector<Kept> kept;
            std::uintmax_t total = 0;
            std::error_code failure;

            for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator(dir, failure)) {
                if (!entry.is_regular_file(failure)) {
                    continue;
                }

                const std::filesystem::path extension = entry.path().extension();

                if (extension == PART) {
                    std::filesystem::remove(entry.path(), failure);
                } else if (extension == SUFFIX) {
                    kept.push_back({
                            .file = entry.path(),
                            .time = entry.last_write_time(failure),
                            .size = entry.file_size(failure),
                    });
                    total += kept.back().size;
                }
            }

            std::ranges::sort(kept, {}, &Kept::time);

            const auto room = [&] {
                const std::filesystem::space_info space = std::filesystem::space(dir, failure);

                return failure ? std::uintmax_t{0} : space.available;
            };

            for (const Kept &old : kept) {
                if (total + wanted <= TileCache::DISK_BYTES && room() >= wanted + SPARE_DISK) {
                    break;
                }

                if (std::filesystem::remove(old.file, failure)) {
                    total -= old.size;
                }
            }

            return room() >= wanted + SPARE_DISK;
        }

        // One tile being made ready to write.
        struct Encoded {
            TileCodec::Encoder encoder;
            std::span<const std::uint8_t> packed;
        };

        // Cuts each band into tiles as it arrives and halves it into the next level, so every
        // level is written in one pass with only a band of each held. The tiles of a band
        // compress on as many threads as the machine has, and are written in order.
        class Builder {

        public:
            // Without keeping the finest level, the pyramid stores only the levels below it.
            Builder(Sink &out, const std::vector<TileCache::Level> &levels, const int channels,
                    const Bitmap::Encoding encoding, const bool keepFinest = true)
                : _out(&out), _channels(channels), _encoding(encoding), _keepFinest(keepFinest),
                  _slots(static_cast<std::size_t>(
                          std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, MAX_ENCODERS))) {
                for (const TileCache::Level &level : levels) {
                    State state;
                    const std::size_t pitch = static_cast<std::size_t>(level.width) * Bitmap::CHANNELS;

                    state.level = level;
                    state.spans.resize(static_cast<std::size_t>(level.columns) * static_cast<std::size_t>(level.rows));
                    state.held.resize(pitch);
                    // The finest level arrives in whole bands, so only the rest gather them.
                    if (!_states.empty()) {
                        state.band.resize(pitch * TileCache::TILE);
                    }

                    _states.push_back(std::move(state));
                }

                for (std::size_t i = 0; i + 1 < _states.size(); ++i) {
                    _states.at(i).half.resize(static_cast<std::size_t>(_states.at(i + 1).level.width)
                                              * Bitmap::CHANNELS);
                }
            }

            // Rows of the finest level, starting on a tile row. They are copied and cut into tiles on a
            // thread of their own, so the decoder goes on to the next band meanwhile. The copy costs a
            // few milliseconds a band, the overlap the whole time the tiles take.
            bool band(const int rows, const std::span<const std::uint8_t> pixels) {
                settle();

                if (!_out->ok() || _failed) {
                    return false;
                }

                _copy.assign(pixels.begin(), pixels.end());
                _cutting = std::jthread([this, rows] { cut(rows, _copy.data()); });

                return true;
            }

            // Every level whole.
            [[nodiscard]] bool finished() {
                settle();

                return !_failed && _out->ok() && std::ranges::all_of(_states, [](const State &state) {
                    return state.received == state.level.height;
                });
            }

            // Where each tile went, after the level sizes.
            void write_index(Writer &out) const {
                for (const State &state : _states) {
                    const LevelSize size{
                            .width = static_cast<std::uint32_t>(state.level.width),
                            .height = static_cast<std::uint32_t>(state.level.height),
                    };

                    out.write(&size, sizeof size);
                }

                for (const State &state : _states) {
                    out.write(state.spans.data(), state.spans.size() * sizeof(Placed));
                }
            }

            [[nodiscard]] std::vector<std::vector<Placed>> spans() const {
                std::vector<std::vector<Placed>> held;

                held.reserve(_states.size());

                for (const State &state : _states) {
                    held.push_back(state.spans);
                }

                return held;
            }

        private:
            struct State {
                TileCache::Level level;
                // RGBA8 rows of the tile row being gathered.
                std::vector<std::uint8_t> band;
                int filled = 0;
                int tileRow = 0;
                int received = 0;
                // Rows handed to feed(), which pairs them into the next level.
                int paired = 0;
                // The first of a pair of rows waiting for its second, to halve into the next level.
                std::vector<std::uint8_t> held;
                bool holding = false;
                std::vector<std::uint8_t> half;
                std::vector<Placed> spans;
            };

            // Waits for the band before to be cut.
            void settle() {
                if (_cutting.joinable()) {
                    _cutting.join();
                }
            }

            void cut(const int rows, const std::uint8_t *pixels) {
                State &finest = _states.front();
                const std::size_t pitch = static_cast<std::size_t>(finest.level.width) * Bitmap::CHANNELS;

                if (_keepFinest) {
                    write_band(finest, pixels, rows);
                } else {
                    ++finest.tileRow;
                    finest.received += rows;
                }

                for (int y = 0; y < rows; ++y) {
                    feed(0, pixels + (pitch * static_cast<std::size_t>(y)));
                }
            }

            void encode(const TileCache::Level &level, const std::uint8_t *rows, const int count, const int column,
                        Encoded &slot) const {
                const int width = std::min(TileCache::TILE, level.width - (column * TileCache::TILE));
                const std::size_t pitch = static_cast<std::size_t>(level.width) * Bitmap::CHANNELS;
                const std::size_t from = static_cast<std::size_t>(column) * TileCache::TILE * Bitmap::CHANNELS;

                slot.packed = slot.encoder.encode(rows + from, pitch, width, count, _channels, _encoding);
            }

            void write_band(State &state, const std::uint8_t *rows, const int count) {
                const TileCache::Level &level = state.level;
                const auto workers = static_cast<int>(_slots.size());

                for (int first = 0; first < level.columns; first += workers) {
                    const int last = std::min(first + workers, level.columns);

                    {
                        std::vector<std::jthread> threads;

                        for (int column = first + 1; column < last; ++column) {
                            threads.emplace_back([&, column] {
                                encode(level, rows, count, column, _slots.at(static_cast<std::size_t>(column - first)));
                            });
                        }

                        encode(level, rows, count, first, _slots.front());
                    }

                    for (int column = first; column < last; ++column) {
                        const Encoded &slot = _slots.at(static_cast<std::size_t>(column - first));

                        _failed = _failed || slot.packed.empty();
                        state.spans.at(
                                (static_cast<std::size_t>(state.tileRow) * static_cast<std::size_t>(level.columns))
                                + static_cast<std::size_t>(column)) = {
                                .offset = _out->put(slot.packed),
                                .bytes = slot.packed.size(),
                        };
                    }
                }

                ++state.tileRow;
                state.received += count;
            }

            // A row of a level below the finest.
            void gather(State &state, const std::uint8_t *row) {
                const std::size_t pitch = static_cast<std::size_t>(state.level.width) * Bitmap::CHANNELS;

                std::memcpy(state.band.data() + (pitch * static_cast<std::size_t>(state.filled)), row, pitch);
                ++state.filled;

                if (state.filled == TileCache::TILE || state.received + state.filled == state.level.height) {
                    write_band(state, state.band.data(), state.filled);
                    state.filled = 0;
                }
            }

            // Pairs the level's rows into the next one, and on down as long as each pair completes
            // a row. An odd last row pairs with itself.
            void feed(std::size_t index, const std::uint8_t *row) {
                for (; index + 1 < _states.size(); ++index) {
                    State &state = _states.at(index);
                    const std::size_t pitch = static_cast<std::size_t>(state.level.width) * Bitmap::CHANNELS;

                    ++state.paired;

                    if (state.holding) {
                        Pyramid::halve_row(state.held.data(), row, state.half.data(), state.level.width,
                                           Pyramid::Kernel::Auto, _encoding);
                    } else if (state.paired == state.level.height) {
                        Pyramid::halve_row(row, row, state.half.data(), state.level.width, Pyramid::Kernel::Auto,
                                           _encoding);
                    } else {
                        std::memcpy(state.held.data(), row, pitch);
                        state.holding = true;

                        return;
                    }

                    state.holding = false;
                    row = state.half.data();
                    gather(_states.at(index + 1), row);
                }
            }

            Sink *_out;
            int _channels;
            Bitmap::Encoding _encoding;
            bool _keepFinest;
            std::vector<State> _states;
            std::vector<Encoded> _slots;
            bool _failed = false;
            // The band being cut, and the thread cutting it, last so it is joined before the rest goes.
            std::vector<std::uint8_t> _copy;
            std::jthread _cutting;
        };

        bool sound(const Header &header, const std::uintmax_t size) {
            const bool known = header.magic == MAGIC || header.magic == MAGIC_PQ;
            const bool channels = header.channels == 3 || header.channels == Bitmap::CHANNELS;
            const bool levels = header.levels > 0 && header.levels <= MAX_LEVELS;

            return known && header.version == VERSION && header.tile == TileCache::TILE && channels && levels
                   && header.index < size;
        }

        // Every tile has to lie before the index, and be no larger than it could compress to.
        bool tiles_fit(const std::span<const Placed> placed, const TileCache::Level &level, const Header &header) {
            std::size_t i = 0;

            for (const Placed &tile : placed) {
                const int column = static_cast<int>(i % static_cast<std::size_t>(level.columns));
                const int row = static_cast<int>(i / static_cast<std::size_t>(level.columns));
                const auto width =
                        static_cast<std::uint64_t>(std::min(TileCache::TILE, level.width - (column * TileCache::TILE)));
                const auto height =
                        static_cast<std::uint64_t>(std::min(TileCache::TILE, level.height - (row * TileCache::TILE)));

                if (tile.offset + tile.bytes > header.index
                    || tile.bytes > TileCodec::bound(static_cast<int>(width), static_cast<int>(height),
                                                     static_cast<int>(header.channels))) {
                    return false;
                }

                ++i;
            }

            return true;
        }

        // What a reader thread keeps between tiles.
        struct Decoder {
            TileCodec::Decoder codec;
            std::vector<std::uint8_t> packed;
        };

        // A tile's packed bytes, read into the scratch from disk, or found in memory when there is no
        // reader. Empty when unreadable.
        std::span<const std::uint8_t> fetch(const Reader *reader, const std::vector<std::vector<std::uint8_t>> &blocks,
                                            const std::uint64_t offset, const std::uint64_t bytes,
                                            std::vector<std::uint8_t> &scratch) {
            if (reader == nullptr) {
                return offset < blocks.size() ? std::span<const std::uint8_t>(blocks.at(offset))
                                              : std::span<const std::uint8_t>();
            }

            scratch.resize(static_cast<std::size_t>(bytes));

            return reader->read(offset, scratch.data(), scratch.size()) ? std::span<const std::uint8_t>(scratch)
                                                                        : std::span<const std::uint8_t>();
        }

        // The packed bytes are empty when they could not be read.
        Bitmap read_tile(const std::span<const std::uint8_t> packed, const TileCache::Level &level, const int channels,
                         const Bitmap::Encoding encoding, const TileCache::Key &key, Decoder &decoder) {
            const int width = std::min(TileCache::TILE, level.width - (key.column * TileCache::TILE));
            const int height = std::min(TileCache::TILE, level.height - (key.row * TileCache::TILE));
            Bitmap tile = Bitmap::allocate(width, height, encoding);

            // A tile that cannot be read shows as a hole, else the view would ask for it forever.
            if (packed.empty() || !decoder.codec.decode(packed, channels, encoding, &tile)) {
                std::memset(tile.data(), 0, tile.bytes());
            }

            return tile;
        }

        // A band of the finest level read from the source is waited for by the view, so it takes every
        // thread the machine has, not the few a decode in the background keeps to.
        int read_threads() {
            static const int held = std::max(1, static_cast<int>(std::thread::hardware_concurrency()));

            return held;
        }

        // A JPEG whose restart markers let any tile row decode on its own, read as it is needed rather
        // than mapped, since a mapping of a file cut short under it faults.
        class JpegSource final : public TileCache::Source {

        public:
            JpegSource(const std::filesystem::path &file, Decode::JpegBands bands)
                : _reader(file), _bands(std::move(bands)) {}

            [[nodiscard]] bool valid() const { return _reader.valid(); }
            [[nodiscard]] int width() const override { return _bands.width(); }
            [[nodiscard]] int height() const override { return _bands.height(); }
            [[nodiscard]] bool alpha() const override { return false; }

            bool read(const int top, const int count, Bitmap *out) const override {
                const Decode::JpegBands::Fetch fetch = [this](const std::uint64_t offset, const std::uint64_t bytes,
                                                              std::vector<std::uint8_t> &scratch) {
                    scratch.resize(static_cast<std::size_t>(bytes));

                    return _reader.read(offset, scratch.data(), scratch.size()) ? std::span<const std::uint8_t>(scratch)
                                                                                : std::span<const std::uint8_t>();
                };

                return _bands.decode(fetch, top, count, 8, out, read_threads(), nullptr);
            }

            [[nodiscard]] std::vector<std::uint8_t> index() const override {
                const std::span<const std::uint64_t> starts = _bands.starts();
                std::vector<std::uint8_t> held(starts.size_bytes());

                std::memcpy(held.data(), starts.data(), held.size());

                return held;
            }

        private:
            Reader _reader;
            Decode::JpegBands _bands;
        };

        class TiffSource final : public TileCache::Source {

        public:
            explicit TiffSource(std::unique_ptr<Decode::TiffBands> bands) : _bands(std::move(bands)) {}

            [[nodiscard]] int width() const override { return _bands->width(); }
            [[nodiscard]] int height() const override { return _bands->height(); }
            [[nodiscard]] bool alpha() const override { return _bands->alpha(); }

            bool read(const int top, const int count, Bitmap *out) const override {
                return _bands->decode(top, count, out, read_threads(), nullptr);
            }

            // Where the strips lie is in the file already.
            [[nodiscard]] std::vector<std::uint8_t> index() const override { return {}; }

        private:
            std::unique_ptr<Decode::TiffBands> _bands;
        };

        class HeifSource final : public TileCache::Source {

        public:
            explicit HeifSource(std::unique_ptr<Decode::HeifBands> bands) : _bands(std::move(bands)) {}

            [[nodiscard]] int width() const override { return _bands->width(); }
            [[nodiscard]] int height() const override { return _bands->height(); }
            [[nodiscard]] bool alpha() const override { return false; }

            bool read(const int top, const int count, Bitmap *out) const override {
                return _bands->decode(top, count, out, read_threads(), nullptr);
            }

            // Where the tiles lie is in the file already.
            [[nodiscard]] std::vector<std::uint8_t> index() const override { return {}; }

        private:
            std::unique_ptr<Decode::HeifBands> _bands;
        };

        // A PNG's checkpoints, each with its window and row kept compressed with the tiles.
        class PngSource final : public TileCache::Source {

        public:
            explicit PngSource(std::unique_ptr<Decode::PngBands> bands) : _bands(std::move(bands)) {}

            [[nodiscard]] int width() const override { return _bands->width(); }
            [[nodiscard]] int height() const override { return _bands->height(); }
            [[nodiscard]] bool alpha() const override { return _bands->alpha(); }
            [[nodiscard]] Bitmap::Encoding encoding() const override { return _bands->encoding(); }

            bool pass(const Take &take, const Keep &keep, const Decode::Abort *abort) override {
                const std::unique_ptr<ZSTD_CCtx, TileCodec::ContextFree> context(ZSTD_createCCtx());
                std::vector<std::uint8_t> packed;

                _kept.clear();

                const Decode::PngBands::Keep keepState = [&](const std::size_t checkpoint,
                                                             const std::span<const std::uint8_t> state) {
                    packed.resize(ZSTD_compressBound(state.size()));

                    const std::size_t made = ZSTD_compressCCtx(context.get(), packed.data(), packed.size(),
                                                               state.data(), state.size(), STATE_LEVEL);

                    if (ZSTD_isError(made) != 0) {
                        return false;
                    }

                    _kept.resize(std::max(_kept.size(), checkpoint + 1));
                    _kept.at(checkpoint) = {.at = keep(std::span(packed).first(made)), .bytes = made};

                    return true;
                };

                return context != nullptr && _bands->scan(TileCache::TILE, take, keepState, abort);
            }

            void found(const Fetch &fetch) override { _fetch = fetch; }

            bool read(const int top, const int count, Bitmap *out) const override {
                const Decode::PngBands::Recall recall = [this](const std::size_t checkpoint,
                                                               std::vector<std::uint8_t> &state) {
                    std::vector<std::uint8_t> scratch;

                    if (checkpoint >= _kept.size() || !_fetch) {
                        return false;
                    }

                    const std::span<const std::uint8_t> packed =
                            _fetch(_kept.at(checkpoint).at, _kept.at(checkpoint).bytes, scratch);
                    const unsigned long long size = ZSTD_getFrameContentSize(packed.data(), packed.size());

                    if (packed.empty() || size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN
                        || size > MAX_STATE) {
                        return false;
                    }

                    state.resize(static_cast<std::size_t>(size));

                    return ZSTD_decompress(state.data(), state.size(), packed.data(), packed.size()) == size;
                };

                return _bands->decode(top, count, out, read_threads(), recall);
            }

            // Each checkpoint, then where its state went.
            [[nodiscard]] std::vector<std::uint8_t> index() const override {
                std::vector<std::uint8_t> held;
                const std::span<const Decode::PngBands::Checkpoint> points = _bands->checkpoints();

                for (std::size_t i = 0; i < points.size() && i < _kept.size(); ++i) {
                    const Stored stored{
                            .point = points[i],
                            .at = _kept.at(i).at,
                            .bytes = _kept.at(i).bytes,
                    };
                    const std::size_t end = held.size();

                    held.resize(end + sizeof stored);
                    std::memcpy(held.data() + end, &stored, sizeof stored);
                }

                return held;
            }

            // The index() of an earlier pass.
            bool adopt(const std::span<const std::uint8_t> index) {
                if (index.empty() || index.size() % sizeof(Stored) != 0) {
                    return false;
                }

                std::vector<Decode::PngBands::Checkpoint> points(index.size() / sizeof(Stored));

                _kept.resize(points.size());

                for (std::size_t i = 0; i < points.size(); ++i) {
                    Stored stored;

                    std::memcpy(&stored, index.data() + (i * sizeof(Stored)), sizeof stored);
                    points.at(i) = stored.point;
                    _kept.at(i) = {.at = stored.at, .bytes = stored.bytes};
                }

                _bands->adopt(std::move(points));

                return true;
            }

        private:
            // zstd's fastest, the state being read far more often than made.
            static constexpr int STATE_LEVEL = 1;
            // A window and a row of the widest PNG a side can hold, far past any real one.
            static constexpr unsigned long long MAX_STATE = 1ULL << 32U;

            struct Kept {
                std::uint64_t at = 0;
                std::uint64_t bytes = 0;
            };

            // As index() writes each checkpoint.
            struct Stored {
                Decode::PngBands::Checkpoint point;
                std::uint64_t at = 0;
                std::uint64_t bytes = 0;
            };

            std::unique_ptr<Decode::PngBands> _bands;
            std::vector<Kept> _kept;
            Fetch _fetch;
        };

        // The file as the source of its finest level, when its format allows. The index kept from
        // before spares the pass over the file. A PNG only gives its own when asked for small.
        std::unique_ptr<TileCache::Source> open_source(const std::filesystem::path &file,
                                                       const std::vector<std::uint8_t> *kept, const bool small,
                                                       const Tone::Display &display) {
            constexpr std::size_t HEAD = 64;
            const Reader head(file);
            std::array<std::uint8_t, HEAD> start{};
            std::error_code failure;
            const std::uintmax_t size = std::filesystem::file_size(file, failure);

            if (failure || !head.valid() || !head.read(0, start.data(), std::min<std::uintmax_t>(size, HEAD))) {
                return nullptr;
            }

            const Decode::Format kind = Decode::sniff(std::span(start).first(std::min<std::size_t>(size, HEAD)));

            if (kind == Decode::Format::Tiff) {
                std::unique_ptr<Decode::TiffBands> strips = Decode::TiffBands::open(file);

                return strips != nullptr ? std::make_unique<TiffSource>(std::move(strips)) : nullptr;
            }

            if (kind == Decode::Format::Heif) {
                std::unique_ptr<Decode::HeifBands> tiles = Decode::HeifBands::open(file);

                return tiles != nullptr ? std::make_unique<HeifSource>(std::move(tiles)) : nullptr;
            }

            if (kind == Decode::Format::Png && (small || kept != nullptr)) {
                std::unique_ptr<Decode::PngBands> rows = Decode::PngBands::open(file, display);

                if (rows == nullptr) {
                    return nullptr;
                }

                auto source = std::make_unique<PngSource>(std::move(rows));

                return kept == nullptr || source->adopt(*kept) ? std::move(source) : nullptr;
            }

            Mapped mapped;
            Decode::JpegBands bands;

            // Finding the markers reads the whole file, taking the kept ones only its ends.
            if (kind != Decode::Format::Jpeg
                || !Mapped::open(file, &mapped, nullptr,
                                 kept != nullptr ? Mapped::Use::Scattered : Mapped::Use::Through)) {
                return nullptr;
            }

            std::vector<std::uint64_t> starts;

            if (kept != nullptr) {
                starts.resize(kept->size() / sizeof(std::uint64_t));
                std::memcpy(starts.data(), kept->data(), starts.size() * sizeof(std::uint64_t));
            }

            if (kept != nullptr ? !Decode::JpegBands::adopt(mapped.data(), std::move(starts), &bands)
                                : !Decode::JpegBands::index(mapped.data(), &bands)) {
                return nullptr;
            }

            auto source = std::make_unique<JpegSource>(file, std::move(bands));

            return source->valid() ? std::move(source) : nullptr;
        }

        // A sink two threads put into, the Builder cutting its bands and the source keeping its state.
        class Locked final : public Sink {

        public:
            explicit Locked(Sink &inner) : _inner(&inner) {}

            std::uint64_t put(const std::span<const std::uint8_t> bytes) override {
                const std::scoped_lock hold(_guard);

                return _inner->put(bytes);
            }

            [[nodiscard]] bool ok() const override {
                const std::scoped_lock hold(_guard);

                return _inner->ok();
            }

        private:
            Sink *_inner;
            mutable std::mutex _guard;
        };

        // One pass over the file, cutting it into a pyramid of tiles through a Builder. The sink the
        // tiles go to is asked for once the image's size is known, and null stops the pass.
        struct Pass {
            std::vector<TileCache::Level> levels;
            int channels = Bitmap::CHANNELS;
            Bitmap::Encoding encoding = Bitmap::Encoding::Srgb;
            // Before the builder, so it goes after it.
            std::unique_ptr<Locked> locked;
            std::unique_ptr<Builder> builder;
            // Why the pass stopped, when it was not the decoder's doing.
            std::string why;
        };

        // The finest level read band by band from the source, and only the levels below it made.
        bool run_source_pass(const std::filesystem::path &file, TileCache::Source &source, std::atomic<float> *progress,
                             std::string *error, Decode::Abort *abort, const std::function<Sink *(const Pass &)> &ready,
                             Pass *made) {
            const int height = source.height();

            made->levels = level_sizes(source.width(), height);
            made->channels = channels_for(source.alpha(), source.encoding());
            made->encoding = source.encoding();

            Sink *sink = ready(*made);

            if (sink == nullptr) {
                fail(error, file, made->why);

                return false;
            }

            made->locked = std::make_unique<Locked>(*sink);
            made->builder =
                    std::make_unique<Builder>(*made->locked, made->levels, made->channels, made->encoding, false);

            const TileCache::Source::Take take = [&](const int y, const int rows,
                                                     const std::span<const std::uint8_t> pixels) {
                if (!made->builder->band(rows, pixels)) {
                    return false;
                }

                if (progress != nullptr) {
                    progress->store(static_cast<float>(y + rows) / static_cast<float>(height),
                                    std::memory_order_relaxed);
                }

                return true;
            };

            const TileCache::Source::Keep keep = [&](const std::span<const std::uint8_t> bytes) {
                return made->locked->put(bytes);
            };

            if (!source.pass(take, keep, abort)) {
                fail(error, file, Decode::aborted(abort) ? "aborted" : "could not read the image's rows");

                return false;
            }

            if (!made->builder->finished()) {
                fail(error, file, "could not store the tiles");

                return false;
            }

            return true;
        }

        bool run_pass(const std::filesystem::path &file, const Tone::Display &display, std::atomic<float> *progress,
                      std::string *error, Decode::Abort *abort, const std::function<Sink *(const Pass &)> &ready,
                      Pass *made) {
            int total = 1;

            const auto begin = [&](const int width, const int height, const bool alpha,
                                   const Bitmap::Encoding encoding) {
                made->levels = level_sizes(width, height);
                made->channels = channels_for(alpha, encoding);
                made->encoding = encoding;
                total = height;

                Sink *sink = ready(*made);

                if (sink == nullptr) {
                    return false;
                }

                made->builder = std::make_unique<Builder>(*sink, made->levels, made->channels, encoding);

                return true;
            };

            const auto take = [&](const int y, const int rows, const std::span<const std::uint8_t> pixels) {
                if (!made->builder->band(rows, pixels)) {
                    made->why = "could not store the tiles";

                    return false;
                }

                if (progress != nullptr) {
                    progress->store(static_cast<float>(y + rows) / static_cast<float>(total),
                                    std::memory_order_relaxed);
                }

                return true;
            };

            bool done = Decode::stream(file, TileCache::TILE, begin, take, error, abort, display);

            if (done && !made->builder->finished()) {
                done = false;
                made->why = "could not store the tiles";
            }

            if (!done && !made->why.empty()) {
                fail(error, file, made->why);
            }

            return done;
        }

        // Makes the file's pyramid in the folder, under a temporary name until it is whole.
        bool write_pyramid(const std::filesystem::path &file, const std::filesystem::path &dir,
                           const Tone::Display &display, TileCache::Source *source, std::atomic<float> *progress,
                           std::string *error, Decode::Abort *abort) {
            Identity identity;

            if (!identify(file, &identity)) {
                fail(error, file, "unreadable");

                return false;
            }

            const std::filesystem::path finished = dir / name_of(identity, display);
            std::filesystem::path part = finished;

            part += PART;

            Header header;
            std::unique_ptr<Writer> out;
            Pass made;

            header.version = VERSION;
            header.tile = TileCache::TILE;
            header.sourceSize = identity.size;
            header.sourceContents = identity.contents;

            const auto ready = [&](const Pass &pass) -> Sink * {
                // Read from the source, the finest level takes no room.
                const std::span<const TileCache::Level> stored =
                        source != nullptr ? std::span(pass.levels).subspan(1) : std::span(pass.levels);
                const std::uint64_t bytes = pyramid_bytes(stored, pass.channels) / EXPECTED_RATIO;

                if (!make_room(dir, bytes)) {
                    made.why = std::format("needs {:.1f} GB free on the drive for tiles",
                                           static_cast<double>(bytes + SPARE_DISK) / 1e9);

                    return nullptr;
                }

                out = std::make_unique<Writer>(part, dir);

                if (!out->ok()) {
                    made.why = "could not create the tile file";

                    return nullptr;
                }

                header.magic = magic_for(pass.encoding);
                header.channels = static_cast<std::uint32_t>(pass.channels);
                header.levels = static_cast<std::uint32_t>(pass.levels.size());
                // Written again once the index is down.
                out->write(&header, sizeof header);

                return out.get();
            };

            bool done = source != nullptr ? run_source_pass(file, *source, progress, error, abort, ready, &made)
                                          : run_pass(file, display, progress, error, abort, ready, &made);

            if (!done && out != nullptr && out->full()) {
                fail(error, file,
                     std::format("the drive filled up, {:.1f} GB has to stay free",
                                 static_cast<double>(SPARE_DISK) / 1e9));
            }

            if (done) {
                header.index = out->at();
                made.builder->write_index(*out);

                if (source != nullptr) {
                    const std::vector<std::uint8_t> kept = source->index();
                    const std::uint64_t count = kept.size();

                    header.source = out->at();
                    out->write(&count, sizeof count);
                    out->write(kept.data(), kept.size());
                }

                out->patch(&header, sizeof header);
                done = out->close();

                if (!done) {
                    fail(error, file, "could not write the tile file");
                }
            }

            if (out != nullptr) {
                out->close();
            }

            std::error_code failure;

            if (done) {
                std::filesystem::rename(part, finished, failure);
                done = !failure;

                if (!done) {
                    fail(error, file, "could not keep the tile file");
                }
            }

            if (!done) {
                std::filesystem::remove(part, failure);
            }

            return done;
        }
    }

    bool TileCache::Source::pass(const Take &take, const Keep & /*keep*/, const Decode::Abort *abort) {
        Bitmap band = Bitmap::allocate(width(), std::min(TILE, height()));

        for (int top = 0; top < height(); top += TILE) {
            const int rows = std::min(TILE, height() - top);

            if (Decode::aborted(abort) || !read(top, rows, &band)
                || !take(top, rows, band.all().first(band.pitch() * static_cast<std::size_t>(rows)))) {
                return false;
            }
        }

        return true;
    }

    std::filesystem::path TileCache::location(const std::filesystem::path &file, const std::filesystem::path &folder,
                                              const bool create) {
        std::error_code failure;

        if (const std::filesystem::path parent = std::filesystem::absolute(file, failure).parent_path();
            !failure && !parent.empty()) {
            // An absolute folder replaces the parent whole.
            std::filesystem::path dir = parent / (folder.empty() ? std::filesystem::path("tinyiv-cache") : folder);

            if (usable(dir, create, folder.empty())) {
                return dir;
            }
        }

        std::filesystem::path dir = user_cache();

        if (!dir.empty() && usable(dir, create, true)) {
            return dir;
        }

        std::error_code missing;

        dir = std::filesystem::temp_directory_path(missing) / "tinyiv";

        return usable(dir, create, true) ? dir : std::filesystem::path{};
    }

    std::shared_ptr<TileCache> TileCache::open(const std::filesystem::path &file, const std::filesystem::path &folder,
                                               const Tone::Display &display) {
        Identity identity;

        if (!identify(file, &identity)) {
            return nullptr;
        }

        const std::filesystem::path dir = location(file, folder, false);

        if (dir.empty()) {
            return nullptr;
        }

        std::filesystem::path path = dir / name_of(identity, display);
        std::error_code failure;

        if (!std::filesystem::is_regular_file(path, failure)) {
            path = made_for(dir, identity, display);
        }

        if (path.empty()) {
            return nullptr;
        }

        // Opened now, so the last to go when room is made.
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now(), failure);

        std::shared_ptr<TileCache> tileCache(new TileCache());

        if (!tileCache->load(path, file, display)) {
            std::filesystem::remove(path, failure);

            return nullptr;
        }

        tileCache->start();

        return tileCache;
    }

    std::shared_ptr<TileCache> TileCache::build(const std::filesystem::path &file, const std::filesystem::path &folder,
                                                const Tone::Display &display, const Store store, const bool small,
                                                std::atomic<float> *progress, std::string *error,
                                                Decode::Abort *abort) {
        // An HDR JPEG is lifted by its gain map as a whole, which the source's bands do not do.
        // Only a PNG tone maps its own rows. A JPEG's gain map lifts the whole image, which bands do not.
        std::unique_ptr<Source> source = open_source(file, nullptr, small, display);

        if (source != nullptr && display.hdr() && source->encoding() != Bitmap::Encoding::Pq) {
            source = nullptr;
        }

        if (store == Store::Disk) {
            const std::filesystem::path dir = location(file, folder);

            if (dir.empty()) {
                fail(error, file, "no folder can take the tiles");

                return nullptr;
            }

            if (!write_pyramid(file, dir, display, source.get(), progress, error, abort)) {
                return nullptr;
            }

            std::shared_ptr<TileCache> tileCache = open(file, folder, display);

            if (tileCache == nullptr) {
                fail(error, file, "could not open the tile file just made");
            }

            return tileCache;
        }

        Blocks blocks;
        Pass made;
        const auto ready = [&](const Pass & /*pass*/) -> Sink * { return &blocks; };

        if (source != nullptr ? !run_source_pass(file, *source, progress, error, abort, ready, &made)
                              : !run_pass(file, display, progress, error, abort, ready, &made)) {
            return nullptr;
        }

        std::shared_ptr<TileCache> tileCache(new TileCache());

        tileCache->_channels = made.channels;
        tileCache->_encoding = made.encoding;
        tileCache->_levels = made.levels;

        for (const std::vector<Placed> &placed : made.builder->spans()) {
            std::vector<Span> &spans = tileCache->_spans.emplace_back();

            spans.reserve(placed.size());

            for (const Placed &tile : placed) {
                spans.push_back({.offset = tile.offset, .bytes = tile.bytes});
                tileCache->_storedBytes += static_cast<std::size_t>(tile.bytes);
            }
        }

        tileCache->_blocks = std::move(blocks.held);
        tileCache->_source = std::move(source);
        tileCache->_file = file;
        tileCache->_display = display;
        tileCache->attach_source();
        tileCache->pin();
        tileCache->start();

        return tileCache;
    }

    bool TileCache::load(const std::filesystem::path &path, const std::filesystem::path &file,
                         const Tone::Display &display) {
        const Reader reader(path);
        Header header;
        std::error_code failure;
        const std::uintmax_t size = std::filesystem::file_size(path, failure);

        if (failure || !reader.valid() || !reader.read(0, &header, sizeof header)) {
            return false;
        }

        if (!sound(header, size)) {
            return false;
        }

        std::vector<LevelSize> sizes(header.levels);

        if (!reader.read(header.index, sizes.data(), sizes.size() * sizeof(LevelSize))) {
            return false;
        }

        _channels = static_cast<int>(header.channels);
        _encoding = header.magic == MAGIC_PQ ? Bitmap::Encoding::Pq : Bitmap::Encoding::Srgb;
        std::uint64_t at = header.index + (sizes.size() * sizeof(LevelSize));

        for (const LevelSize &level : sizes) {
            if (level.width == 0 || level.height == 0 || level.width > MAX_SIDE || level.height > MAX_SIDE) {
                return false;
            }

            const auto width = static_cast<int>(level.width);
            const auto height = static_cast<int>(level.height);
            const Level held{
                    .width = width,
                    .height = height,
                    .columns = (width + TILE - 1) / TILE,
                    .rows = (height + TILE - 1) / TILE,
            };
            const std::size_t count = static_cast<std::size_t>(held.columns) * static_cast<std::size_t>(held.rows);

            // The table has to fit in the file before anything is sized from it.
            if (at + (count * sizeof(Placed)) > size) {
                return false;
            }

            std::vector<Placed> placed(count);

            if (!reader.read(at, placed.data(), placed.size() * sizeof(Placed)) || !tiles_fit(placed, held, header)) {
                return false;
            }

            at += placed.size() * sizeof(Placed);

            std::vector<Span> spans;

            spans.reserve(count);

            for (const Placed &tile : placed) {
                spans.push_back({.offset = tile.offset, .bytes = tile.bytes});
            }

            _levels.push_back(held);
            _spans.push_back(std::move(spans));
        }

        if (header.source != 0) {
            std::uint64_t count = 0;

            if (!reader.read(header.source, &count, sizeof count) || count > size - header.source - sizeof count) {
                return false;
            }

            std::vector<std::uint8_t> kept(static_cast<std::size_t>(count));

            if (!reader.read(header.source + sizeof count, kept.data(), kept.size())) {
                return false;
            }

            _source = open_source(file, &kept, false, display);

            if (_source == nullptr || _source->width() != _levels.front().width
                || _source->height() != _levels.front().height) {
                return false;
            }
        }

        _path = path;
        _file = file;
        _display = display;
        attach_source();
        pin();

        return true;
    }

    bool TileCache::suits(const bool small) const {
        if (_checkpoints || !small) {
            return _checkpoints == small;
        }

        // Every tile is kept, which small only allows for a file it cannot give checkpoints of.
        return _source != nullptr || Decode::PngBands::open(_file, _display) == nullptr;
    }

    void TileCache::attach_source() {
        _checkpoints = dynamic_cast<const PngSource *>(_source.get()) != nullptr;

        if (_source == nullptr) {
            return;
        }

        const std::shared_ptr<Reader> reader = _path.empty() ? nullptr : std::make_shared<Reader>(_path);

        _source->found(
                [this, reader](const std::uint64_t at, const std::uint64_t bytes, std::vector<std::uint8_t> &scratch) {
                    return fetch(reader.get(), _blocks, at, bytes, scratch);
                });
    }

    void TileCache::pin() {
        const std::unique_ptr<Reader> reader = _path.empty() ? nullptr : std::make_unique<Reader>(_path);
        Decoder decoder;
        Bitmap band;

        for (std::size_t index = 0; index < _levels.size(); ++index) {
            const Level &level = _levels.at(index);

            if (level.columns * level.rows > PINNED_TILES) {
                continue;
            }

            if (index == 0 && _source != nullptr) {
                for (int row = 0; row < level.rows; ++row) {
                    read_source(row, true, band);
                }

                continue;
            }

            const std::scoped_lock hold(_guard);

            for (int row = 0; row < level.rows; ++row) {
                for (int column = 0; column < level.columns; ++column) {
                    const Key key{.level = static_cast<int>(index), .column = column, .row = row};
                    const Span &span = _spans.at(index).at(
                            (static_cast<std::size_t>(row) * static_cast<std::size_t>(level.columns))
                            + static_cast<std::size_t>(column));
                    const std::span<const std::uint8_t> packed =
                            fetch(reader.get(), _blocks, span.offset, span.bytes, decoder.packed);

                    keep(id_of(key),
                         std::make_shared<const Bitmap>(read_tile(packed, level, _channels, _encoding, key, decoder)),
                         true);
                }
            }
        }
    }

    void TileCache::read_source(const int row, const bool pinned, Bitmap &band) {
        const Level &level = _levels.front();
        const int top = row * TILE;
        const int rows = std::min(TILE, level.height - top);

        if (band.width() != level.width || band.height() < rows) {
            band = Bitmap::allocate(level.width, std::min(TILE, level.height), _encoding);
        }

        const bool read = _source->read(top, rows, &band);
        std::vector<std::shared_ptr<const Bitmap>> tiles(static_cast<std::size_t>(level.columns));
        std::atomic<int> next = 0;

        // Cut on several threads, since fresh tiles fault in page by page as they are first written.
        const auto cut = [&] {
            for (int column = next++; column < level.columns; column = next++) {
                const int width = std::min(TILE, level.width - (column * TILE));
                const std::size_t from = static_cast<std::size_t>(column) * TILE * Bitmap::CHANNELS;
                Bitmap tile = Bitmap::allocate(width, rows, _encoding);

                for (int y = 0; y < rows; ++y) {
                    const std::span<std::uint8_t> target = tile.row(y);

                    // A band that cannot be read shows as a hole, else the view would ask for it forever.
                    if (read) {
                        std::memcpy(target.data(), band.row(y).subspan(from).data(), target.size());
                    } else {
                        std::memset(target.data(), 0, target.size());
                    }
                }

                tiles.at(static_cast<std::size_t>(column)) = std::make_shared<const Bitmap>(std::move(tile));
            }
        };

        {
            std::vector<std::jthread> cutters;
            const int spread = std::min(level.columns, read_threads());

            for (int i = 1; i < spread; ++i) {
                cutters.emplace_back(cut);
            }

            cut();
        }

        const std::scoped_lock hold(_guard);

        for (int column = 0; column < level.columns; ++column) {
            keep(id_of({.level = 0, .column = column, .row = row}),
                 std::move(tiles.at(static_cast<std::size_t>(column))), pinned);
        }
    }

    void TileCache::start() {
        for (int i = 0; i < READERS; ++i) {
            _readers.emplace_back([this] { read_loop(); });
        }
    }

    TileCache::~TileCache() {
        {
            const std::scoped_lock hold(_guard);

            _running = false;
        }

        _wake.notify_all();

        for (std::thread &reader : _readers) {
            reader.join();
        }
    }

    void TileCache::read_loop() {
        const std::unique_ptr<Reader> reader = _path.empty() ? nullptr : std::make_unique<Reader>(_path);
        Decoder decoder;
        Bitmap band;
        std::unique_lock hold(_guard);

        while (_running) {
            if (_queue.empty()) {
                _wake.wait(hold);

                continue;
            }

            const std::uint64_t id = _queue.front();

            _queue.pop_front();

            if (_cache.contains(id) || _reading.contains(id)) {
                continue;
            }

            const Key key = key_of(id);

            if (key.level == 0 && _source != nullptr) {
                read_source_row(hold, key.row, band);

                continue;
            }

            const Level &level = _levels.at(static_cast<std::size_t>(key.level));
            const Span &span = _spans.at(static_cast<std::size_t>(key.level))
                                       .at((static_cast<std::size_t>(key.row) * static_cast<std::size_t>(level.columns))
                                           + static_cast<std::size_t>(key.column));

            _reading.insert(id);
            hold.unlock();

            const std::span<const std::uint8_t> packed =
                    fetch(reader.get(), _blocks, span.offset, span.bytes, decoder.packed);
            auto tile = std::make_shared<const Bitmap>(read_tile(packed, level, _channels, _encoding, key, decoder));

            hold.lock();
            _reading.erase(id);
            keep(id, std::move(tile), false);
            announce(hold);
        }
    }

    // The source decodes whole rows of tiles, so the row is read at once and kept whole.
    void TileCache::read_source_row(std::unique_lock<std::mutex> &hold, const int row, Bitmap &band) {
        std::vector<std::uint64_t> reading;

        for (int column = 0; column < _levels.front().columns; ++column) {
            const std::uint64_t each = id_of({.level = 0, .column = column, .row = row});

            if (!_cache.contains(each) && _reading.insert(each).second) {
                reading.push_back(each);
            }
        }

        if (reading.empty()) {
            return;
        }

        hold.unlock();
        read_source(row, false, band);
        hold.lock();

        for (const std::uint64_t each : reading) {
            _reading.erase(each);
        }

        announce(hold);
    }

    void TileCache::announce(std::unique_lock<std::mutex> &hold) const {
        const std::function<void()> ready = _ready;

        hold.unlock();

        if (ready) {
            ready();
        }

        hold.lock();
    }

    void TileCache::keep(const std::uint64_t id, std::shared_ptr<const Bitmap> bitmap, const bool pinned) const {
        Slot slot;

        slot.pinned = pinned;

        // Pinned tiles stay out of the order of use, so nothing ever evicts them.
        if (!pinned) {
            _used.push_front(id);
            slot.used = _used.begin();
            _bytes += bitmap->bytes();
        }

        slot.bitmap = std::move(bitmap);
        _cache.insert_or_assign(id, std::move(slot));

        while (_bytes > CACHE_BYTES && !_used.empty()) {
            const auto oldest = _cache.find(_used.back());

            _used.pop_back();

            if (oldest != _cache.end()) {
                _bytes -= oldest->second.bitmap->bytes();
                _cache.erase(oldest);
            }
        }
    }

    std::shared_ptr<const Bitmap> TileCache::find(const Key &key) const {
        const std::scoped_lock hold(_guard);

        const auto found = _cache.find(id_of(key));

        if (found == _cache.end()) {
            return nullptr;
        }

        if (!found->second.pinned) {
            _used.splice(_used.begin(), _used, found->second.used);
        }

        return found->second.bitmap;
    }

    void TileCache::want(const std::span<const Key> keys) const {
        {
            const std::scoped_lock hold(_guard);
            std::unordered_set<std::uint64_t> seen;

            _queue.clear();

            for (const Key &key : keys) {
                if (_queue.size() >= MAX_QUEUE) {
                    break;
                }

                if (key.level < 0 || static_cast<std::size_t>(key.level) >= _levels.size()) {
                    continue;
                }

                const Level &level = _levels.at(static_cast<std::size_t>(key.level));

                if (key.column < 0 || key.column >= level.columns || key.row < 0 || key.row >= level.rows) {
                    continue;
                }

                const std::uint64_t id = id_of(key);

                if (_cache.contains(id) || _reading.contains(id) || !seen.insert(id).second) {
                    continue;
                }

                _queue.push_back(id);
            }
        }

        _wake.notify_all();
    }

    void TileCache::on_ready(std::function<void()> ready) const {
        const std::scoped_lock hold(_guard);

        _ready = std::move(ready);
    }

    void TileCache::shrink() const {
        const std::scoped_lock hold(_guard);

        for (const std::uint64_t id : _used) {
            _cache.erase(id);
        }

        _used.clear();
        _bytes = 0;
    }

    std::size_t TileCache::cached_bytes() const {
        const std::scoped_lock hold(_guard);

        return _bytes;
    }

    std::uint64_t TileCache::id_of(const Key &key) {
        return (static_cast<std::uint64_t>(key.level) << 48U) | (static_cast<std::uint64_t>(key.row) << 24U)
               | static_cast<std::uint64_t>(key.column);
    }

    TileCache::Key TileCache::key_of(const std::uint64_t id) {
        constexpr std::uint64_t MASK = (std::uint64_t{1} << 24U) - 1;

        return {
                .level = static_cast<int>(id >> 48U),
                .column = static_cast<int>(id & MASK),
                .row = static_cast<int>((id >> 24U) & MASK),
        };
    }
}
// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)
