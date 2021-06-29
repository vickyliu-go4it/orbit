// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "BasicPagefaultTrack.h"

#include "GlCanvas.h"

namespace orbit_gl {

void BasicPagefaultTrack::AddValues(uint64_t timestamp_ns, const std::array<double, 3>& values) {
  if (previous_time_and_values_.has_value()) {
    std::array<double, 3> differences;
    std::transform(values.begin(), values.end(), previous_time_and_values_.value().second.begin(),
                   differences.begin(), std::minus<double>());
    series_.AddValues(previous_time_and_values_.value().first, differences);
  }

  previous_time_and_values_ = std::make_pair(timestamp_ns, values);
}

void BasicPagefaultTrack::AddValuesAndUpdateAnnotations(uint64_t timestamp_ns,
                                                        const std::array<double, 3>& values) {
  AddValues(timestamp_ns, values);
  SetValueUpperBound(absl::StrFormat("Maximum count: %.0f", GetGraphMaxValue()),
                     GetGraphMaxValue());
  SetValueLowerBound(absl::StrFormat("Minimum count: %.0f", GetGraphMinValue()),
                     GetGraphMinValue());
}

bool BasicPagefaultTrack::IsCollapsed() const {
  return collapse_toggle_->IsCollapsed() || GetParent()->IsCollapsed();
}

void BasicPagefaultTrack::Draw(Batcher& batcher, TextRenderer& text_renderer,
                               uint64_t current_mouse_time_ns, PickingMode picking_mode,
                               float z_offset) {
  LineGraphTrack<3>::Draw(batcher, text_renderer, current_mouse_time_ns, picking_mode, z_offset);

  if (picking_mode != PickingMode::kNone || this->IsCollapsed()) return;
  AnnotationTrack::DrawAnnotation(batcher, text_renderer, this->layout_,
                                  GlCanvas::kZValueTrackText + z_offset);
}

void BasicPagefaultTrack::DrawSingleSeriesEntry(
    Batcher* batcher, uint64_t start_tick, uint64_t end_tick,
    const std::array<float, 3>& current_normalized_values,
    const std::array<float, 3>& next_normalized_values, float z) {
  LineGraphTrack<3>::DrawSingleSeriesEntry(batcher, start_tick, end_tick, current_normalized_values,
                                           next_normalized_values, z);
  if (!index_of_series_to_highlight_.has_value()) return;
  if (current_normalized_values[index_of_series_to_highlight_.value()] == 0) return;

  const Color kHightlightingColor(231, 68, 53, 100);
  float x0 = time_graph_->GetWorldFromTick(start_tick);
  float width = time_graph_->GetWorldFromTick(end_tick) - x0;
  float content_height =
      size_[1] - layout_->GetTrackTabHeight() - layout_->GetTrackBottomMargin() - GetLegendHeight();
  float y0 = pos_[1] - size_[1] + layout_->GetTrackBottomMargin();
  batcher->AddShadedBox(Vec2(x0, y0), Vec2(width, content_height), z, kHightlightingColor);
}

}  // namespace orbit_gl