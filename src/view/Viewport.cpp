// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <cmath>
#include <utility>

#include "image/Orient.h"
#include "view/Viewport.h"

namespace tiv {
    Rect oriented(const Rect &unit, const int orientation) {
        double x0 = unit.x;
        double y0 = unit.y;
        double x1 = unit.x + unit.width;
        double y1 = unit.y + unit.height;

        if (Orient::mirrors(orientation)) {
            x0 = 1.0 - x0;
            x1 = 1.0 - x1;
        }

        for (int turn = 0; turn < Orient::quarters(orientation); ++turn) {
            x0 = std::exchange(y0, x0);
            x1 = std::exchange(y1, x1);
            x0 = 1.0 - x0;
            x1 = 1.0 - x1;
        }

        return {.x = std::min(x0, x1), .y = std::min(y0, y1), .width = std::abs(x1 - x0), .height = std::abs(y1 - y0)};
    }

    void Viewport::set_area(const double width, const double height) {
        if (width == _areaWidth && height == _areaHeight) {
            return;
        }

        _areaWidth = width;
        _areaHeight = height;

        fit();
    }

    void Viewport::set_image(const int width, const int height) {
        _imageWidth = width;
        _imageHeight = height;

        fit();
    }

    void Viewport::fit() {
        if (!has_image() || _areaWidth <= 0.0 || _areaHeight <= 0.0) {
            return;
        }

        _fitZoom = std::min(_areaWidth / _imageWidth, _areaHeight / _imageHeight);
        _zoom = _fitZoom;
        _x = (_areaWidth - (_imageWidth * _zoom)) / 2.0;
        _y = (_areaHeight - (_imageHeight * _zoom)) / 2.0;
    }

    void Viewport::centre() {
        _x = (_areaWidth - (_imageWidth * _zoom)) / 2.0;
        _y = (_areaHeight - (_imageHeight * _zoom)) / 2.0;
    }

    void Viewport::pan(const double dx, const double dy) {
        _x += dx;
        _y += dy;
    }

    void Viewport::begin_zoom(const double x, const double y) {
        _anchorX = x;
        _anchorY = y;
        _anchorImageX = (x - _x) / _zoom;
        _anchorImageY = (y - _y) / _zoom;
        _anchorZoom = _zoom;
    }

    void Viewport::zoom_by(const double factor) {
        const double lowest = _fitZoom / ZOOM_RANGE;
        const double highest = std::max(_fitZoom * ZOOM_RANGE, MIN_MAX_ZOOM);

        _zoom = std::clamp(_anchorZoom * factor, lowest, highest);
        _x = _anchorX - (_anchorImageX * _zoom);
        _y = _anchorY - (_anchorImageY * _zoom);
    }

    void Viewport::zoom_centred(const double factor) {
        begin_zoom(_areaWidth / 2.0, _areaHeight / 2.0);
        zoom_by(factor);
    }

    void Viewport::zoom_to(const double zoom) {
        zoom_centred(zoom / _zoom);
    }

    Rect Viewport::image_rect() const {
        return {.x = _x, .y = _y, .width = _imageWidth * _zoom, .height = _imageHeight * _zoom};
    }
}
