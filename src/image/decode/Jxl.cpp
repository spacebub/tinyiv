// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <jxl/decode.h>
#include <jxl/thread_parallel_runner.h>

#include "image/Bitmap.h"
#include "image/Tone.h"
#include "image/decode/GainMap.h"
#include "image/decode/Heif.h"
#include "image/decode/Jxl.h"
#include "image/decode/Support.h"

namespace tiv::Decode {
    namespace {
        struct JxlHandle {
            JxlDecoder *decoder = JxlDecoderCreate(nullptr);
            void *runner = JxlThreadParallelRunnerCreate(nullptr, JxlThreadParallelRunnerDefaultNumWorkerThreads());

            JxlHandle() = default;

            ~JxlHandle() {
                JxlThreadParallelRunnerDestroy(runner);
                JxlDecoderDestroy(decoder);
            }

            JxlHandle(const JxlHandle &) = delete;
            JxlHandle(JxlHandle &&) = delete;
            JxlHandle &operator=(const JxlHandle &) = delete;
            JxlHandle &operator=(JxlHandle &&) = delete;
        };

        Size jxl_size(const JxlBasicInfo &info) {
            const bool swapped = info.orientation >= JXL_ORIENT_TRANSPOSE;

            return {static_cast<int>(swapped ? info.ysize : info.xsize), static_cast<int>(swapped ? info.xsize : info.ysize)};
        }

        // The encoding the pixels come out in. An ICC profile alone is taken for SDR.
        Tone::Source jxl_tone(JxlDecoder *decoder) {
            JxlColorEncoding encoding;

            if (JxlDecoderGetColorAsEncodedProfile(decoder, JXL_COLOR_PROFILE_TARGET_DATA, &encoding) != JXL_DEC_SUCCESS) {
                return {};
            }

            // The enumerations take their values from H.273.
            return Tone::from_cicp(static_cast<int>(encoding.primaries), static_cast<int>(encoding.transfer_function));
        }

        // Up to the colour encoding: the size and whether it is HDR.
        bool jxl_header(const std::span<const std::uint8_t> data, JxlBasicInfo *basic, Tone::Source *tone) {
            const JxlHandle handle;

            if (handle.decoder == nullptr || JxlDecoderSubscribeEvents(handle.decoder, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING) != JXL_DEC_SUCCESS
                || JxlDecoderSetInput(handle.decoder, data.data(), data.size()) != JXL_DEC_SUCCESS) {
                return false;
            }

            JxlDecoderCloseInput(handle.decoder);

            for (;;) {
                const JxlDecoderStatus status = JxlDecoderProcessInput(handle.decoder);

                if (status == JXL_DEC_BASIC_INFO) {
                    if (JxlDecoderGetBasicInfo(handle.decoder, basic) != JXL_DEC_SUCCESS) {
                        return false;
                    }
                } else if (status == JXL_DEC_COLOR_ENCODING) {
                    *tone = jxl_tone(handle.decoder);

                    return true;
                } else {
                    return false;
                }
            }
        }

        // A gain map that brings an HDR base down to its SDR rendition, as a jhgm box carries it.
        // Gains outside the base's colours are in sRGB's, the only ones read.
        struct JxlGain {
            GainMap::Metadata metadata;
            Bitmap map;
        };

        // Where a run of pixels libjxl hands over goes, and what hears it has landed.
        using JxlPlace = std::function<std::span<std::uint8_t>(std::size_t x, std::size_t y, std::size_t pixels)>;
        using JxlLanded = std::function<void(std::size_t y, std::size_t pixels)>;

        // How libjxl's pixels become four bytes a pixel, a run of a row at a time from several
        // threads: copied as RGBA8, or through the tone mapper and a gain map.
        struct JxlOutput {
            std::optional<Tone::Mapper> mapper;
            JxlDataType type = JXL_TYPE_UINT8;
            Bitmap::Encoding encoding = Bitmap::Encoding::Srgb;
            // Held apart, as the applier keeps the map's address.
            std::unique_ptr<JxlGain> gainMap;
            std::optional<GainMap::Applier> gain;
            // Into the colour space the gains apply in and back, when that is not the base's own.
            std::optional<std::pair<Tone::Matrix, Tone::Matrix>> gainColours;
            JxlPlace place;
            JxlLanded landed;

