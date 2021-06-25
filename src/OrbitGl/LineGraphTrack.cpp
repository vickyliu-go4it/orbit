// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "LineGraphTrack.h"

#include <GteVector.h>
#include <absl/strings/str_format.h>
#include <absl/strings/str_split.h>

#include <algorithm>
#include <cstdint>
#include <iterator>

#include "Geometry.h"
#include "GlCanvas.h"
#include "TextRenderer.h"
#include "TimeGraph.h"
#include "TimeGraphLayout.h"
#include "Viewport.h"

template <size_t Dimension>
float LineGraphTrack<Dimension>::GetLabelYFromValues(
    const std::array<double, Dimension>& values) const {
  float content_height =
      size_[1] - layout_->GetTrackTabHeight() - layout_->GetTrackBottomMargin() - GetLegendHeight();
  float base_y = pos_[1] - size_[1] + layout_->GetTrackBottomMargin();
  double min = GetGraphMinValue();
  double inverse_value_range = GetGraphInverseValueRange();
  std::array<float, Dimension> normalized_values;
  std::transform(values.begin(), values.end(), normalized_values.begin(),
                 [min, inverse_value_range](double value) {
                   return static_cast<float>((value - min) * inverse_value_range);
                 });

  if (Dimension == 1) return base_y + normalized_values[0] * content_height;
  return base_y + *std::max_element(normalized_values.begin(), normalized_values.end()) *
                      content_height / 2.0f;
}

template <size_t Dimension>
void LineGraphTrack<Dimension>::DrawSeries(Batcher* batcher, uint64_t min_tick, uint64_t max_tick,
                                           float z) {
  auto entries_affected_range_result = series_.GetEntriesAffectedByTimeRange(min_tick, max_tick);
  if (!entries_affected_range_result.has_value()) return;

  typename MultivariateTimeSeries<Dimension>::Range& entries{entries_affected_range_result.value()};

  double min = GetGraphMinValue();
  double inverse_value_range = GetGraphInverseValueRange();

  auto current_iterator = entries.begin;
  uint64_t current_time = current_iterator->first;
  std::array<float, Dimension> current_normalized_values;
  std::transform(current_iterator->second.begin(), current_iterator->second.end(),
                 current_normalized_values.begin(), [min, inverse_value_range](double value) {
                   return static_cast<float>((value - min) * inverse_value_range);
                 });

  while (current_iterator != entries.end) {
    auto next_iterator = std::next(current_iterator);
    uint64_t next_time = next_iterator->first;
    std::array<float, Dimension> next_normalized_values;
    std::transform(next_iterator->second.begin(), next_iterator->second.end(),
                   next_normalized_values.begin(), [min, inverse_value_range](double value) {
                     return static_cast<float>((value - min) * inverse_value_range);
                   });

    DrawSingleSeriesEntry(batcher, current_time, next_time, current_normalized_values,
                          next_normalized_values, z);

    current_iterator = next_iterator;
    current_time = next_time;
    current_normalized_values = next_normalized_values;
  }
}

template <size_t Dimension>
void LineGraphTrack<Dimension>::DrawSingleSeriesEntry(
    Batcher* batcher, uint64_t start_tick, uint64_t end_tick,
    const std::array<float, Dimension>& current_normalized_values,
    const std::array<float, Dimension>& next_normalized_values, float z) {
  constexpr float kDotRadius = 2.f;
  float x0 = time_graph_->GetWorldFromTick(start_tick);
  float x1 = time_graph_->GetWorldFromTick(end_tick);
  float content_height =
      size_[1] - layout_->GetTrackTabHeight() - layout_->GetTrackBottomMargin() - GetLegendHeight();
  float base_y = pos_[1] - size_[1] + layout_->GetTrackBottomMargin();

  for (size_t i = 0; i < Dimension; ++i) {
    float y0 = base_y + current_normalized_values[i] * content_height;
    float y1 = base_y + next_normalized_values[i] * content_height;
    batcher->AddLine(Vec2(x0, y0), Vec2(x1, y0), z, GetColor(i));
    batcher->AddLine(Vec2(x1, y0), Vec2(x1, y1), z, GetColor(i));
    DrawSquareDot(batcher, Vec2(x0, y0), kDotRadius, z, GetColor(i));
  }
}

template <size_t Dimension>
void LineGraphTrack<Dimension>::DrawSquareDot(Batcher* batcher, Vec2 center, float radius, float z,
                                              const Color& color) {
  Vec2 position(center[0] - radius, center[1] - radius);
  Vec2 size(2 * radius, 2 * radius);
  batcher->AddBox(Box(position, size, z), color);
}

template class LineGraphTrack<1>;
template class LineGraphTrack<2>;
template class LineGraphTrack<3>;
template class LineGraphTrack<4>;