// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "image/Pyramid.h"
#include "render/Canvas.h"
#include "render/Tiles.h"
#include "view/Viewport.h"

namespace tiv {
    namespace {
        Rect intersect(const Rect &a, const Rect &b) {
            const double x0 = std::max(a.x, b.x);
            const double y0 = std::max(a.y, b.y);
            const double x1 = std::min(a.x + a.width, b.x + b.width);
            const double y1 = std::min(a.y + a.height, b.y + b.height);

            if (x1 <= x0 || y1 <= y0) {
                return {};
            }

            return {x0, y0, x1 - x0, y1 - y0};
        }

        Rect grow(const Rect &rect, const double by) {
            return {rect.x - by, rect.y - by, rect.width + (2.0 * by), rect.height + (2.0 * by)};
        }

        Rect whole(const Tiles &tiles) {
            return {0.0, 0.0, static_cast<double>(tiles.width()), static_cast<double>(tiles.height())};
        }
    }

    Canvas::Canvas(SDL_Renderer *renderer, const int maxTexture, const std::size_t vramBudget) : _renderer(renderer), _maxTexture(maxTexture), _budget(vramBudget) {
    }

    Canvas::Held Canvas::build(std::shared_ptr<const Pyramid> pyramid, const std::uint64_t image, const int width, const int height, Held previous) const {
        Held held;

        held.image = image;
        held.width = std::max(width, 1);
        held.height = std::max(height, 1);
        held.pyramid = std::move(pyramid);

        for (const std::shared_ptr<const Bitmap> &level : std::views::reverse(held.pyramid->levels)) {
            Sheet sheet;

            sheet.scale = static_cast<double>(level->width()) / held.width;

            if (previous.image == image) {
                const auto same = std::ranges::find_if(previous.sheets, [&](const Sheet &old) {
                    return old.tiles != nullptr && old.tiles->width() == level->width() && old.tiles->height() == level->height();
                });

                if (same != previous.sheets.end()) {
                    sheet.tiles = std::move(same->tiles);
                    sheet.tiles->rebind(level);
                }
            }

            if (sheet.tiles == nullptr) {
                sheet.tiles = std::make_unique<Tiles>(_renderer, _maxTexture, level);
            }

            held.sheets.push_back(std::move(sheet));
        }

        return held;
    }

    void Canvas::show(std::shared_ptr<const Pyramid> pyramid, const std::uint64_t image, const int width, const int height) {
        if (pyramid == nullptr || pyramid->empty()) {
            return;
        }

        Held previous = std::move(_current);

        // A neighbour already on the GPU is the whole point of keeping it warm.
        if (previous.image != image) {
            if (const auto warm = std::ranges::find(_warm, image, &Held::image); warm != _warm.end()) {
                Held promoted = std::move(*warm);

                _warm.erase(warm);

                if (!previous.sheets.empty()) {
                    _warm.push_back(std::move(previous));
                }

                previous = std::move(promoted);
            } else if (!previous.sheets.empty()) {
                _warm.push_back(std::move(previous));
                previous = {};
            }
        }

        _current = build(std::move(pyramid), image, width, height, std::move(previous));

        // The coarsest level is what every missing tile falls back to, so it is never missing.
        if (!_current.sheets.empty()) {
            Tiles &coarsest = *_current.sheets.front().tiles;

            coarsest.upload(whole(coarsest), ~std::size_t{0}, _frame);
        }
    }

    void Canvas::warm(std::shared_ptr<const Pyramid> pyramid, const std::uint64_t image, const int width, const int height) {
        if (pyramid == nullptr || pyramid->empty() || image == _current.image) {
            return;
        }

        const auto found = std::ranges::find(_warm, image, &Held::image);

        if (found != _warm.end()) {
            if (found->pyramid == pyramid) {
                return;
            }

            Held previous = std::move(*found);

            _warm.erase(found);
            _warm.push_back(build(std::move(pyramid), image, width, height, std::move(previous)));

            return;
        }

        _warm.push_back(build(std::move(pyramid), image, width, height, {}));
    }

    void Canvas::keep(const std::span<const std::uint64_t> images) {
        std::erase_if(_warm, [&](const Held &held) { return std::ranges::find(images, held.image) == images.end(); });
    }

    void Canvas::clear() {
        _current = {};
        _warm.clear();
    }

    std::size_t Canvas::wanted(const Held &held, const double zoom) {
        for (std::size_t i = 0; i < held.sheets.size(); ++i) {
            if (held.sheets.at(i).scale >= zoom) {
                return i;
            }
        }

        return held.sheets.empty() ? 0 : held.sheets.size() - 1;
    }

    Rect Canvas::area_of(const Held &held, const std::size_t sheet, const Rect &screen, const Rect &image) {
        const Tiles &tiles = *held.sheets.at(sheet).tiles;
        const double perX = tiles.width() / image.width;
        const double perY = tiles.height() / image.height;

        return {(screen.x - image.x) * perX, (screen.y - image.y) * perY, screen.width * perX, screen.height * perY};
    }

