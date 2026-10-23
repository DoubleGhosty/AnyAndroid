#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

// The device's screen shown in a window of another shape (FrameWindow): the
// image keeps its aspect ratio, centred, with bands around it, and the
// window's pixels map back to the display's. Independent of Win32.
namespace aa::backend {

// A rectangle of window pixels: x in [left, right), y in [top, bottom).
struct Box {
  int32_t left = 0, top = 0, right = 0, bottom = 0;
  int32_t width() const { return right - left; }
  int32_t height() const { return bottom - top; }
  bool contains(int32_t x, int32_t y) const { return x >= left && x < right && y >= top && y < bottom; }
  bool operator==(const Box&) const = default;
};

// The largest rectangle of the w x h image's aspect ratio within `area`,
// centred (an empty one at the area's corner when either is empty).
inline Box fit_letterbox(const Box& area, int32_t w, int32_t h) {
  int32_t aw = area.width(), ah = area.height();
  if (w <= 0 || h <= 0 || aw <= 0 || ah <= 0) return Box{area.left, area.top, area.left, area.top};
  int32_t dw = aw, dh = int32_t(int64_t(aw) * h / w);
  if (dh > ah) {
    dh = ah;
    dw = int32_t(int64_t(ah) * w / h);
  }
  int32_t x = area.left + (aw - dw) / 2, y = area.top + (ah - dh) / 2;
  return Box{x, y, x + dw, y + dh};
}

// A point of the image, in its own pixels (the display's).
struct ImagePoint {
  float x = 0, y = 0;
  bool operator==(const ImagePoint&) const = default;
};

// The image point under window pixel (cx, cy) of a w x h image shown in
// `image`, clamped to the image: pixel centres map to pixel centres (the
// identity at scale 1).
inline ImagePoint window_to_image_clamped(const Box& image, int32_t w, int32_t h, int32_t cx, int32_t cy) {
  if (image.width() <= 0 || image.height() <= 0) return {};
  float x = (float(cx - image.left) + 0.5f) * float(w) / float(image.width()) - 0.5f;
  float y = (float(cy - image.top) + 0.5f) * float(h) / float(image.height()) - 0.5f;
  return ImagePoint{std::clamp(x, 0.0f, float(w - 1)), std::clamp(y, 0.0f, float(h - 1))};
}

// The same for a press: nullopt outside the image (in the bands).
inline std::optional<ImagePoint> window_to_image(const Box& image, int32_t w, int32_t h, int32_t cx, int32_t cy) {
  if (!image.contains(cx, cy)) return std::nullopt;
  return window_to_image_clamped(image, w, h, cx, cy);
}

}  // namespace aa::backend