            void take(const std::size_t x, const std::size_t y, const std::size_t pixels, const void *samples) const {
                const std::span<std::uint8_t> out = place(x, y, pixels);
                const std::size_t count = pixels * Bitmap::CHANNELS;
                // Captured by reference, so the adjust fits in std::function without an allocation per run.
                const std::array<std::size_t, 2> start{x, y};
                const Tone::Adjust adjust = gain ? Tone::Adjust([this, &start](const std::size_t first, const std::span<float> rgba) { lift(start, first, rgba); }) : Tone::Adjust{};

                if (!mapper) {
                    std::memcpy(out.data(), samples, count);
                } else if (type == JXL_TYPE_FLOAT) {
                    mapper->map(std::span(static_cast<const float *>(samples), count), Bitmap::CHANNELS, out, adjust);
                } else {
                    mapper->map(std::span(static_cast<const std::uint16_t *>(samples), count), Bitmap::CHANNELS, out, adjust);
                }

                if (landed) {
                    landed(y, pixels);
                }
            }

            void lift(const std::array<std::size_t, 2> &start, const std::size_t first, const std::span<float> rgba) const {
                const auto [x, y] = start;

                if (!gain) {
                    return;
                }

                if (gainColours) {
                    Tone::transform(gainColours->first, rgba);
                }

                gain->apply(static_cast<int>(x + first), static_cast<int>(y), rgba);

                if (gainColours) {
                    Tone::transform(gainColours->second, rgba);
                }
            }
        };

        void jxl_take(void *opaque, const std::size_t x, const std::size_t y, const std::size_t pixels, const void *samples) {
            static_cast<const JxlOutput *>(opaque)->take(x, y, pixels, samples);
        }

        // Told the colour encoding, says how the pixels come out, or nothing for RGBA8 as stored.
        using JxlSetup = std::function<std::optional<JxlOutput>(const Tone::Source &tone, int width, int height)>;

        // RGBA8 straight into the bitmap, or through the output's callback.
        bool jxl_output(JxlDecoder *decoder, Bitmap &held, JxlOutput *output) {
            if (output != nullptr) {
                const JxlPixelFormat format{Bitmap::CHANNELS, output->type, JXL_NATIVE_ENDIAN, 0};

                return JxlDecoderSetImageOutCallback(decoder, &format, jxl_take, output) == JXL_DEC_SUCCESS;
            }

            const JxlPixelFormat format{Bitmap::CHANNELS, JXL_TYPE_UINT8, JXL_NATIVE_ENDIAN, 0};
            std::size_t needed = 0;

            return JxlDecoderImageOutBufferSize(decoder, &format, &needed) == JXL_DEC_SUCCESS && needed == held.bytes()
                   && JxlDecoderSetImageOutBuffer(decoder, &format, held.data(), held.bytes()) == JXL_DEC_SUCCESS;
        }

        // The bitmap sized for the image, false for an animation.
        bool jxl_canvas(JxlDecoder *decoder, Bitmap *held) {
            JxlBasicInfo info;

            if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS || info.have_animation != 0) {
                return false;
            }

            *held = Bitmap::allocate(static_cast<int>(info.xsize), static_cast<int>(info.ysize));

            return true;
        }

        // An output that maps writes into the bitmap, reallocated in the output's encoding.
        void jxl_bind(std::optional<JxlOutput> &output, Bitmap *held) {
            if (!output) {
                return;
            }

            *held = Bitmap::allocate(held->width(), held->height(), output->encoding);
            output->place = [held](const std::size_t x, const std::size_t y, const std::size_t pixels) {
                return held->row(static_cast<int>(y)).subspan(x * Bitmap::CHANNELS, pixels * Bitmap::CHANNELS);
            };
        }

