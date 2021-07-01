// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_GL_BASIC_PAGEFAULT_TRACK_H_
#define ORBIT_GL_BASIC_PAGEFAULT_TRACK_H_

#include <string>
#include <utility>

#include "AnnotationTrack.h"
#include "LineGraphTrack.h"
#include "Track.h"
#include "Viewport.h"

namespace orbit_gl {

class BasicPagefaultTrack final : public LineGraphTrack<3>, public AnnotationTrack {
 public:
  explicit BasicPagefaultTrack(Track* parent, TimeGraph* time_graph, orbit_gl::Viewport* viewport,
                               TimeGraphLayout* layout, std::string name,
                               std::array<std::string, 3> series_names,
                               const orbit_client_model::CaptureData* capture_data)
      : LineGraphTrack<3>(parent, time_graph, viewport, layout, name, series_names, capture_data),
        AnnotationTrack() {
    draw_background_ = false;
    parent_ = parent;
  }

  [[nodiscard]] Track* GetParent() const override { return parent_; }
  [[nodiscard]] Track::Type GetType() const override { return Track::Type::kUnknown; }

  void AddValues(uint64_t timestamp_ns, const std::array<double, 3>& values) override;
  void AddValuesAndUpdateAnnotations(uint64_t timestamp_ns, const std::array<double, 3>& values);

  void Draw(Batcher& batcher, TextRenderer& text_renderer, uint64_t current_mouse_time_ns,
            PickingMode picking_mode, float z_offset = 0) override;

  void SetIndexOfSeriesToHighlight(size_t series_index) {
    if (series_index >= 3) return;
    index_of_series_to_highlight_ = series_index;
  }

 protected:
  void DrawSingleSeriesEntry(Batcher* batcher, uint64_t start_tick, uint64_t end_tick,
                             const std::array<float, 3>& current_normalized_values,
                             const std::array<float, 3>& next_normalized_values, float z) override;

 private:
  [[nodiscard]] bool ShouldBeCollapsed() const override;
  [[nodiscard]] float GetAnnotatedTrackContentHeight() const override {
    return this->size_[1] - this->layout_->GetTrackTabHeight() -
           this->layout_->GetTrackBottomMargin() - this->GetLegendHeight();
  }
  [[nodiscard]] Vec2 GetAnnotatedTrackPosition() const override { return this->pos_; };
  [[nodiscard]] Vec2 GetAnnotatedTrackSize() const override { return this->size_; };

  Track* parent_;
  std::optional<std::pair<uint64_t, std::array<double, 3>>> previous_time_and_values_ =
      std::nullopt;
  std::optional<size_t> index_of_series_to_highlight_ = std::nullopt;
};

}  // namespace orbit_gl

#endif  // ORBIT_GL_BASIC_PAGEFAULT_TRACK_H_
