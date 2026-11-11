// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_APP_PICTURE_H
#define TIV_APP_PICTURE_H


#include <cstdint>
#include <filesystem>
#include <string>

#include "app/Window.h"
#include "gallery/Folder.h"
#include "image/decode/Decode.h"
#include "services/Loader.h"
#include "services/Refiner.h"
#include "view/Playback.h"
#include "view/Viewport.h"

namespace tiv {
class Picture {

public:
    struct Size {
        int width = 0;
        int height = 0;
    };

    struct Parts {
        Folder *folder = nullptr;
        Viewport *viewport = nullptr;
        Playback *playback = nullptr;
        Window *window = nullptr;
        Loader *loader = nullptr;
        Refiner *refiner = nullptr;
        bool *dirty = nullptr;
        bool *seeking = nullptr;
        std::uint32_t tileCacheEvent = 0;
        // Only the image on screen is decoded and held, as little of it as the view allows.
        bool footprint = false;
    };

    void attach(const Parts &parts);

    // Direction is the way the wheel was going, so prefetch leans that way.
    void show(int index, int direction);
    void deliver();
    void deliver_detail();
    void give_back();
    // Reads the image from disk again, dropping any turn not saved.
    void reload();
    void rebuild();
    void turn(int by);
    // Writes how the image is turned into its file, when it is turned.
    void save();
    // Switches streaming mode, where every still image shows from a pyramid of tiles.
    void toggle_stream();
    // Sorts the folder the next way along, keeping the image on screen.
    void next_order();
    // The view changed, so the part on screen may want rendering again once it rests.
    void moved();
    void refine();

    // How the image on screen is drawn, from how it is stored.
    [[nodiscard]] int orientation() const;
    // The size of the image on screen as stored, and as shown.
    [[nodiscard]] Size stored() const;
    [[nodiscard]] Size shown() const;
    // The image on screen streams from tiles because it is too large for memory, not by the mode.
    [[nodiscard]] bool always_streamed() const;
    [[nodiscard]] bool settled() const;

    [[nodiscard]] const Decode::Info &info() const { return _info; }
    [[nodiscard]] bool turned() const { return _turn != 1; }
    [[nodiscard]] int direction() const { return _direction; }
    [[nodiscard]] std::uintmax_t bytes() const { return _bytes; }
    [[nodiscard]] bool loading() const { return _loading; }
    [[nodiscard]] bool building() const { return _building; }
    [[nodiscard]] bool streamed() const { return _streamed; }
    [[nodiscard]] bool stale() const { return _stale; }
    [[nodiscard]] const std::string &message() const { return _message; }
    [[nodiscard]] const std::string &failure() const { return _failure; }
    [[nodiscard]] const std::string &flash() const { return _flash; }
    [[nodiscard]] const std::filesystem::path &tile_folder() const { return _tileFolder; }
    [[nodiscard]] std::uint64_t switched_at() const { return _switchedAt; }
    [[nodiscard]] bool settling() const { return _settling; }
    [[nodiscard]] std::uint64_t rests_at() const;
    [[nodiscard]] bool freed_since_give_back() const { return _freedSinceGiveBack; }
    [[nodiscard]] std::uint64_t give_back_at() const;

private:
    void warm_neighbours();

    // What was last sent to the refiner, so a request that failed is not repeated.
    struct Ask {
        std::uint64_t generation = 0;
        double scale = 0.0;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;

        bool operator==(const Ask &) const = default;
    };

    Folder *_folder = nullptr;
    Viewport *_viewport = nullptr;
    Playback *_playback = nullptr;
    Window *_window = nullptr;
    Loader *_loader = nullptr;
    Refiner *_refiner = nullptr;
    bool *_dirty = nullptr;
    bool *_seeking = nullptr;
    std::uint32_t _tileCacheEvent = 0;
    bool _footprint = false;

    Decode::Info _info;
    // Turns and flips on top of the orientation the file says, until saved.
    int _turn = 1;
    // Replaces the image details in the bottom bar until the next image or turn.
    std::string _message;
    bool _freedSinceGiveBack = false;
    std::uint64_t _shownAt = 0;
    std::uintmax_t _bytes = 0;
    std::uint64_t _generation = 0;
    std::uint64_t _shownGeneration = 0;
    // Something of the current image is still on its way: its first pixels, or the whole
    // image behind a preview.
    bool _loading = false;
    // The image is too large for memory, and its tiles are being written to disk.
    bool _building = false;
    // The image shows from disk.
    bool _streamed = false;
    int _direction = 1;
    // Shown in place of an image that could not be opened.
    std::string _failure;
    // What the last press of S or O did, shown for a moment after.
    std::string _flash;
    // Where the tiles being made go on disk, empty when they stay in memory.
    std::filesystem::path _tileFolder;
    // The streamed image shows from a tile cache made for another display, until Shift+R rebuilds it.
    bool _stale = false;

    std::uint64_t _movedAt = 0;
    // When streaming mode or the order last switched, zero before either has.
    std::uint64_t _switchedAt = 0;
    // Moved and not yet looked at by refine().
    bool _settling = false;
    Ask _asked;
};
}


#endif //TIV_APP_PICTURE_H