        // Without a setup the samples come out as stored, which a gain map wants. libjxl runs
        // with its own thread pool, which libvips does not use.
        Direct jxl_decode(const std::span<const std::uint8_t> data, Bitmap *out, const Decode::Abort *abort, const JxlSetup &setup) {
            const JxlHandle handle;
            const int events = JXL_DEC_BASIC_INFO | JXL_DEC_FULL_IMAGE | (setup ? JXL_DEC_COLOR_ENCODING : 0);

            if (handle.decoder == nullptr || handle.runner == nullptr
                || JxlDecoderSetParallelRunner(handle.decoder, JxlThreadParallelRunner, handle.runner) != JXL_DEC_SUCCESS
                || JxlDecoderSetKeepOrientation(handle.decoder, JXL_TRUE) != JXL_DEC_SUCCESS
                || JxlDecoderSubscribeEvents(handle.decoder, events) != JXL_DEC_SUCCESS
                || JxlDecoderSetInput(handle.decoder, data.data(), data.size()) != JXL_DEC_SUCCESS) {
                return Direct::Skip;
            }

            JxlDecoderCloseInput(handle.decoder);

            Bitmap held;
            std::optional<JxlOutput> output;

            for (;;) {
                if (aborted(abort)) {
                    return Direct::Failed;
                }

                const JxlDecoderStatus status = JxlDecoderProcessInput(handle.decoder);

                if (status == JXL_DEC_BASIC_INFO) {
                    if (!jxl_canvas(handle.decoder, &held)) {
                        return Direct::Skip;
                    }
                } else if (status == JXL_DEC_COLOR_ENCODING) {
                    // Basic info comes first, so the bitmap is there.
                    output = setup(jxl_tone(handle.decoder), held.width(), held.height());
                    jxl_bind(output, &held);
                } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                    if (held.empty() || !jxl_output(handle.decoder, held, output ? &*output : nullptr)) {
                        return Direct::Skip;
                    }
                } else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS) {
                    break;
                } else {
                    return Direct::Failed;
                }
            }

            if (held.empty()) {
                return Direct::Skip;
            }

            *out = std::move(held);

            return Direct::Done;
        }

        std::unique_ptr<JxlGain> jxl_gain(const std::span<const std::uint8_t> data) {
            GainMap::Jxl bundle;
            auto held = std::make_unique<JxlGain>();

            if (!GainMap::read_jxl(Heif::box(data, "jhgm"), &bundle) || jxl_decode(bundle.image, &held->map, nullptr, {}) != Direct::Done) {
                return nullptr;
            }

            held->metadata = bundle.metadata;

            return held;
        }

        // HDR is tone mapped, or brought down to SDR by a gain map where there is one.
        std::optional<JxlOutput> jxl_setup(const std::span<const std::uint8_t> data, const Tone::Source &tone, const int width, const int height, const Tone::Display &display) {
            if (!tone.hdr()) {
                return std::nullopt;
            }

            std::unique_ptr<JxlGain> gain = jxl_gain(data);
            const float weight = gain != nullptr ? gain->metadata.weight(std::log2(display.headroom)) : 0.0F;
            // Brought down to SDR, the gain map leaves nothing to roll off, and clipping keeps its colours.
            const bool rolledOff = weight == 0.0F || display.hdr();
            JxlOutput held;

            held.mapper.emplace(tone, display, rolledOff);
            held.type = tone.transfer == Tone::Transfer::Linear ? JXL_TYPE_FLOAT : JXL_TYPE_UINT16;
            held.encoding = display.hdr() ? Bitmap::Encoding::Pq : Bitmap::Encoding::Srgb;

            if (gain != nullptr && weight != 0.0F) {
                held.gain.emplace(gain->metadata, &gain->map, width, height, weight);

                if (!gain->metadata.inBaseColours && tone.primaries != Tone::Primaries::Bt709) {
                    held.gainColours.emplace(Tone::convert(tone.primaries, Tone::Primaries::Bt709), Tone::convert(Tone::Primaries::Bt709, tone.primaries));
                }

                held.gainMap = std::move(gain);
            }

            return held;
        }

        // Runs libjxl hands over from many threads in no order, gathered into bands of rows that
        // go on in order. A thread more than a few bands ahead of the one going on waits, so the
        // bands in memory stay few while the decode outruns whatever takes them. The runs of the
        // bands before it were claimed first by other threads, so the wait always ends.
        class JxlBands {

        public:
            static constexpr std::size_t AHEAD = 4;

            JxlBands(const int width, const int height, const int rows, const Bitmap::Encoding encoding, const Decode::Take &take, const Decode::Abort *abort)
                : _width(width), _height(height), _rows(rows), _encoding(encoding), _take(take), _abort(abort), _slots(static_cast<std::size_t>((height + rows - 1) / rows)) {
            }

            std::span<std::uint8_t> place(const std::size_t x, const std::size_t y, const std::size_t pixels) {
                const std::size_t index = y / static_cast<std::size_t>(_rows);
                Slot &slot = _slots.at(index);

                wait_for(index);
                std::call_once(slot.made, [&] {
                    const int rows = std::min(_rows, _height - (static_cast<int>(index) * _rows));

                    slot.band = Bitmap::allocate(_width, rows, _encoding);
                    slot.remaining.store(static_cast<std::size_t>(_width) * static_cast<std::size_t>(rows));
                });

                return slot.band.row(static_cast<int>(y % static_cast<std::size_t>(_rows))).subspan(x * Bitmap::CHANNELS, pixels * Bitmap::CHANNELS);
            }

            void landed(const std::size_t y, const std::size_t pixels) {
                Slot &slot = _slots.at(y / static_cast<std::size_t>(_rows));

                if (slot.remaining.fetch_sub(pixels) == pixels) {
                    slot.whole.store(true);
                    deliver();
                }
            }

            // Take said no, or the abort came, so the decode should end.
            [[nodiscard]] bool stopped(const Decode::Abort *abort) const { return _failed.load() || aborted(abort); }

            void stop() {
                _failed.store(true);
                _moved.notify_all();
            }

            [[nodiscard]] bool complete() const { return _next == _slots.size() && !_failed.load(); }

        private:
            struct Slot {
                std::once_flag made;
                Bitmap band;
                std::atomic<std::size_t> remaining = 0;
                std::atomic<bool> whole = false;
            };

            // An abort sends no notice, so the wait looks for one now and then. Nearly every run
            // is within reach, which the lock-free look sees.
            void wait_for(const std::size_t index) {
                constexpr auto LOOK = std::chrono::milliseconds(20);
                const auto near = [&] { return index < _next.load() + AHEAD || _failed.load(); };

                if (near()) {
                    return;
                }

                std::unique_lock hold(_guard);

                while (!_moved.wait_for(hold, LOOK, near)) {
                    if (aborted(_abort)) {
                        return;
                    }
                }
            }

            void deliver() {
                const std::scoped_lock hold(_guard);

                while (_next < _slots.size() && _slots.at(_next).whole.load() && !_failed.load()) {
                    Slot &slot = _slots.at(_next);

                    if (!_take(static_cast<int>(_next) * _rows, slot.band.height(), slot.band.all())) {
                        _failed.store(true);
                    }

                    slot.band = {};
                    ++_next;
                    _moved.notify_all();
                }
            }

            int _width;
            int _height;
            int _rows;
            Bitmap::Encoding _encoding;
            const Decode::Take &_take;
            const Decode::Abort *_abort;
            std::vector<Slot> _slots;
            std::mutex _guard;
            std::condition_variable _moved;
            std::atomic<std::size_t> _next = 0;
            std::atomic<bool> _failed = false;
        };

        // libjxl's pool, which stops handing out work once the decode should end, so an abort
        // lands within a group rather than after the whole frame.
        struct JxlStoppable {
            void *pool = nullptr;
            std::function<bool()> stopped;
        };

        struct JxlTask {
            const JxlStoppable *runner = nullptr;
            void *opaque = nullptr;
            JxlParallelRunInit init = nullptr;
            JxlParallelRunFunction run = nullptr;
        };

        JxlParallelRetCode jxl_run(void *opaque, void *jpegxlOpaque, JxlParallelRunInit init, JxlParallelRunFunction run, const std::uint32_t start, const std::uint32_t end) {
            const auto *runner = static_cast<const JxlStoppable *>(opaque);

            if (runner->stopped()) {
                return JXL_PARALLEL_RET_RUNNER_ERROR;
            }

            JxlTask task{runner, jpegxlOpaque, init, run};

            const JxlParallelRetCode code = JxlThreadParallelRunner(runner->pool, &task,
                [](void *held, const std::size_t threads) {
                    const auto *given = static_cast<const JxlTask *>(held);

                    return given->init(given->opaque, threads);
                },
                [](void *held, const std::uint32_t value, const std::size_t thread) {
                    const auto *given = static_cast<const JxlTask *>(held);

                    if (!given->runner->stopped()) {
                        given->run(given->opaque, value, thread);
                    }
                },
                start, end);

            return runner->stopped() ? JXL_PARALLEL_RET_RUNNER_ERROR : code;
        }

        struct JxlStreamTo {
            int rows = 0;
            const Decode::Begin *begin = nullptr;
            const Decode::Take *take = nullptr;
            const Decode::Abort *abort = nullptr;
        };

        // Tells begin what is coming, and points the output at bands that go on to take. An
        // image with nothing to map is copied as it comes.
        bool jxl_start(JxlDecoder *decoder, const std::span<const std::uint8_t> data, const Tone::Display &display, const JxlStreamTo &to, std::optional<JxlOutput> &output,
                       std::optional<JxlBands> &bands) {
            JxlBasicInfo info;

            if (JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS) {
                return false;
            }

            const auto width = static_cast<int>(info.xsize);
            const auto height = static_cast<int>(info.ysize);

            output = jxl_setup(data, jxl_tone(decoder), width, height, display);

            if (!output) {
                output.emplace();
            }

            if (!(*to.begin)(width, height, info.alpha_bits > 0, output->encoding)) {
                return false;
            }

            bands.emplace(width, height, to.rows, output->encoding, *to.take, to.abort);
            output->place = [&bands](const std::size_t x, const std::size_t y, const std::size_t pixels) { return bands->place(x, y, pixels); };
            output->landed = [&bands](const std::size_t y, const std::size_t pixels) { bands->landed(y, pixels); };

            return true;
        }

        bool jxl_animated(JxlDecoder *decoder) {
            JxlBasicInfo info;

            return JxlDecoderGetBasicInfo(decoder, &info) != JXL_DEC_SUCCESS || info.have_animation != 0;
        }

        bool jxl_listen(JxlDecoder *decoder, std::optional<JxlOutput> &output) {
            if (!output) {
                return false;
            }

            const JxlPixelFormat format{Bitmap::CHANNELS, output->type, JXL_NATIVE_ENDIAN, 0};

            return JxlDecoderSetImageOutCallback(decoder, &format, jxl_take, &*output) == JXL_DEC_SUCCESS;
        }

        void jxl_stop(std::optional<JxlBands> &bands) {
            if (bands) {
                bands->stop();
            }
        }

        // libjxl keeps float rows along every group border of a lossy frame, for the filters that
        // run across groups, which comes to about 2.7 bytes a pixel in measurements. Lossless
        // frames need far less.
        constexpr double JXL_LOSSY_BYTES = 2.8;
        constexpr double JXL_LOSSLESS_BYTES = 1.0;
    }

    Tone::Source Jxl::tone(const std::span<const std::uint8_t> data) {
        JxlBasicInfo basic;
        Tone::Source tone;

        return jxl_header(data, &basic, &tone) ? tone : Tone::Source{};
    }

    bool Jxl::probe(const std::span<const std::uint8_t> data, Decode::Info *info) {
        JxlBasicInfo basic;
        Tone::Source tone;

        if (!jxl_header(data, &basic, &tone)) {
            return false;
        }

        const Size size = jxl_size(basic);

        info->width = size.width;
        info->height = size.height;
        info->orientation = static_cast<int>(basic.orientation);
        info->hdr = tone.hdr() || !Heif::box(data, "jhgm").empty();

        return true;
    }

    Direct Jxl::load(const std::filesystem::path &file, const std::span<const std::uint8_t> data, Bitmap *out, std::string *error, const Decode::Abort *abort, const Tone::Display &display) {
        const Direct direct = jxl_decode(data, out, abort, [data, display](const Tone::Source &tone, const int width, const int height) {
            return jxl_setup(data, tone, width, height, display);
        });

        if (direct == Direct::Failed) {
            fail(error, file, aborted(abort) ? "aborted" : "jxl decode failed");
        }

        return direct;
    }

    Direct Jxl::stream(const std::span<const std::uint8_t> data, const int rows, const Decode::Begin &begin, const Decode::Take &take, const Decode::Abort *abort, const Tone::Display &display) {
        const JxlHandle handle;
        std::optional<JxlBands> bands;
        JxlStoppable runner{handle.runner, [&] { return bands ? bands->stopped(abort) : aborted(abort); }};

        if (handle.decoder == nullptr || handle.runner == nullptr || JxlDecoderSetParallelRunner(handle.decoder, jxl_run, &runner) != JXL_DEC_SUCCESS
            || JxlDecoderSetKeepOrientation(handle.decoder, JXL_TRUE) != JXL_DEC_SUCCESS
            || JxlDecoderSubscribeEvents(handle.decoder, JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FULL_IMAGE) != JXL_DEC_SUCCESS
            || JxlDecoderSetInput(handle.decoder, data.data(), data.size()) != JXL_DEC_SUCCESS) {
            return Direct::Skip;
        }

        JxlDecoderCloseInput(handle.decoder);

        const JxlStreamTo to{rows, &begin, &take, abort};
        std::optional<JxlOutput> output;
        // Begin is heard at the colour encoding, and from then on there is no going back to libvips.
        Direct failed = Direct::Skip;

        for (;;) {
            const JxlDecoderStatus status = JxlDecoderProcessInput(handle.decoder);

            if (status == JXL_DEC_BASIC_INFO) {
                if (jxl_animated(handle.decoder)) {
                    return Direct::Skip;
                }
            } else if (status == JXL_DEC_COLOR_ENCODING) {
                failed = Direct::Failed;

                if (!jxl_start(handle.decoder, data, display, to, output, bands)) {
                    return Direct::Failed;
                }
            } else if (status == JXL_DEC_NEED_IMAGE_OUT_BUFFER) {
                if (!jxl_listen(handle.decoder, output)) {
                    return Direct::Failed;
                }
            } else if (status == JXL_DEC_FULL_IMAGE || status == JXL_DEC_SUCCESS) {
                break;
            } else {
                jxl_stop(bands);

                return failed;
            }
        }

        return bands && bands->complete() ? Direct::Done : Direct::Failed;
    }

    std::uint64_t Jxl::stream_bytes(const std::span<const std::uint8_t> data, const int rows) {
        JxlBasicInfo basic;
        Tone::Source tone;

        std::memset(&basic, 0, sizeof basic);

        if (!jxl_header(data, &basic, &tone)) {
            return 0;
        }

        const double pixels = static_cast<double>(basic.xsize) * static_cast<double>(basic.ysize);
        const double band = static_cast<double>(basic.xsize) * static_cast<double>(rows) * Bitmap::CHANNELS;

        return static_cast<std::uint64_t>((pixels * (basic.uses_original_profile != 0 ? JXL_LOSSLESS_BYTES : JXL_LOSSY_BYTES)) + (band * (JxlBands::AHEAD + 1)));
    }
}
