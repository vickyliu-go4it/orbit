// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_GL_TRACK_MANAGER_H_
#define ORBIT_GL_TRACK_MANAGER_H_

#include <stdint.h>
#include <stdlib.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "AsyncTrack.h"
#include "FrameTrack.h"
#include "GpuTrack.h"
#include "GraphTrack.h"
#include "PickingManager.h"
#include "SchedulerTrack.h"
#include "StringManager.h"
#include "ThreadTrack.h"
#include "Timer.h"
#include "Track.h"
#include "capture_data.pb.h"

class OrbitApp;
class Timegraph;

// TrackManager is in charge of the active Tracks in Timegraph (their creation, searching, erasing
// and sorting).
class TrackManager {
 public:
  explicit TrackManager(TimeGraph* time_graph, TimeGraphLayout* layout, OrbitApp* app,
                        const CaptureData* capture_data);

  [[nodiscard]] std::vector<Track*> GetAllTracks() const;
  [[nodiscard]] std::vector<Track*> GetVisibleTracks() const { return visible_tracks_; }
  [[nodiscard]] std::vector<ThreadTrack*> GetThreadTracks() const;
  [[nodiscard]] std::vector<FrameTrack*> GetFrameTracks() const;

  [[nodiscard]] ThreadTrack* GetTracepointsSystemWideTrack() const {
    return tracepoints_system_wide_track_;
  }

  void SortTracks();
  void SetFilter(const std::string& filter);

  void UpdateTracks(Batcher* batcher, uint64_t min_tick, uint64_t max_tick,
                    PickingMode picking_mode);
  [[nodiscard]] float GetTracksTotalHeight() const { return tracks_total_height_; }

  SchedulerTrack* GetOrCreateSchedulerTrack();
  ThreadTrack* GetOrCreateThreadTrack(int32_t tid);
  GpuTrack* GetOrCreateGpuTrack(uint64_t timeline_hash);
  GraphTrack* GetOrCreateGraphTrack(
      const std::string& name,
      const std::optional<std::pair<std::string, double>>& warning_threshold = std::nullopt,
      const std::optional<std::pair<std::string, double>>& value_upper_bound = std::nullopt);
  AsyncTrack* GetOrCreateAsyncTrack(const std::string& name);
  FrameTrack* GetOrCreateFrameTrack(uint64_t function_id,
                                    const orbit_client_protos::FunctionInfo& function);

  void AddTrack(const std::shared_ptr<Track>& track);
  void RemoveFrameTrack(uint64_t function_address);

  void UpdateMovingTrackSorting();

 private:
  void UpdateFilteredTrackList();
  [[nodiscard]] int FindMovingTrackIndex();
  [[nodiscard]] std::vector<ThreadTrack*> GetSortedThreadTracks();

  mutable std::recursive_mutex mutex_;

  std::vector<std::shared_ptr<Track>> all_tracks_;
  std::unordered_map<int32_t, std::shared_ptr<ThreadTrack>> thread_tracks_;
  std::map<std::string, std::shared_ptr<AsyncTrack>> async_tracks_;
  std::map<std::string, std::shared_ptr<GraphTrack>> graph_tracks_;
  // Mapping from timeline to GPU tracks. Timeline name is used for stable ordering. In particular
  // we want the marker tracks next to their queue track. E.g. "gfx" and "gfx_markers" should appear
  // next to each other.
  std::map<std::string, std::shared_ptr<GpuTrack>> gpu_tracks_;
  // Mapping from function address to frame tracks.
  // TODO (b/175865913): Use Function info instead of their address as key to FrameTracks
  std::unordered_map<uint64_t, std::shared_ptr<FrameTrack>> frame_tracks_;
  std::shared_ptr<SchedulerTrack> scheduler_track_;
  ThreadTrack* tracepoints_system_wide_track_;

  TimeGraph* time_graph_;
  TimeGraphLayout* layout_;

  std::vector<Track*> sorted_tracks_;
  bool sorting_invalidated_ = false;
  Timer last_thread_reorder_;

  std::string filter_;
  std::vector<Track*> visible_tracks_;

  float tracks_total_height_ = 0.0f;
  const CaptureData* capture_data_ = nullptr;

  OrbitApp* app_ = nullptr;
};

#endif  // ORBIT_GL_TRACK_MANAGER_H_
