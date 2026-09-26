// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Part of tinyiv
 *
 * Copyright (c) 2026
 * Authors:
 *	spacebub <spacebubs@proton.me>
 */
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

#include "image/Svg.h"

namespace tiv {
    namespace {
        struct Root {
            // Just past the element name, and at the '>' or "/>" closing the start tag.
            std::size_t attributes = 0;
            std::size_t close = 0;
            // Every attribute but the four that place the document, each with a leading space.
            std::string kept;
            std::string_view viewBox;
            std::string_view aspect;
        };

        // The whole document at the scale: device = user * scale + offset.
        struct Placement {
            double scaleX = 1.0;
            double scaleY = 1.0;
            double offsetX = 0.0;
            double offsetY = 0.0;
        };

        // NUL past the end, which no test below matches.
        char peek(const std::string_view text, const std::size_t at) {
            return at < text.size() ? text.at(at) : '\0';
        }

        bool space(const char c) {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        std::size_t skip_space(const std::string_view text, std::size_t at) {
            while (space(peek(text, at))) {
                ++at;
            }

            return at;
        }

        // Past a doctype, which may carry an internal subset in brackets.
        std::size_t skip_doctype(const std::string_view text, const std::size_t at) {
            int depth = 0;
            char quote = 0;

            for (std::size_t i = at + 2; i < text.size(); ++i) {
                const char c = peek(text, i);

                if (quote != 0) {
                    quote = c == quote ? 0 : quote;
                } else if (c == '"' || c == '\'') {
                    quote = c;
                } else if (c == '[' || c == ']') {
                    depth += c == '[' ? 1 : -1;
                } else if (c == '>' && depth == 0) {
                    return i + 1;
                }
            }

            return std::string_view::npos;
        }

        std::size_t past(const std::string_view text, const std::size_t at, const std::string_view closing) {
            const std::size_t end = text.find(closing, at);

            return end == std::string_view::npos ? end : end + closing.size();
        }

        // Past the prolog: declarations, comments and a doctype.
        std::size_t skip_prolog(const std::string_view text) {
            std::size_t at = text.starts_with("\xEF\xBB\xBF") ? 3 : 0;

            while (at < text.size()) {
                at = skip_space(text, at);

                const std::string_view rest = text.substr(std::min(at, text.size()));

                if (rest.starts_with("<?")) {
                    at = past(text, at, "?>");
                } else if (rest.starts_with("<!--")) {
                    at = past(text, at, "-->");
                } else if (rest.starts_with("<!")) {
                    at = skip_doctype(text, at);
                } else {
                    break;
                }
            }

            return std::min(at, text.size());
        }

        std::size_t skip_name(const std::string_view text, std::size_t at) {
            while (peek(text, at) != '\0' && !space(peek(text, at)) && peek(text, at) != '=' && peek(text, at) != '>' && peek(text, at) != '/') {
                ++at;
            }

            return at;
        }

        // One name="value" pair from at, returning just past it, or npos when malformed.
        std::size_t read_attribute(const std::string_view text, const std::size_t at, std::string_view *key, std::string_view *value) {
            const std::size_t nameEnd = skip_name(text, at);
            std::size_t pos = skip_space(text, nameEnd);

            if (peek(text, pos) != '=') {
                return std::string_view::npos;
            }

            pos = skip_space(text, pos + 1);

            if ((peek(text, pos) != '"' && peek(text, pos) != '\'')) {
                return std::string_view::npos;
            }

            const std::size_t end = text.find(peek(text, pos), pos + 1);

            if (end == std::string_view::npos) {
                return end;
            }

            *key = text.substr(at, nameEnd - at);
            *value = text.substr(pos + 1, end - pos - 1);

            return end + 1;
        }

        bool find_root(const std::string_view text, Root *root) {
            const std::size_t at = skip_prolog(text);

            if (peek(text, at) != '<') {
                return false;
            }

            std::size_t pos = skip_name(text, at + 1);
            std::string_view name = text.substr(at + 1, pos - at - 1);

            if (const std::size_t colon = name.rfind(':'); colon != std::string_view::npos) {
                name = name.substr(colon + 1);
            }

            if (name != "svg") {
                return false;
            }

            root->attributes = pos;

            while (true) {
                pos = skip_space(text, pos);

                if (pos >= text.size()) {
                    return false;
                }

                if (peek(text, pos) == '>' || peek(text, pos) == '/') {
                    root->close = pos;

                    return true;
                }

                std::string_view key;
                std::string_view value;
                const std::size_t start = pos;

                pos = read_attribute(text, pos, &key, &value);

                if (pos == std::string_view::npos) {
                    return false;
                }

                if (key == "viewBox") {
                    root->viewBox = value;
                } else if (key == "preserveAspectRatio") {
                    root->aspect = value;
                } else if (key != "width" && key != "height") {
                    root->kept += ' ';
                    root->kept += text.substr(start, pos - start);
                }
            }
        }

        struct ViewBox {
            double x = 0.0;
            double y = 0.0;
            double width = 0.0;
            double height = 0.0;
        };

        bool parse_view_box(std::string_view text, ViewBox *out) {
            for (double *number : {&out->x, &out->y, &out->width, &out->height}) {
                while (!text.empty() && (space(text.front()) || text.front() == ',')) {
                    text.remove_prefix(1);
                }

                const auto [end, failed] = std::from_chars(std::to_address(text.begin()), std::to_address(text.end()), *number);

                if (failed != std::errc{}) {
                    return false;
                }

                text.remove_prefix(static_cast<std::size_t>(end - std::to_address(text.begin())));
            }

            return out->width > 0.0 && out->height > 0.0;
        }

        // Where an axis puts the view box in the viewport, as preserveAspectRatio names it: 0 min, 1 mid, 2 max.
        int align(const std::string_view aspect, const std::string_view axis) {
            const std::size_t at = aspect.find(axis);

            if (at == std::string_view::npos) {
                return 1;
            }

            const std::string_view rest = aspect.substr(at + axis.size());

            if (rest.starts_with("Min")) {
                return 0;
            }

            return rest.starts_with("Max") ? 2 : 1;
        }

        // Without a view box librsvg takes the intrinsic size for one, which is the whole
        // image before the scale.
        bool place(const Root &root, const double scale, const int wholeWidth, const int wholeHeight, Placement *out) {
            ViewBox box{0.0, 0.0, wholeWidth / scale, wholeHeight / scale};

            if (!root.viewBox.empty() && !parse_view_box(root.viewBox, &box)) {
                return false;
            }

            out->scaleX = wholeWidth / box.width;
            out->scaleY = wholeHeight / box.height;

            if (!root.aspect.contains("none")) {
                const bool slice = root.aspect.contains("slice");
                const double uniform = slice ? std::max(out->scaleX, out->scaleY) : std::min(out->scaleX, out->scaleY);

                out->scaleX = uniform;
                out->scaleY = uniform;
            }

            out->offsetX = ((wholeWidth - (box.width * out->scaleX)) * align(root.aspect, "x") / 2.0) - (box.x * out->scaleX);
            out->offsetY = ((wholeHeight - (box.height * out->scaleY)) * align(root.aspect, "Y") / 2.0) - (box.y * out->scaleY);

            return true;
        }
    }

    std::string Svg::narrow(const std::span<const std::uint8_t> document, const double scale, const int wholeWidth, const int wholeHeight, const int x, const int y, const int width, const int height) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the mapped file is text.
        const std::string_view text(reinterpret_cast<const char *>(document.data()), document.size());
        Root root;
        Placement placement;

        if (!find_root(text, &root) || !place(root, scale, wholeWidth, wholeHeight, &placement)) {
            return {};
        }

        std::string narrowed;

        narrowed.reserve(text.size() + 256);
        narrowed += text.substr(0, root.attributes);
        narrowed += root.kept;
        narrowed += std::format(R"( width="{}" height="{}" viewBox="{} {} {} {}" preserveAspectRatio="none")", width, height, (x - placement.offsetX) / placement.scaleX, (y - placement.offsetY) / placement.scaleY,
                                width / placement.scaleX, height / placement.scaleY);
        narrowed += text.substr(root.close);

        return narrowed;
    }
}
