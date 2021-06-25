// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_GL_LINE_GRAPH_TRACK_H_
#define ORBIT_GL_LINE_GRAPH_TRACK_H_

#include <string>
#include <utility>

#include "GraphTrack.h"
#include "Track.h"
#include "Viewport.h"

template <size_t Dimension>
class LineGraphTrack : public GraphTrack<Dimension> {
 public:
  explicit LineGraphTrack(CaptureViewElement* parent, TimeGraph* time_graph,
                          orbit_gl::Viewport* viewport, TimeGraphLayout* layout, std::string name,
                          std::array<std::string, Dimension> series_names,
                          const orbit_client_model::CaptureData* capture_data)
      : GraphTrack<Dimension>(parent, time_graph, viewport, layout, name, series_names,
                              capture_data) {}
  ~LineGraphTrack() override = default;

 protected:
  [[nodiscard]] float GetLabelYFromValues(
      const std::array<double, Dimension>& values) const override;
  void DrawSeries(Batcher* batcher, uint64_t min_tick, uint64_t max_tick, float z) override;
  virtual void DrawSingleSeriesEntry(Batcher* batcher, uint64_t start_tick, uint64_t end_tick,
                                     const std::array<float, Dimension>& current_normalized_values,
                                     const std::array<float, Dimension>& next_normalized_values,
                                     float z);

 private:
  void DrawSquareDot(Batcher* batcher, Vec2 center, float radius, float z, const Color& color);
};

#endif  // ORBIT_GL_LINE_GRAPH_TRACK_H_