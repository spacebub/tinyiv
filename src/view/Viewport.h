// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#ifndef TIV_VIEW_VIEWPORT_H
#define TIV_VIEW_VIEWPORT_H


namespace tiv {
    struct Rect {
        double x = 0.0;
        double y = 0.0;
        double width = 0.0;
        double height = 0.0;
    };

    // Where the image sits on screen. Everything is in pixels, zoom is screen pixels per image pixel.
    class Viewport {

    public:
        // How far either side of fit the zoom may go.
        static constexpr double ZOOM_RANGE = 64.0;

        void set_area(double width, double height);
        void set_image(int width, int height);

        [[nodiscard]] bool has_image() const { return _imageWidth > 0 && _imageHeight > 0; }

        // Whole image visible and centred, small images scaled up.
        void fit();

        void pan(double dx, double dy);

        // Later zoom_by() calls keep the image point under (x, y) fixed there.
        void begin_zoom(double x, double y);
        void zoom_by(double factor);

        [[nodiscard]] double zoom() const { return _zoom; }
        [[nodiscard]] double area_width() const { return _areaWidth; }
        [[nodiscard]] double area_height() const { return _areaHeight; }
        [[nodiscard]] Rect image_rect() const;

    private:
        double _areaWidth = 0.0;
        double _areaHeight = 0.0;
        int _imageWidth = 0;
        int _imageHeight = 0;

        double _zoom = 1.0;
        double _fitZoom = 1.0;
        double _x = 0.0;
        double _y = 0.0;

        double _anchorX = 0.0;
        double _anchorY = 0.0;
        double _anchorImageX = 0.0;
        double _anchorImageY = 0.0;
        double _anchorZoom = 1.0;
    };
}


#endif //TIV_VIEW_VIEWPORT_H