    std::size_t Canvas::fit_sheet(const Held &held) const {
        if (_areaWidth <= 0.0 || _areaHeight <= 0.0) {
            return 0;
        }

        return wanted(held, std::min(_areaWidth / held.width, _areaHeight / held.height));
    }

    bool Canvas::pending(const Viewport &viewport) const {
        if (_current.sheets.empty()) {
            return false;
        }

        const Rect image = viewport.image_rect();
        const Rect visible = intersect(image, {0.0, 0.0, viewport.area_width(), viewport.area_height()});
        const std::size_t sheet = wanted(_current, viewport.zoom());

        if (visible.width > 0.0 && !_current.sheets.at(sheet).tiles->covered(area_of(_current, sheet, visible, image))) {
            return true;
        }

        return std::ranges::any_of(_warm, [&](const Held &held) {
            return std::ranges::any_of(std::span(held.sheets).first(fit_sheet(held) + 1), [](const Sheet &s) { return !s.tiles->complete(); });
        });
    }

    void Canvas::upload(const Viewport &viewport, const std::size_t budget) {
        _areaWidth = viewport.area_width();
        _areaHeight = viewport.area_height();

        std::size_t spent = 0;

        if (!_current.sheets.empty()) {
            const Rect image = viewport.image_rect();
            const Rect visible = intersect(image, {0.0, 0.0, _areaWidth, _areaHeight});
            const std::size_t sheet = wanted(_current, viewport.zoom());
            Tiles &tiles = *_current.sheets.at(sheet).tiles;

            if (visible.width > 0.0) {
                const Rect area = area_of(_current, sheet, visible, image);

                spent += tiles.upload(area, budget - std::min(spent, budget), _frame);

                if (spent < budget) {
                    spent += tiles.upload(grow(area, Tiles::SIZE), budget - spent, _frame);
                }

                if (spent < budget && sheet > 0) {
                    Tiles &below = *_current.sheets.at(sheet - 1).tiles;

                    spent += below.upload(grow(area_of(_current, sheet - 1, visible, image), Tiles::SIZE), budget - spent, _frame);
                }
            }
        }

        if (spent < budget) {
            upload_warm(budget - spent);
        }

        evict();
    }

    std::size_t Canvas::upload_warm(const std::size_t budget) {
        std::size_t spent = 0;

        for (Held &held : _warm) {
            for (Sheet &sheet : std::span(held.sheets).first(fit_sheet(held) + 1)) {
                if (spent >= budget) {
                    return spent;
                }

                if (!sheet.tiles->complete()) {
                    spent += sheet.tiles->upload(whole(*sheet.tiles), budget - spent, _frame);
                }
            }
        }

        return spent;
    }

    void Canvas::draw(const Viewport &viewport) {
        ++_frame;

        if (_current.sheets.empty()) {
            return;
        }

        const Rect image = viewport.image_rect();
        const Rect visible = intersect(image, {0.0, 0.0, viewport.area_width(), viewport.area_height()});

        if (visible.width <= 0.0 || visible.height <= 0.0) {
            return;
        }

        draw_sheet(_current, wanted(_current, viewport.zoom()), visible, image);
    }

    // Whatever the sheet is missing under the screen rect is drawn from the one below it.
    void Canvas::draw_sheet(Held &held, const std::size_t index, const Rect &screen, const Rect &image) {
        Tiles &tiles = *held.sheets.at(index).tiles;
        const Rect area = area_of(held, index, screen, image);
        const double perX = tiles.width() / image.width;
        const double perY = tiles.height() / image.height;

        tiles.draw(area, screen, _frame, [&](const Rect &missing) {
            if (index == 0) {
                return;
            }

            const Rect fallback{image.x + (missing.x / perX), image.y + (missing.y / perY), missing.width / perX, missing.height / perY};

            draw_sheet(held, index - 1, fallback, image);
        });
    }

    // Over the budget, tiles nobody drew this frame go: the neighbours' fine levels first,
    // then the current image's, never the coarsest of any and never a warm level within the screen.
    void Canvas::evict() {
        std::size_t over = vram() > _budget ? vram() - _budget : 0;

        if (over == 0) {
            return;
        }

        for (Held &held : _warm) {
            for (auto it = held.sheets.rbegin(); it != held.sheets.rend() && over > 0; ++it) {
                const auto index = static_cast<std::size_t>(held.sheets.rend() - it) - 1;

                if (index <= fit_sheet(held)) {
                    break;
                }

                over -= std::min(over, it->tiles->evict_all());
            }
        }

        for (auto it = _current.sheets.rbegin(); it != _current.sheets.rend() && over > 0; ++it) {
            if (it == _current.sheets.rend() - 1) {
                break;
            }

            over -= std::min(over, it->tiles->evict(_frame, over));
        }
    }

    std::size_t Canvas::vram() const {
        const auto sum = [](const Held &held) {
            return std::accumulate(held.sheets.begin(), held.sheets.end(), std::size_t{0}, [](const std::size_t total, const Sheet &sheet) {
                return total + sheet.tiles->resident_bytes();
            });
        };

        return std::accumulate(_warm.begin(), _warm.end(), sum(_current), [&](const std::size_t total, const Held &held) { return total + sum(held); });
    }
}
