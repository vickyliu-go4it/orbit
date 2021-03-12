// Copyright (c) 2020 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ORBIT_CAPTURE_CLIENT_CAPTURE_LISTENER_H_
#define ORBIT_CAPTURE_CLIENT_CAPTURE_LISTENER_H_

#include "OrbitBase/Result.h"
#include "OrbitClientData/Callstack.h"
#include "OrbitClientData/ProcessData.h"
#include "OrbitClientData/TracepointCustom.h"
#include "OrbitClientData/UserDefinedCaptureData.h"
#include "absl/container/flat_hash_set.h"
#include "capture.pb.h"
#include "capture_data.pb.h"

class CaptureListener {
 public:
  enum class CaptureOutcome { kComplete, kCancelled };

  virtual ~CaptureListener() = default;

  // Called after capture started but before the first event arrived.
  virtual void OnCaptureStarted(
      ProcessData&& process,
      absl::flat_hash_map<uint64_t, orbit_client_protos::FunctionInfo> instrumented_functions,
      TracepointInfoSet selected_tracepoints,
      absl::flat_hash_set<uint64_t> frame_track_function_ids) = 0;

  virtual void OnTimer(const orbit_client_protos::TimerInfo& timer_info) = 0;
  virtual void OnSystemMemoryUsage(
      const orbit_grpc_protos::SystemMemoryUsage& system_memory_usage) = 0;
  virtual void OnKeyAndString(uint64_t key, std::string str) = 0;
  virtual void OnUniqueCallStack(CallStack callstack) = 0;
  virtual void OnCallstackEvent(orbit_client_protos::CallstackEvent callstack_event) = 0;
  virtual void OnThreadName(int32_t thread_id, std::string thread_name) = 0;
  virtual void OnThreadStateSlice(orbit_client_protos::ThreadStateSliceInfo thread_state_slice) = 0;
  virtual void OnAddressInfo(orbit_client_protos::LinuxAddressInfo address_info) = 0;
  virtual void OnUniqueTracepointInfo(uint64_t key,
                                      orbit_grpc_protos::TracepointInfo tracepoint_info) = 0;
  virtual void OnTracepointEvent(
      orbit_client_protos::TracepointEventInfo tracepoint_event_info) = 0;
};

#endif  // ORBIT_GL_CAPTURE_LISTENER_H_
