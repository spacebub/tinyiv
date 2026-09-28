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

    // A rect in the unit square of the image as stored, and where it lands in the unit square
    // of the image shown with the orientation.
    [[nodiscard]] Rect oriented(const Rect &unit, int orientation);

    // Everything is in pixels, zoom is screen pixels per image pixel.
    class Viewport {

    public:
        // How far either side of fit the zoom may go.
        static constexpr double ZOOM_RANGE = 64.0;

        // However large the image, zoom reaches this many screen pixels per image pixel.
        static constexpr double MIN_MAX_ZOOM = 32.0;

        void set_area(double width, double height);
        void set_image(int width, int height);

        [[nodiscard]] bool has_image() const { return _imageWidth > 0 && _imageHeight > 0; }

        // Whole image visible and centred, small images scaled up.
        void fit();

        // Centres the image at the current zoom.
        void centre();

        void pan(double dx, double dy);

        // Later zoom_by() calls keep the image point under (x, y) fixed there.
        void begin_zoom(double x, double y);
        void zoom_by(double factor);

        // Keeps the image point at the centre of the area where it is.
        void zoom_centred(double factor);
        void zoom_to(double zoom);

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
