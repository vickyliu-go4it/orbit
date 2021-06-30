// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_GL_PAGEFAULT_TRACK_H_
#define ORBIT_GL_PAGEFAULT_TRACK_H_

#include "BasicPagefaultTrack.h"
#include "CoreMath.h"
#include "Timer.h"
#include "Track.h"
#include "Viewport.h"
#include "capture_data.pb.h"

namespace orbit_gl {

class PagefaultTrack : public Track {
 public:
  explicit PagefaultTrack(CaptureViewElement* parent, TimeGraph* time_graph,
                          orbit_gl::Viewport* viewport, TimeGraphLayout* layout,
                          std::array<std::string, 3> series_names,
                          const orbit_client_model::CaptureData* capture_data,
                          uint32_t indentation_level = 0);

  [[nodiscard]] Type GetType() const override { return Type::kPagefaultTrack; }
  [[nodiscard]] float GetHeight() const override;
  [[nodiscard]] std::vector<CaptureViewElement*> GetVisibleChildren() override;

  [[nodiscard]] BasicPagefaultTrack* GetMajorPagefaultTrack() const {
    return major_pagefault_track_.get();
  }
  [[nodiscard]] BasicPagefaultTrack* GetMinorPagefaultTrack() const {
    return minor_pagefault_track_.get();
  }

  [[nodiscard]] bool IsEmpty() const override {
    return major_pagefault_track_->IsEmpty() && minor_pagefault_track_->IsEmpty();
  }
  [[nodiscard]] bool IsCollapsible() const override { return true; }

  void Draw(Batcher& batcher, TextRenderer& text_renderer, uint64_t current_mouse_time_ns,
            PickingMode picking_mode, float z_offset = 0) override;
  void UpdatePrimitives(Batcher* batcher, uint64_t min_tick, uint64_t max_tick,
                        PickingMode picking_mode, float z_offset = 0) override;

  void OnTimer(const orbit_client_protos::TimerInfo& timer_info) override;
  [[nodiscard]] std::vector<std::shared_ptr<TimerChain>> GetAllChains() const override;
  [[nodiscard]] std::vector<std::shared_ptr<TimerChain>> GetAllSerializableChains() const override {
    return GetAllChains();
  }

  void SetNumberOfDecimalDigits(uint8_t value_decimal_digits) {
    major_pagefault_track_->SetNumberOfDecimalDigits(value_decimal_digits);
    minor_pagefault_track_->SetNumberOfDecimalDigits(value_decimal_digits);
  }

 private:
  void UpdatePositionOfSubtracks();

  std::shared_ptr<BasicPagefaultTrack> major_pagefault_track_;
  std::shared_ptr<BasicPagefaultTrack> minor_pagefault_track_;
};

}  // namespace orbit_gl

#endif  // ORBIT_GL_PAGEFAULT_TRACK_H_
