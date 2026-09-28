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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <ranges>
#include <span>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>

#include "image/Orient.h"
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

            return {.x = x0, .y = y0, .width = x1 - x0, .height = y1 - y0};
        }

        Rect grow(const Rect &rect, const double by) {
            return {
                    .x = rect.x - by,
                    .y = rect.y - by,
                    .width = rect.width + (2.0 * by),
                    .height = rect.height + (2.0 * by),
            };
        }

        Rect whole(const Tiles &tiles) {
            return {
                    .x = 0.0,
                    .y = 0.0,
                    .width = static_cast<double>(tiles.width()),
                    .height = static_cast<double>(tiles.height()),
            };
        }

        bool contains(const Rect &outer, const Rect &inner) {
            constexpr double SLACK = 1e-6;

            return inner.x >= outer.x - SLACK && inner.y >= outer.y - SLACK
                   && inner.x + inner.width <= outer.x + outer.width + SLACK
                   && inner.y + inner.height <= outer.y + outer.height + SLACK;
        }

        // The rect as a share of the outer one, which is the unit square.
        Rect within(const Rect &rect, const Rect &outer) {
            return {
                    .x = (rect.x - outer.x) / outer.width,
                    .y = (rect.y - outer.y) / outer.height,
                    .width = rect.width / outer.width,
                    .height = rect.height / outer.height,
            };
        }

        // A share of the outer rect back in the outer rect's own terms.
        Rect at(const Rect &unit, const Rect &outer) {
            return {
                    .x = outer.x + (unit.x * outer.width),
                    .y = outer.y + (unit.y * outer.height),
                    .width = unit.width * outer.width,
                    .height = unit.height * outer.height,
            };
        }

        // What is left of the outer rect around the inner one, which lies within it, as bands.
        std::array<Rect, 4> around(const Rect &outer, const Rect &inner) {
            const double right = inner.x + inner.width;
            const double bottom = inner.y + inner.height;

            return {
                    {
                            {.x = outer.x, .y = outer.y, .width = outer.width, .height = inner.y - outer.y},
                            {
                                    .x = outer.x,
                                    .y = bottom,
                                    .width = outer.width,
                                    .height = outer.y + outer.height - bottom,
                            },
                            {.x = outer.x, .y = inner.y, .width = inner.x - outer.x, .height = inner.height},
                            {.x = right, .y = inner.y, .width = outer.x + outer.width - right, .height = inner.height},
                    },
            };
        }
    }

    Canvas::Canvas(SDL_Renderer *renderer, const int maxTexture, const std::size_t vramBudget)
        : _renderer(renderer), _maxTexture(maxTexture), _budget(vramBudget) {
    }

    Canvas::Held Canvas::build(std::shared_ptr<const Pyramid> pyramid, const std::uint64_t image, const int width,
                               const int height, const int orientation, Held previous) const {
        Held held;

        held.image = image;
        held.width = std::max(width, 1);
        held.height = std::max(height, 1);
        held.orientation = orientation;
        held.pyramid = std::move(pyramid);

        for (const std::shared_ptr<const Bitmap> &level : std::views::reverse(held.pyramid->levels)) {
            Sheet sheet;

            sheet.scale = static_cast<double>(level->width()) / held.width;

            if (previous.image == image && previous.tileCache == nullptr) {
                const auto same = std::ranges::find_if(previous.sheets, [&](const Sheet &old) {
                    return old.tiles != nullptr && old.tiles->width() == level->width()
                           && old.tiles->height() == level->height();
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

    Canvas::Held Canvas::build(std::shared_ptr<const TileCache> tileCache, const std::uint64_t image, const int width,
                               const int height, const int orientation, Held previous) const {
        // The same tile cache again keeps every tile it has on the GPU.
        if (previous.image == image && previous.tileCache == tileCache) {
            previous.orientation = orientation;

            return previous;
        }

        Held held;

        held.image = image;
        held.width = std::max(width, 1);
        held.height = std::max(height, 1);
        held.orientation = orientation;
        held.tileCache = std::move(tileCache);

        const std::span<const TileCache::Level> levels = held.tileCache->levels();

        for (std::size_t index = levels.size(); index-- > 0;) {
            Sheet sheet;

            sheet.scale = static_cast<double>(levels[index].width) / held.width;
            sheet.tiles = std::make_unique<Tiles>(_renderer, held.tileCache, static_cast<int>(index));
            held.sheets.push_back(std::move(sheet));
        }

        return held;
    }

    Canvas::Held Canvas::take_previous(const std::uint64_t image) {
        if (image != _current.image) {
            _detail = {};
        }

        Held previous = std::move(_current);

        _current = {};

        if (previous.image == image) {
            return previous;
        }

        // An image on disk would never finish uploading warm, so it is let go.
        if (previous.tileCache != nullptr) {
            previous = {};
        }

        if (const auto warm = std::ranges::find(_warm, image, &Held::image); warm != _warm.end()) {
            Held promoted = std::move(*warm);

            _warm.erase(warm);

            if (!previous.sheets.empty()) {
                _warm.push_back(std::move(previous));
            }

            return promoted;
        }

        if (!previous.sheets.empty()) {
            _warm.push_back(std::move(previous));
        }

        return {};
    }

    void Canvas::settle() {
        if (!_current.sheets.empty()) {
            Tiles &coarsest = *_current.sheets.front().tiles;

            coarsest.upload(whole(coarsest), ~std::size_t{0}, _frame);
        }
    }

    void Canvas::show(std::shared_ptr<const Pyramid> pyramid, const std::uint64_t image, const int width,
                      const int height, const int orientation) {
        if (pyramid == nullptr || pyramid->empty()) {
            return;
        }

        Held previous = take_previous(image);

        _current = build(std::move(pyramid), image, width, height, orientation, std::move(previous));
        settle();
    }

    void Canvas::show(std::shared_ptr<const TileCache> tileCache, const std::uint64_t image, const int width,
                      const int height, const int orientation) {
        if (tileCache == nullptr) {
            return;
        }

        Held previous = take_previous(image);

        _current = build(std::move(tileCache), image, width, height, orientation, std::move(previous));
        settle();
    }

    void Canvas::fetch(const Viewport &viewport) {
        if (_current.tileCache == nullptr || _current.sheets.empty()) {
            return;
        }

        const Rect image = viewport.image_rect();
        const Rect visible = intersect(
                image, {.x = 0.0, .y = 0.0, .width = viewport.area_width(), .height = viewport.area_height()});
        std::vector<TileCache::Key> keys;

        if (visible.width > 0.0 && visible.height > 0.0) {
            const std::size_t sheet = wanted(_current, viewport.zoom());
            const Tiles &tiles = *_current.sheets.at(sheet).tiles;
            const Rect area = area_of(_current, sheet, visible, image);

            if (sheet > 0) {
                _current.sheets.at(sheet - 1).tiles->missing(area_of(_current, sheet - 1, visible, image), &keys);
            }

            tiles.missing(area, &keys);
            tiles.missing(grow(area, Tiles::SIZE), &keys);
        }

        _current.tileCache->want(keys);
    }

    void Canvas::replace(std::shared_ptr<const Pyramid> pyramid) {
        if (pyramid == nullptr || pyramid->empty() || _current.sheets.empty()) {
            return;
        }

        const auto &levels = pyramid->levels;
        const bool same = levels.size() == _current.sheets.size()
                          && std::ranges::equal(std::views::reverse(levels), _current.sheets,
                                                [](const auto &level, const Sheet &sheet) {
                                                    return level->width() == sheet.tiles->width()
                                                           && level->height() == sheet.tiles->height();
                                                });

        if (!same) {
            show(std::move(pyramid), _current.image, _current.width, _current.height, _current.orientation);

            return;
        }

        for (std::size_t i = 0; i < levels.size(); ++i) {
            _current.sheets.at(i).tiles->refresh(levels.at(levels.size() - 1 - i));
        }

        _current.pyramid = std::move(pyramid);
    }

    void Canvas::orient(const int orientation) {
        _current.orientation = orientation;
    }

    void Canvas::warm(std::shared_ptr<const Pyramid> pyramid, const std::uint64_t image, const int width,
                      const int height, const int orientation) {
        if (pyramid == nullptr || pyramid->empty() || image == _current.image) {
            return;
        }

        const auto found = std::ranges::find(_warm, image, &Held::image);

        if (found != _warm.end()) {
            if (found->pyramid == pyramid) {
                found->orientation = orientation;

                return;
            }

            Held previous = std::move(*found);

            _warm.erase(found);
            _warm.push_back(build(std::move(pyramid), image, width, height, orientation, std::move(previous)));

            return;
        }

        _warm.push_back(build(std::move(pyramid), image, width, height, orientation, {}));
    }

    void Canvas::refine(std::shared_ptr<const Bitmap> bitmap, const double scale, const int x, const int y) {
        if (bitmap == nullptr || bitmap->empty() || _current.sheets.empty() || scale <= 0.0) {
            return;
        }

        const Rect area{
                .x = x / scale,
                .y = y / scale,
                .width = bitmap->width() / scale,
                .height = bitmap->height() / scale,
        };

        _detail = {
                .image = _current.image,
                .scale = scale,
                .area = area,
                .tiles = std::make_unique<Tiles>(_renderer, _maxTexture, std::move(bitmap)),
        };

        // Whole, so it never has holes to fall back from.
        _detail.tiles->upload(whole(*_detail.tiles), ~std::size_t{0}, _frame);
    }

    bool Canvas::refined(const double zoom, const Rect &area) const {
        return _detail.tiles != nullptr && _detail.image == _current.image && _detail.scale == zoom
               && contains(_detail.area, area);
    }

    void Canvas::keep(const std::span<const std::uint64_t> images) {
        std::erase_if(_warm, [&](const Held &held) { return std::ranges::find(images, held.image) == images.end(); });
    }

    void Canvas::clear() {
        _detail = {};
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
        const Rect unit = oriented(within(screen, image), Orient::inverse(held.orientation));

        return {
                .x = unit.x * tiles.width(),
                .y = unit.y * tiles.height(),
                .width = unit.width * tiles.width(),
                .height = unit.height * tiles.height(),
        };
    }

    std::size_t Canvas::fit_sheet(const Held &held) const {
        if (_areaWidth <= 0.0 || _areaHeight <= 0.0) {
            return 0;
        }

        const bool swapped = Orient::swaps(held.orientation);
        const double shownWidth = swapped ? held.height : held.width;
        const double shownHeight = swapped ? held.width : held.height;

        return wanted(held, std::min(_areaWidth / shownWidth, _areaHeight / shownHeight));
    }

    bool Canvas::pending(const Viewport &viewport) const {
        if (_current.sheets.empty()) {
            return false;
        }

        const Rect image = viewport.image_rect();
        const Rect visible = intersect(
                image, {.x = 0.0, .y = 0.0, .width = viewport.area_width(), .height = viewport.area_height()});
        const std::size_t sheet = wanted(_current, viewport.zoom());

        if (visible.width > 0.0
            && _current.sheets.at(sheet).tiles->uploadable(area_of(_current, sheet, visible, image))) {
            return true;
        }

        return std::ranges::any_of(_warm, [&](const Held &held) {
            return std::ranges::any_of(std::span(held.sheets).first(fit_sheet(held) + 1),
                                       [](const Sheet &s) { return !s.tiles->complete(); });
        });
    }

    void Canvas::upload(const Viewport &viewport, const std::size_t budget) {
        _areaWidth = viewport.area_width();
        _areaHeight = viewport.area_height();

        std::size_t spent = 0;

        if (!_current.sheets.empty()) {
            const Rect image = viewport.image_rect();
            const Rect visible = intersect(image, {.x = 0.0, .y = 0.0, .width = _areaWidth, .height = _areaHeight});
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

                    spent += below.upload(grow(area_of(_current, sheet - 1, visible, image), Tiles::SIZE),
                                          budget - spent, _frame);
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
        const Rect visible = intersect(
                image, {.x = 0.0, .y = 0.0, .width = viewport.area_width(), .height = viewport.area_height()});

        if (visible.width <= 0.0 || visible.height <= 0.0) {
            return;
        }

        const std::size_t sheet = wanted(_current, viewport.zoom());

        if (!detail_wins(viewport.zoom(), sheet)) {
            draw_sheet(_current, sheet, visible, image);

            return;
        }

        const Rect stored{
                .x = _detail.area.x / _current.width,
                .y = _detail.area.y / _current.height,
                .width = _detail.area.width / _current.width,
                .height = _detail.area.height / _current.height,
        };
        const Rect placed = at(oriented(stored, _current.orientation), image);
        const Rect over = intersect(visible, placed);

        if (over.width <= 0.0 || over.height <= 0.0) {
            draw_sheet(_current, sheet, visible, image);

            return;
        }

        // The levels only around the rendering, since translucent pixels drawn twice show.
        for (const Rect &band : around(visible, over)) {
            if (band.width > 0.0 && band.height > 0.0) {
                draw_sheet(_current, sheet, band, image);
            }
        }

        Tiles &tiles = *_detail.tiles;
        const Rect unit = oriented(within(over, placed), Orient::inverse(_current.orientation));
        const Rect area{
                .x = unit.x * tiles.width(),
                .y = unit.y * tiles.height(),
                .width = unit.width * tiles.width(),
                .height = unit.height * tiles.height(),
        };

        tiles.draw(area, placed, _current.orientation, _frame, [](const Rect &) {});
    }

    // Whichever is nearer the zoom, the rendering or the level, as a ratio either way.
    bool Canvas::detail_wins(const double zoom, const std::size_t sheet) const {
        if (_detail.tiles == nullptr || _detail.image != _current.image) {
            return false;
        }

        return std::abs(std::log(_detail.scale / zoom)) <= std::abs(std::log(_current.sheets.at(sheet).scale / zoom));
    }

    // Whatever the sheet is missing under the screen rect is drawn from the one below it.
    void Canvas::draw_sheet(Held &held, const std::size_t index, const Rect &screen, const Rect &image) {
        held.sheets.at(index).tiles->draw(area_of(held, index, screen, image), image, held.orientation, _frame,
                                          [&](const Rect &missing) {
                                              if (index > 0) {
                                                  draw_sheet(held, index - 1, missing, image);
                                              }
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
            return std::accumulate(
                    held.sheets.begin(), held.sheets.end(), std::size_t{0},
                    [](const std::size_t total, const Sheet &sheet) { return total + sheet.tiles->resident_bytes(); });
        };

        const std::size_t detail = _detail.tiles != nullptr ? _detail.tiles->resident_bytes() : 0;

        return std::accumulate(_warm.begin(), _warm.end(), sum(_current) + detail,
                               [&](const std::size_t total, const Held &held) { return total + sum(held); });
    }
}
