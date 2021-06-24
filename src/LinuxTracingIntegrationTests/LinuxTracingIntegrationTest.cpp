// Copyright (c) 2021 The Orbit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <absl/container/flat_hash_set.h>
#include <absl/strings/match.h>
#include <absl/strings/numbers.h>
#include <absl/synchronization/mutex.h>
#include <absl/time/clock.h>
#include <gmock/gmock-matchers.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <array>
#include <thread>
#include <utility>

#include "LinuxTracing/Tracer.h"
#include "LinuxTracing/TracerListener.h"
#include "LinuxTracingIntegrationTestPuppet.h"
#include "ObjectUtils/ElfFile.h"
#include "ObjectUtils/LinuxMap.h"
#include "OrbitBase/ExecutablePath.h"
#include "OrbitBase/Logging.h"
#include "OrbitBase/ReadFileToString.h"
#include "OrbitBase/ThreadUtils.h"
#include "capture.pb.h"

namespace orbit_linux_tracing_integration_tests {
namespace {

[[nodiscard]] bool IsRunningAsRoot() { return geteuid() == 0; }

[[nodiscard]] bool CheckIsRunningAsRoot() {
  if (IsRunningAsRoot()) {
    return true;
  }

  ERROR("Root required for this test");
  return false;
}

[[nodiscard]] int ReadPerfEventParanoid() {
  auto error_or_content = orbit_base::ReadFileToString("/proc/sys/kernel/perf_event_paranoid");
  CHECK(error_or_content.has_value());
  const std::string& content = error_or_content.value();
  int perf_event_paranoid = 2;
  bool atoi_succeeded = absl::SimpleAtoi(content, &perf_event_paranoid);
  CHECK(atoi_succeeded);
  return perf_event_paranoid;
}

[[nodiscard]] bool CheckIsPerfEventParanoidAtMost(int max_perf_event_paranoid) {
  if (IsRunningAsRoot()) {
    return true;
  }

  int perf_event_paranoid = ReadPerfEventParanoid();
  if (perf_event_paranoid <= max_perf_event_paranoid) {
    return true;
  }

  ERROR("Root or max perf_event_paranoid %d (actual is %d) required for this test",
        max_perf_event_paranoid, perf_event_paranoid);
  return false;
}

[[nodiscard]] std::string ReadUnameKernelRelease() {
  utsname utsname{};
  int uname_result = uname(&utsname);
  CHECK(uname_result == 0);
  return utsname.release;
}

[[nodiscard]] bool CheckIsStadiaInstance() {
  std::string release = ReadUnameKernelRelease();
  if (absl::StrContains(release, "-ggp-")) {
    return true;
  }

  ERROR("Stadia instance required for this test (but kernel release is \"%s\")", release);
  return false;
}

class ChildProcess {
 public:
  explicit ChildProcess(const std::function<int()>& child_main) {
    std::array<int, 2> parent_to_child_pipe{};
    CHECK(pipe(parent_to_child_pipe.data()) == 0);

    std::array<int, 2> child_to_parent_pipe{};
    CHECK(pipe(child_to_parent_pipe.data()) == 0);

    pid_t child_pid = fork();
    CHECK(child_pid >= 0);
    if (child_pid > 0) {
      // Parent.
      child_pid_ = child_pid;

      // Close unused ends of the pipes.
      CHECK(close(parent_to_child_pipe[0]) == 0);
      CHECK(close(child_to_parent_pipe[1]) == 0);

      reading_fd_ = child_to_parent_pipe[0];
      writing_fd_ = parent_to_child_pipe[1];

    } else {
      // Child.

      // Close unused ends of the pipes.
      CHECK(close(parent_to_child_pipe[1]) == 0);
      CHECK(close(child_to_parent_pipe[0]) == 0);

      // Redirect reading end of parent_to_child_pipe to stdin and close the pipe's original fd.
      CHECK(close(STDIN_FILENO) == 0);
      CHECK(dup2(parent_to_child_pipe[0], STDIN_FILENO) == STDIN_FILENO);
      CHECK(close(parent_to_child_pipe[0]) == 0);

      // Redirect writing end of child_to_parent_pipe to stdout and close the pipe's original fd.
      CHECK(close(STDOUT_FILENO) == 0);
      CHECK(dup2(child_to_parent_pipe[1], STDOUT_FILENO) == STDOUT_FILENO);
      CHECK(close(child_to_parent_pipe[1]) == 0);

      // Run the child and exit.
      exit(child_main());
    }
  }

  ~ChildProcess() {
    CHECK(close(reading_fd_) == 0);
    CHECK(close(writing_fd_) == 0);

    CHECK(waitpid(child_pid_, nullptr, 0) == child_pid_);
  }

  [[nodiscard]] pid_t GetChildPid() const { return child_pid_; }

  void WriteLine(std::string_view str) {
    std::string string_with_newline = std::string{str}.append("\n");
    CHECK(write(writing_fd_, string_with_newline.c_str(), string_with_newline.length()) ==
          static_cast<ssize_t>(string_with_newline.length()));
  }

  [[nodiscard]] std::string ReadLine() {
    std::string str;
    while (true) {
      char c;
      CHECK(read(reading_fd_, &c, sizeof(c)) == sizeof(c));
      if (c == '\n' || c == '\0') break;
      str.push_back(c);
    }
    return str;
  }

 private:
  pid_t child_pid_ = -1;
  int reading_fd_ = -1;
  int writing_fd_ = -1;
};

class BufferTracerListener : public orbit_linux_tracing::TracerListener {
 public:
  void OnSchedulingSlice(orbit_grpc_protos::SchedulingSlice scheduling_slice) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_scheduling_slice() = std::move(scheduling_slice);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
    {
      absl::MutexLock lock{&one_scheduling_slice_received_mutex_};
      one_scheduling_slice_received_ = true;
    }
  }

  void OnCallstackSample(orbit_grpc_protos::FullCallstackSample callstack_sample) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_full_callstack_sample() = std::move(callstack_sample);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  void OnFunctionCall(orbit_grpc_protos::FunctionCall function_call) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_function_call() = std::move(function_call);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  void OnIntrospectionScope(orbit_grpc_protos::IntrospectionScope introspection_scope) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_introspection_scope() = std::move(introspection_scope);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  void OnGpuJob(orbit_grpc_protos::FullGpuJob full_gpu_job_event) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_full_gpu_job() = std::move(full_gpu_job_event);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  void OnThreadName(orbit_grpc_protos::ThreadName thread_name) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_thread_name() = std::move(thread_name);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  void OnThreadNamesSnapshot(
      orbit_grpc_protos::ThreadNamesSnapshot thread_names_snapshot) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_thread_names_snapshot() = std::move(thread_names_snapshot);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  void OnThreadStateSlice(orbit_grpc_protos::ThreadStateSlice thread_state_slice) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_thread_state_slice() = std::move(thread_state_slice);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  void OnAddressInfo(orbit_grpc_protos::FullAddressInfo address_info) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_full_address_info() = std::move(address_info);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  void OnTracepointEvent(orbit_grpc_protos::FullTracepointEvent tracepoint_event) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_full_tracepoint_event() = std::move(tracepoint_event);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  void OnModuleUpdate(orbit_grpc_protos::ModuleUpdateEvent module_update_event) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_module_update_event() = std::move(module_update_event);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  void OnModulesSnapshot(orbit_grpc_protos::ModulesSnapshot modules_snapshot) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_modules_snapshot() = std::move(modules_snapshot);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  void OnErrorsWithPerfEventOpenEvent(
      orbit_grpc_protos::ErrorsWithPerfEventOpenEvent errors_with_perf_event_open_event) override {
    orbit_grpc_protos::ProducerCaptureEvent event;
    *event.mutable_metadata_event()->mutable_errors_with_perf_event_open_event() =
        std::move(errors_with_perf_event_open_event);
    {
      absl::MutexLock lock{&events_mutex_};
      events_.emplace_back(std::move(event));
    }
  }

  [[nodiscard]] std::vector<orbit_grpc_protos::ProducerCaptureEvent> GetAndClearEvents() {
    absl::MutexLock lock{&events_mutex_};
    std::vector<orbit_grpc_protos::ProducerCaptureEvent> events = std::move(events_);
    events_.clear();
    return events;
  }

  void WaitForAtLeastOneSchedulingSlice() {
    one_scheduling_slice_received_mutex_.LockWhen(absl::Condition(
        +[](bool* one_scheduling_slice_received) { return *one_scheduling_slice_received; },
        &one_scheduling_slice_received_));
    one_scheduling_slice_received_mutex_.Unlock();
  }

 private:
  std::vector<orbit_grpc_protos::ProducerCaptureEvent> events_;
  absl::Mutex events_mutex_;

  bool one_scheduling_slice_received_ = false;
  absl::Mutex one_scheduling_slice_received_mutex_;
};

// GTest's test fixtures seem to mess with the pipe logic in ChildProcess.
// Let's make the fixture ourselves, which is also advised in http://go/totw/122.
class LinuxTracingIntegrationTestFixture {
 public:
  LinuxTracingIntegrationTestFixture() : puppet_{&LinuxTracingIntegrationTestPuppetMain} {}

  [[nodiscard]] pid_t GetPuppetPid() const { return puppet_.GetChildPid(); }

  void WriteLineToPuppet(std::string_view str) { puppet_.WriteLine(str); }

  [[nodiscard]] std::string ReadLineFromPuppet() { return puppet_.ReadLine(); }

  [[nodiscard]] orbit_grpc_protos::CaptureOptions BuildDefaultCaptureOptions() {
    orbit_grpc_protos::CaptureOptions capture_options;
    capture_options.set_trace_context_switches(true);
    capture_options.set_pid(puppet_.GetChildPid());
    capture_options.set_samples_per_second(1000.0);
    capture_options.set_stack_dump_size(65000);
    capture_options.set_unwinding_method(orbit_grpc_protos::CaptureOptions::kDwarf);
    capture_options.set_trace_thread_state(true);
    capture_options.set_trace_gpu_driver(true);
    return capture_options;
  }

  void StartTracingAndWaitForTracingLoopStarted(orbit_grpc_protos::CaptureOptions capture_options) {
    CHECK(!tracer_.has_value());
    CHECK(!listener_.has_value());

    if (IsRunningAsRoot()) {
      // Needed for BufferTracerListener::WaitForAtLeastOneSchedulingSlice().
      CHECK(capture_options.trace_context_switches());
    }

    tracer_.emplace(std::move(capture_options));
    listener_.emplace();
    tracer_->SetListener(&*listener_);
    tracer_->Start();

    if (IsRunningAsRoot()) {
      // Waiting for the first SchedulingSlice (at least one of which is always expected as long as
      // capture_options.trace_context_switches() is true) guarantees that the main loop in
      // TracerThread has started, and hence that the capture has been fully set up.
      listener_->WaitForAtLeastOneSchedulingSlice();
    } else {
      // Some tests verify events that don't require root, but SchedulingSlices do need root.
      // So when running those tests without being root, sleep for a long time instead of waiting
      // for the first SchedulingSlice.
      absl::SleepFor(absl::Milliseconds(2500));
    }
  }

  [[nodiscard]] std::vector<orbit_grpc_protos::ProducerCaptureEvent> StopTracingAndGetEvents() {
    CHECK(tracer_.has_value());
    CHECK(listener_.has_value());
    tracer_->Stop();
    tracer_.reset();
    std::vector<orbit_grpc_protos::ProducerCaptureEvent> events = listener_->GetAndClearEvents();
    listener_.reset();
    return events;
  }

 private:
  ChildProcess puppet_;
  std::optional<orbit_linux_tracing::Tracer> tracer_ = std::nullopt;
  std::optional<BufferTracerListener> listener_ = std::nullopt;
};

using PuppetConstants = LinuxTracingIntegrationTestPuppetConstants;

[[nodiscard]] std::vector<orbit_grpc_protos::ProducerCaptureEvent> TraceAndGetEvents(
    LinuxTracingIntegrationTestFixture* fixture, std::string_view command,
    std::optional<orbit_grpc_protos::CaptureOptions> capture_options = std::nullopt) {
  CHECK(fixture != nullptr);
  if (!capture_options.has_value()) {
    capture_options = fixture->BuildDefaultCaptureOptions();
  }

  fixture->StartTracingAndWaitForTracingLoopStarted(capture_options.value());

  fixture->WriteLineToPuppet(command);
  while (fixture->ReadLineFromPuppet() != PuppetConstants::kDoneResponse) continue;

  constexpr absl::Duration kSleepBeforeStopTracing = absl::Milliseconds(100);
  absl::SleepFor(kSleepBeforeStopTracing);
  return fixture->StopTracingAndGetEvents();
}

std::filesystem::path GetExecutableBinaryPath(pid_t pid) {
  auto error_or_executable_path = orbit_base::GetExecutablePath(pid);
  CHECK(error_or_executable_path.has_value());
  return error_or_executable_path.value();
}

orbit_grpc_protos::ModuleSymbols GetExecutableBinaryModuleSymbols(pid_t pid) {
  const std::filesystem::path& executable_path = GetExecutableBinaryPath(pid);

  auto error_or_elf_file = orbit_object_utils::CreateElfFile(executable_path.string());
  CHECK(error_or_elf_file.has_value());
  const std::unique_ptr<orbit_object_utils::ElfFile>& elf_file = error_or_elf_file.value();

  auto error_or_module = elf_file->LoadDebugSymbols();
  CHECK(error_or_module.has_value());
  return error_or_module.value();
}

orbit_grpc_protos::ModuleInfo GetExecutableBinaryModuleInfo(pid_t pid) {
  auto error_or_module_infos = orbit_object_utils::ReadModules(pid);
  CHECK(error_or_module_infos.has_value());
  const std::vector<orbit_grpc_protos::ModuleInfo>& module_infos = error_or_module_infos.value();

  auto error_or_executable_path = orbit_base::GetExecutablePath(pid);
  CHECK(error_or_executable_path.has_value());
  const std::filesystem::path& executable_path = error_or_executable_path.value();

  const orbit_grpc_protos::ModuleInfo* executable_module_info = nullptr;
  for (const auto& module_info : module_infos) {
    if (module_info.file_path() == executable_path) {
      executable_module_info = &module_info;
      break;
    }
  }
  CHECK(executable_module_info != nullptr);
  return *executable_module_info;
}

void VerifyOrderOfAllEvents(const std::vector<orbit_grpc_protos::ProducerCaptureEvent>& events) {
  uint64_t previous_event_timestamp_ns = 0;
  for (const auto& event : events) {
    switch (event.event_case()) {
      case orbit_grpc_protos::ProducerCaptureEvent::kCaptureStarted:
        // LinuxTracingHandler does not send this event.
        UNREACHABLE();
      case orbit_grpc_protos::ProducerCaptureEvent::kSchedulingSlice:
        EXPECT_GE(event.scheduling_slice().out_timestamp_ns(), previous_event_timestamp_ns);
        previous_event_timestamp_ns = event.scheduling_slice().out_timestamp_ns();
        break;
      case orbit_grpc_protos::ProducerCaptureEvent::kInternedCallstack:
        UNREACHABLE();
      case orbit_grpc_protos::ProducerCaptureEvent::kCallstackSample:
        UNREACHABLE();
      case orbit_grpc_protos::ProducerCaptureEvent::kFullCallstackSample:
        EXPECT_GE(event.full_callstack_sample().timestamp_ns(), previous_event_timestamp_ns);
        previous_event_timestamp_ns = event.full_callstack_sample().timestamp_ns();
        break;
      case orbit_grpc_protos::ProducerCaptureEvent::kFullTracepointEvent:
        UNREACHABLE();
      case orbit_grpc_protos::ProducerCaptureEvent::kFunctionCall:
        EXPECT_GE(event.function_call().end_timestamp_ns(), previous_event_timestamp_ns);
        previous_event_timestamp_ns = event.function_call().end_timestamp_ns();
        break;
      case orbit_grpc_protos::ProducerCaptureEvent::kIntrospectionScope:
        UNREACHABLE();
      case orbit_grpc_protos::ProducerCaptureEvent::kInternedString:
        UNREACHABLE();
      case orbit_grpc_protos::ProducerCaptureEvent::kModulesSnapshot:
        EXPECT_GE(event.modules_snapshot().timestamp_ns(), previous_event_timestamp_ns);
        previous_event_timestamp_ns = event.modules_snapshot().timestamp_ns();
        break;
      case orbit_grpc_protos::ProducerCaptureEvent::kFullGpuJob:
        EXPECT_GE(event.full_gpu_job().dma_fence_signaled_time_ns(), previous_event_timestamp_ns);
        previous_event_timestamp_ns = event.full_gpu_job().dma_fence_signaled_time_ns();
        break;
      case orbit_grpc_protos::ProducerCaptureEvent::kThreadName:
        EXPECT_GE(event.thread_name().timestamp_ns(), previous_event_timestamp_ns);
        previous_event_timestamp_ns = event.thread_name().timestamp_ns();
        break;
      case orbit_grpc_protos::ProducerCaptureEvent::kThreadNamesSnapshot:
        EXPECT_GE(event.thread_names_snapshot().timestamp_ns(), previous_event_timestamp_ns);
        previous_event_timestamp_ns = event.thread_names_snapshot().timestamp_ns();
        break;
      case orbit_grpc_protos::ProducerCaptureEvent::kThreadStateSlice:
        EXPECT_GE(event.thread_state_slice().end_timestamp_ns(), previous_event_timestamp_ns);
        previous_event_timestamp_ns = event.thread_state_slice().end_timestamp_ns();
        break;
      case orbit_grpc_protos::ProducerCaptureEvent::kFullAddressInfo:
        // AddressInfos have no timestamp.
        break;
      case orbit_grpc_protos::ProducerCaptureEvent::kGpuQueueSubmission:
        UNREACHABLE();
      case orbit_grpc_protos::ProducerCaptureEvent::kModuleUpdateEvent:
        EXPECT_GE(event.module_update_event().timestamp_ns(), previous_event_timestamp_ns);
        previous_event_timestamp_ns = event.module_update_event().timestamp_ns();
        break;
      case orbit_grpc_protos::ProducerCaptureEvent::kMemoryEventWrapper:
        // Cases of memory events are tested in MemoryTracingIntegrationTest.
        UNREACHABLE();
      case orbit_grpc_protos::ProducerCaptureEvent::kApiEvent:
        UNREACHABLE();
      case orbit_grpc_protos::ProducerCaptureEvent::kMetadataEvent: {
        const orbit_grpc_protos::MetadataEvent& metadata_event = event.metadata_event();
        switch (metadata_event.event_case()) {
          case orbit_grpc_protos::MetadataEvent::kWarningEvent:
            UNREACHABLE();
          case orbit_grpc_protos::MetadataEvent::kInfoEvent:
            UNREACHABLE();
          case orbit_grpc_protos::MetadataEvent::kErrorEnablingOrbitApiEvent:
            UNREACHABLE();
          case orbit_grpc_protos::MetadataEvent::kClockResolutionEvent:
            UNREACHABLE();
          case orbit_grpc_protos::MetadataEvent::kErrorsWithPerfEventOpenEvent:
            EXPECT_GE(metadata_event.errors_with_perf_event_open_event().timestamp_ns(),
                      previous_event_timestamp_ns);
            previous_event_timestamp_ns =
                metadata_event.errors_with_perf_event_open_event().timestamp_ns();
            break;
          case orbit_grpc_protos::MetadataEvent::EVENT_NOT_SET:
            UNREACHABLE();
        }
      } break;
      case orbit_grpc_protos::ProducerCaptureEvent::EVENT_NOT_SET:
        UNREACHABLE();
    }
  }
}

void VerifyErrorsWithPerfEventOpenEvent(
    const std::vector<orbit_grpc_protos::ProducerCaptureEvent>& events) {
  bool errors_with_perf_event_open_event_found = false;
  for (const auto& event : events) {
    if (event.event_case() == orbit_grpc_protos::ProducerCaptureEvent::kMetadataEvent &&
        event.metadata_event().event_case() ==
            orbit_grpc_protos::MetadataEvent::kErrorsWithPerfEventOpenEvent) {
      EXPECT_FALSE(errors_with_perf_event_open_event_found);
      errors_with_perf_event_open_event_found = true;
    }
  }
  EXPECT_EQ(errors_with_perf_event_open_event_found, !IsRunningAsRoot());
}

TEST(LinuxTracingIntegrationTest, SchedulingSlices) {
  if (!CheckIsRunningAsRoot()) {
    GTEST_SKIP();
  }
  LinuxTracingIntegrationTestFixture fixture;

  std::vector<orbit_grpc_protos::ProducerCaptureEvent> events =
      TraceAndGetEvents(&fixture, PuppetConstants::kSleepCommand);

  VerifyOrderOfAllEvents(events);

  uint64_t scheduling_slice_count = 0;
  uint64_t last_out_timestamp_ns = 0;
  for (const auto& event : events) {
    if (event.event_case() != orbit_grpc_protos::ProducerCaptureEvent::kSchedulingSlice) {
      continue;
    }

    const orbit_grpc_protos::SchedulingSlice& scheduling_slice = event.scheduling_slice();
    if (scheduling_slice.pid() != fixture.GetPuppetPid()) {
      continue;
    }

    ++scheduling_slice_count;

    // The puppet is not expected to spawn new threads.
    EXPECT_EQ(scheduling_slice.tid(), scheduling_slice.pid());

    EXPECT_GT(scheduling_slice.duration_ns(), 0);
    // SchedulingSlices are expected to be in order of out_timestamp_ns across all CPUs.
    EXPECT_GT(scheduling_slice.out_timestamp_ns(), last_out_timestamp_ns);
    last_out_timestamp_ns = scheduling_slice.out_timestamp_ns();
  }

  LOG("scheduling_slice_count=%lu", scheduling_slice_count);
  // "- 1" as it is the expected number of SchedulingSlices *only between* the first and last sleep.
  EXPECT_GE(scheduling_slice_count, PuppetConstants::kSleepCount - 1);
}

void AddOuterAndInnerFunctionToCaptureOptions(orbit_grpc_protos::CaptureOptions* capture_options,
                                              pid_t pid, uint64_t outer_function_id,
                                              uint64_t inner_function_id) {
  // Find the offset in the ELF file of the "outer" function and the "inner" function and add those
  // functions to the CaptureOptions to be instrumented.
  const orbit_grpc_protos::ModuleSymbols& module_symbols = GetExecutableBinaryModuleSymbols(pid);
  const std::filesystem::path& executable_path = GetExecutableBinaryPath(pid);

  bool outer_function_symbol_found = false;
  bool inner_function_symbol_found = false;
  for (const orbit_grpc_protos::SymbolInfo& symbol : module_symbols.symbol_infos()) {
    if (symbol.name() == PuppetConstants::kOuterFunctionName) {
      CHECK(!outer_function_symbol_found);
      outer_function_symbol_found = true;
      orbit_grpc_protos::InstrumentedFunction instrumented_function;
      instrumented_function.set_file_path(executable_path);
      instrumented_function.set_file_offset(symbol.address() - module_symbols.load_bias());
      instrumented_function.set_function_id(outer_function_id);
      instrumented_function.set_function_name(symbol.name());
      capture_options->mutable_instrumented_functions()->Add(std::move(instrumented_function));
    }

    if (symbol.name() == PuppetConstants::kInnerFunctionName) {
      CHECK(!inner_function_symbol_found);
      inner_function_symbol_found = true;
      orbit_grpc_protos::InstrumentedFunction instrumented_function;
      instrumented_function.set_file_path(executable_path);
      instrumented_function.set_file_offset(symbol.address() - module_symbols.load_bias());
      instrumented_function.set_function_id(inner_function_id);
      instrumented_function.set_function_name(symbol.name());
      capture_options->mutable_instrumented_functions()->Add(std::move(instrumented_function));
    }
  }
  CHECK(outer_function_symbol_found);
  CHECK(inner_function_symbol_found);
}

void VerifyFunctionCallsOfOuterAndInnerFunction(
    const std::vector<orbit_grpc_protos::ProducerCaptureEvent>& events, pid_t pid,
    uint64_t outer_function_id, uint64_t inner_function_id) {
  std::vector<orbit_grpc_protos::FunctionCall> function_calls;
  for (const auto& event : events) {
    if (event.event_case() != orbit_grpc_protos::ProducerCaptureEvent::kFunctionCall) {
      continue;
    }

    const orbit_grpc_protos::FunctionCall& function_call = event.function_call();
    ASSERT_EQ(function_call.pid(), pid);
    ASSERT_EQ(function_call.tid(), pid);
    function_calls.emplace_back(function_call);
  }

  // We expect an ordered sequence of kInnerFunctionCallCount calls to the "inner" function followed
  // by one call to the "outer" function, repeated kOuterFunctionCallCount times.
  ASSERT_EQ(function_calls.size(), PuppetConstants::kOuterFunctionCallCount +
                                       PuppetConstants::kOuterFunctionCallCount *
                                           PuppetConstants::kInnerFunctionCallCount);
  size_t function_call_index = 0;
  for (size_t outer_index = 0; outer_index < PuppetConstants::kOuterFunctionCallCount;
       ++outer_index) {
    uint64_t inner_calls_duration_ns_sum = 0;
    for (size_t inner_index = 0; inner_index < PuppetConstants::kInnerFunctionCallCount;
         ++inner_index) {
      const orbit_grpc_protos::FunctionCall& function_call = function_calls[function_call_index];
      EXPECT_EQ(function_call.function_id(), inner_function_id);
      EXPECT_GT(function_call.duration_ns(), 0);
      inner_calls_duration_ns_sum += function_call.duration_ns();
      if (function_call_index > 0) {
        EXPECT_GT(function_call.end_timestamp_ns(),
                  function_calls[function_call_index - 1].end_timestamp_ns());
      }
      EXPECT_EQ(function_call.depth(), 1);
      ++function_call_index;
    }

    {
      const orbit_grpc_protos::FunctionCall& function_call = function_calls[function_call_index];
      EXPECT_EQ(function_call.function_id(), outer_function_id);
      EXPECT_GT(function_call.duration_ns(), inner_calls_duration_ns_sum);
      if (function_call_index > 0) {
        EXPECT_GT(function_call.end_timestamp_ns(),
                  function_calls[function_call_index - 1].end_timestamp_ns());
      }
      EXPECT_EQ(function_call.depth(), 0);
      ++function_call_index;
    }
  }
}

TEST(LinuxTracingIntegrationTest, FunctionCalls) {
  if (!CheckIsRunningAsRoot()) {
    GTEST_SKIP();
  }
  LinuxTracingIntegrationTestFixture fixture;

  orbit_grpc_protos::CaptureOptions capture_options = fixture.BuildDefaultCaptureOptions();
  constexpr uint64_t kOuterFunctionId = 1;
  constexpr uint64_t kInnerFunctionId = 2;
  AddOuterAndInnerFunctionToCaptureOptions(&capture_options, fixture.GetPuppetPid(),
                                           kOuterFunctionId, kInnerFunctionId);

  std::vector<orbit_grpc_protos::ProducerCaptureEvent> events =
      TraceAndGetEvents(&fixture, PuppetConstants::kCallOuterFunctionCommand, capture_options);

  VerifyOrderOfAllEvents(events);

  VerifyFunctionCallsOfOuterAndInnerFunction(events, fixture.GetPuppetPid(), kOuterFunctionId,
                                             kInnerFunctionId);
}

std::pair<std::pair<uint64_t, uint64_t>, std::pair<uint64_t, uint64_t>>
GetOuterAndInnerFunctionVirtualAddressRanges(pid_t pid) {
  const orbit_grpc_protos::ModuleInfo& module_info = GetExecutableBinaryModuleInfo(pid);
  const orbit_grpc_protos::ModuleSymbols& module_symbols = GetExecutableBinaryModuleSymbols(pid);

  uint64_t outer_function_virtual_address_start = 0;
  uint64_t outer_function_virtual_address_end = 0;
  uint64_t inner_function_virtual_address_start = 0;
  uint64_t inner_function_virtual_address_end = 0;
  for (const orbit_grpc_protos::SymbolInfo& symbol : module_symbols.symbol_infos()) {
    if (symbol.name() == PuppetConstants::kOuterFunctionName) {
      CHECK(outer_function_virtual_address_start == 0 && outer_function_virtual_address_end == 0);
      outer_function_virtual_address_start = module_info.address_start() + symbol.address();
      outer_function_virtual_address_end = outer_function_virtual_address_start + symbol.size() - 1;
    }

    if (symbol.name() == PuppetConstants::kInnerFunctionName) {
      CHECK(inner_function_virtual_address_start == 0 && inner_function_virtual_address_end == 0);
      inner_function_virtual_address_start = module_info.address_start() + symbol.address();
      inner_function_virtual_address_end = inner_function_virtual_address_start + symbol.size() - 1;
    }
  }
  CHECK(outer_function_virtual_address_start != 0);
  CHECK(outer_function_virtual_address_end != 0);
  CHECK(inner_function_virtual_address_start != 0);
  CHECK(inner_function_virtual_address_end != 0);
  return std::make_pair(
      std::make_pair(outer_function_virtual_address_start, outer_function_virtual_address_end),
      std::make_pair(inner_function_virtual_address_start, inner_function_virtual_address_end));
}

absl::flat_hash_set<uint64_t> VerifyAndGetAddressInfosWithOuterAndInnerFunction(
    const std::vector<orbit_grpc_protos::ProducerCaptureEvent>& events,
    const std::filesystem::path& executable_path,
    std::pair<uint64_t, uint64_t> outer_function_virtual_address_range,
    std::pair<uint64_t, uint64_t> inner_function_virtual_address_range) {
  absl::flat_hash_set<uint64_t> address_infos_received;
  for (const auto& event : events) {
    if (event.event_case() != orbit_grpc_protos::ProducerCaptureEvent::kFullAddressInfo) {
      continue;
    }

    const orbit_grpc_protos::FullAddressInfo& address_info = event.full_address_info();
    if (!(address_info.absolute_address() >= outer_function_virtual_address_range.first &&
          address_info.absolute_address() <= outer_function_virtual_address_range.second) &&
        !(address_info.absolute_address() >= inner_function_virtual_address_range.first &&
          address_info.absolute_address() <= inner_function_virtual_address_range.second)) {
      continue;
    }
    address_infos_received.emplace(address_info.absolute_address());

    if (address_info.absolute_address() >= outer_function_virtual_address_range.first &&
        address_info.absolute_address() <= outer_function_virtual_address_range.second) {
      EXPECT_EQ(address_info.function_name(), PuppetConstants::kOuterFunctionName);
      EXPECT_EQ(address_info.offset_in_function(),
                address_info.absolute_address() - outer_function_virtual_address_range.first);
    } else if (address_info.absolute_address() >= inner_function_virtual_address_range.first &&
               address_info.absolute_address() <= inner_function_virtual_address_range.second) {
      EXPECT_EQ(address_info.function_name(), PuppetConstants::kInnerFunctionName);
      EXPECT_EQ(address_info.offset_in_function(),
                address_info.absolute_address() - inner_function_virtual_address_range.first);
    }

    EXPECT_EQ(address_info.module_name(), executable_path.string());
  }
  return address_infos_received;
}

void VerifyCallstackSamplesWithOuterAndInnerFunction(
    const std::vector<orbit_grpc_protos::ProducerCaptureEvent>& events, pid_t pid,
    std::pair<uint64_t, uint64_t> outer_function_virtual_address_range,
    std::pair<uint64_t, uint64_t> inner_function_virtual_address_range, double sampling_rate,
    const absl::flat_hash_set<uint64_t>* address_infos_received, bool unwound_with_frame_pointers) {
  uint64_t previous_callstack_timestamp_ns = 0;
  size_t matching_callstack_count = 0;
  uint64_t first_matching_callstack_timestamp_ns = std::numeric_limits<uint64_t>::max();
  uint64_t last_matching_callstack_timestamp_ns = 0;
  for (const auto& event : events) {
    if (event.event_case() != orbit_grpc_protos::ProducerCaptureEvent::kFullCallstackSample) {
      continue;
    }

    const orbit_grpc_protos::FullCallstackSample& callstack_sample = event.full_callstack_sample();

    // All CallstackSamples should be ordered by timestamp.
    EXPECT_GT(callstack_sample.timestamp_ns(), previous_callstack_timestamp_ns);
    previous_callstack_timestamp_ns = callstack_sample.timestamp_ns();

    // We don't expect other reasons for broken callstacks other than these.
    std::vector<orbit_grpc_protos::Callstack::CallstackType> expected_callstack_types;
    if (unwound_with_frame_pointers) {
      expected_callstack_types = {orbit_grpc_protos::Callstack::kComplete,
                                  orbit_grpc_protos::Callstack::kFramePointerUnwindingError,
                                  orbit_grpc_protos::Callstack::kStackTopForDwarfUnwindingTooSmall,
                                  orbit_grpc_protos::Callstack::kStackTopDwarfUnwindingError,
                                  orbit_grpc_protos::Callstack::kInUprobes};
    } else {
      expected_callstack_types = {orbit_grpc_protos::Callstack::kComplete,
                                  orbit_grpc_protos::Callstack::kDwarfUnwindingError,
                                  orbit_grpc_protos::Callstack::kInUprobes};
    }
    EXPECT_THAT(expected_callstack_types, ::testing::Contains(callstack_sample.callstack().type()));

    // We are only sampling the puppet.
    EXPECT_EQ(callstack_sample.pid(), pid);
    // The puppet is expected single-threaded.
    ASSERT_EQ(callstack_sample.tid(), pid);

    if (callstack_sample.callstack().type() != orbit_grpc_protos::Callstack::kComplete) {
      LOG("callstack_sample.callstack().type() == %s",
          orbit_grpc_protos::Callstack::CallstackType_Name(callstack_sample.callstack().type()));
      continue;
    }

    const orbit_grpc_protos::Callstack& callstack = callstack_sample.callstack();
    for (int32_t pc_index = 0; pc_index < callstack.pcs_size(); ++pc_index) {
      // We found one of the callstacks we are looking for: it contains the "inner" function's
      // address and the caller address should match the "outer" function's address.
      if (callstack.pcs(pc_index) >= inner_function_virtual_address_range.first &&
          callstack.pcs(pc_index) <= inner_function_virtual_address_range.second) {
        if (address_infos_received != nullptr) {
          // Verify that we got the AddressInfo for this virtual address of the "inner" function.
          EXPECT_TRUE(address_infos_received->contains(callstack.pcs(pc_index)));
        }

        // Verify that the caller of the "inner" function is the "outer" function.
        ASSERT_LT(pc_index + 1, callstack.pcs_size());
        ASSERT_TRUE(callstack.pcs(pc_index + 1) >= outer_function_virtual_address_range.first &&
                    callstack.pcs(pc_index + 1) <= outer_function_virtual_address_range.second);

        if (address_infos_received != nullptr) {
          // Verify that we got the AddressInfo for this virtual address of the "outer" function.
          EXPECT_TRUE(address_infos_received->contains(callstack.pcs(pc_index + 1)));
        }

        ++matching_callstack_count;
        first_matching_callstack_timestamp_ns =
            std::min(first_matching_callstack_timestamp_ns, callstack_sample.timestamp_ns());
        last_matching_callstack_timestamp_ns =
            std::max(last_matching_callstack_timestamp_ns, callstack_sample.timestamp_ns());
        break;
      }
    }
  }

  ASSERT_GT(matching_callstack_count, 0);
  CHECK(first_matching_callstack_timestamp_ns <= last_matching_callstack_timestamp_ns);
  LOG("Found %lu of the expected callstacks over %.0f ms", matching_callstack_count,
      (last_matching_callstack_timestamp_ns - first_matching_callstack_timestamp_ns) / 1e6);
  constexpr double kMinExpectedScheduledRelativeTime = 0.67;
  const auto min_expected_matching_callstack_count = static_cast<uint64_t>(
      floor((last_matching_callstack_timestamp_ns - first_matching_callstack_timestamp_ns) / 1e9 *
            sampling_rate * kMinExpectedScheduledRelativeTime));
  EXPECT_GE(matching_callstack_count, min_expected_matching_callstack_count);
}

void VerifyCallstackSamplesWithOuterAndInnerFunctionForDwarfUnwinding(
    const std::vector<orbit_grpc_protos::ProducerCaptureEvent>& events, pid_t pid,
    std::pair<uint64_t, uint64_t> outer_function_virtual_address_range,
    std::pair<uint64_t, uint64_t> inner_function_virtual_address_range, double sampling_rate,
    const absl::flat_hash_set<uint64_t>* address_infos_received) {
  VerifyCallstackSamplesWithOuterAndInnerFunction(
      events, pid, outer_function_virtual_address_range, inner_function_virtual_address_range,
      sampling_rate, address_infos_received, /*unwound_with_frame_pointers=*/false);
}

void VerifyCallstackSamplesWithOuterAndInnerFunctionForFramePointerUnwinding(
    const std::vector<orbit_grpc_protos::ProducerCaptureEvent>& events, pid_t pid,
    std::pair<uint64_t, uint64_t> outer_function_virtual_address_range,
    std::pair<uint64_t, uint64_t> inner_function_virtual_address_range, double sampling_rate,
    const absl::flat_hash_set<uint64_t>* address_infos_received) {
  VerifyCallstackSamplesWithOuterAndInnerFunction(
      events, pid, outer_function_virtual_address_range, inner_function_virtual_address_range,
      sampling_rate, address_infos_received, /*unwound_with_frame_pointers=*/true);
}

TEST(LinuxTracingIntegrationTest, CallstackSamplesAndAddressInfos) {
  if (!CheckIsPerfEventParanoidAtMost(0)) {
    GTEST_SKIP();
  }
  LinuxTracingIntegrationTestFixture fixture;

  const auto& [outer_function_virtual_address_range, inner_function_virtual_address_range] =
      GetOuterAndInnerFunctionVirtualAddressRanges(fixture.GetPuppetPid());
  const std::filesystem::path& executable_path = GetExecutableBinaryPath(fixture.GetPuppetPid());

  orbit_grpc_protos::CaptureOptions capture_options = fixture.BuildDefaultCaptureOptions();
  const double samples_per_second = capture_options.samples_per_second();

  std::vector<orbit_grpc_protos::ProducerCaptureEvent> events =
      TraceAndGetEvents(&fixture, PuppetConstants::kCallOuterFunctionCommand, capture_options);

  VerifyOrderOfAllEvents(events);

  VerifyErrorsWithPerfEventOpenEvent(events);

  absl::flat_hash_set<uint64_t> address_infos_received =
      VerifyAndGetAddressInfosWithOuterAndInnerFunction(events, executable_path,
                                                        outer_function_virtual_address_range,
                                                        inner_function_virtual_address_range);

  VerifyCallstackSamplesWithOuterAndInnerFunctionForDwarfUnwinding(
      events, fixture.GetPuppetPid(), outer_function_virtual_address_range,
      inner_function_virtual_address_range, samples_per_second, &address_infos_received);
}

TEST(LinuxTracingIntegrationTest, CallstackSamplesTogetherWithFunctionCalls) {
  if (!CheckIsRunningAsRoot()) {
    GTEST_SKIP();
  }
  LinuxTracingIntegrationTestFixture fixture;

  const auto& [outer_function_virtual_address_range, inner_function_virtual_address_range] =
      GetOuterAndInnerFunctionVirtualAddressRanges(fixture.GetPuppetPid());
  const std::filesystem::path& executable_path = GetExecutableBinaryPath(fixture.GetPuppetPid());

  orbit_grpc_protos::CaptureOptions capture_options = fixture.BuildDefaultCaptureOptions();
  constexpr uint64_t kOuterFunctionId = 1;
  constexpr uint64_t kInnerFunctionId = 2;
  AddOuterAndInnerFunctionToCaptureOptions(&capture_options, fixture.GetPuppetPid(),
                                           kOuterFunctionId, kInnerFunctionId);
  const double sampling_rate = capture_options.samples_per_second();

  std::vector<orbit_grpc_protos::ProducerCaptureEvent> events =
      TraceAndGetEvents(&fixture, PuppetConstants::kCallOuterFunctionCommand, capture_options);

  VerifyOrderOfAllEvents(events);

  VerifyFunctionCallsOfOuterAndInnerFunction(events, fixture.GetPuppetPid(), kOuterFunctionId,
                                             kInnerFunctionId);

  absl::flat_hash_set<uint64_t> address_infos_received =
      VerifyAndGetAddressInfosWithOuterAndInnerFunction(events, executable_path,
                                                        outer_function_virtual_address_range,
                                                        inner_function_virtual_address_range);

  VerifyCallstackSamplesWithOuterAndInnerFunctionForDwarfUnwinding(
      events, fixture.GetPuppetPid(), outer_function_virtual_address_range,
      inner_function_virtual_address_range, sampling_rate, &address_infos_received);
}

void VerifyNoAddressInfos(const std::vector<orbit_grpc_protos::ProducerCaptureEvent>& events) {
  for (const auto& event : events) {
    EXPECT_NE(event.event_case(), orbit_grpc_protos::ProducerCaptureEvent::kFullAddressInfo);
  }
}

TEST(LinuxTracingIntegrationTest, CallstackSamplesWithFramePointers) {
  if (!CheckIsPerfEventParanoidAtMost(0)) {
    GTEST_SKIP();
  }
  LinuxTracingIntegrationTestFixture fixture;

  const auto& [outer_function_virtual_address_range, inner_function_virtual_address_range] =
      GetOuterAndInnerFunctionVirtualAddressRanges(fixture.GetPuppetPid());

  orbit_grpc_protos::CaptureOptions capture_options = fixture.BuildDefaultCaptureOptions();
  capture_options.set_unwinding_method(orbit_grpc_protos::CaptureOptions::kFramePointers);
  capture_options.set_stack_dump_size(512);
  const double sampling_rate = capture_options.samples_per_second();

  std::vector<orbit_grpc_protos::ProducerCaptureEvent> events =
      TraceAndGetEvents(&fixture, PuppetConstants::kCallOuterFunctionCommand, capture_options);

  VerifyOrderOfAllEvents(events);

  VerifyErrorsWithPerfEventOpenEvent(events);

  // AddressInfos are not sent when unwinding with frame pointers as they are produced by
  // libunwindstack.
  VerifyNoAddressInfos(events);

  // Note that this test requires that the "inner" function of the puppet use frame pointers.
  VerifyCallstackSamplesWithOuterAndInnerFunctionForFramePointerUnwinding(
      events, fixture.GetPuppetPid(), outer_function_virtual_address_range,
      inner_function_virtual_address_range, sampling_rate, /*address_infos_received=*/nullptr);
}

TEST(LinuxTracingIntegrationTest, CallstackSamplesWithFramePointersTogetherWithFunctionCalls) {
  if (!CheckIsRunningAsRoot()) {
    GTEST_SKIP();
  }
  LinuxTracingIntegrationTestFixture fixture;

  const auto& [outer_function_virtual_address_range, inner_function_virtual_address_range] =
      GetOuterAndInnerFunctionVirtualAddressRanges(fixture.GetPuppetPid());

  orbit_grpc_protos::CaptureOptions capture_options = fixture.BuildDefaultCaptureOptions();
  capture_options.set_unwinding_method(orbit_grpc_protos::CaptureOptions::kFramePointers);
  constexpr uint64_t kOuterFunctionId = 1;
  constexpr uint64_t kInnerFunctionId = 2;
  AddOuterAndInnerFunctionToCaptureOptions(&capture_options, fixture.GetPuppetPid(),
                                           kOuterFunctionId, kInnerFunctionId);
  const double sampling_rate = capture_options.samples_per_second();

  std::vector<orbit_grpc_protos::ProducerCaptureEvent> events =
      TraceAndGetEvents(&fixture, PuppetConstants::kCallOuterFunctionCommand, capture_options);

  VerifyOrderOfAllEvents(events);

  VerifyFunctionCallsOfOuterAndInnerFunction(events, fixture.GetPuppetPid(), kOuterFunctionId,
                                             kInnerFunctionId);

  VerifyNoAddressInfos(events);

  VerifyCallstackSamplesWithOuterAndInnerFunctionForFramePointerUnwinding(
      events, fixture.GetPuppetPid(), outer_function_virtual_address_range,
      inner_function_virtual_address_range, sampling_rate, /*address_infos_received=*/nullptr);
}

TEST(LinuxTracingIntegrationTest, ThreadStateSlices) {
  if (!CheckIsRunningAsRoot()) {
    GTEST_SKIP();
  }
  LinuxTracingIntegrationTestFixture fixture;

  std::vector<orbit_grpc_protos::ProducerCaptureEvent> events =
      TraceAndGetEvents(&fixture, PuppetConstants::kSleepCommand);

  VerifyOrderOfAllEvents(events);

  uint64_t running_slice_count = 0;
  uint64_t runnable_slice_count = 0;
  uint64_t interruptible_sleep_slice_count = 0;
  uint64_t last_end_timestamp_ns = 0;
  for (const auto& event : events) {
    if (event.event_case() != orbit_grpc_protos::ProducerCaptureEvent::kThreadStateSlice) {
      continue;
    }

    const orbit_grpc_protos::ThreadStateSlice& thread_state_slice = event.thread_state_slice();
    if (thread_state_slice.tid() != fixture.GetPuppetPid()) {
      continue;
    }

    // We currently don't set the pid.
    EXPECT_EQ(thread_state_slice.pid(), 0);

    EXPECT_TRUE(
        thread_state_slice.thread_state() == orbit_grpc_protos::ThreadStateSlice::kRunning ||
        thread_state_slice.thread_state() == orbit_grpc_protos::ThreadStateSlice::kRunnable ||
        thread_state_slice.thread_state() ==
            orbit_grpc_protos::ThreadStateSlice::kInterruptibleSleep);
    switch (thread_state_slice.thread_state()) {
      case orbit_grpc_protos::ThreadStateSlice_ThreadState_kRunning:
        ++running_slice_count;
        break;
      case orbit_grpc_protos::ThreadStateSlice_ThreadState_kRunnable:
        ++runnable_slice_count;
        break;
      case orbit_grpc_protos::ThreadStateSlice_ThreadState_kInterruptibleSleep:
        ++interruptible_sleep_slice_count;
        break;
      default:
        break;
    }

    EXPECT_GT(thread_state_slice.duration_ns(), 0);
    EXPECT_GT(thread_state_slice.end_timestamp_ns(), last_end_timestamp_ns);
    last_end_timestamp_ns = thread_state_slice.end_timestamp_ns();
  }

  LOG("running_slice_count=%lu", running_slice_count);
  LOG("runnable_slice_count=%lu", runnable_slice_count);
  LOG("interruptible_sleep_slice_count=%lu", interruptible_sleep_slice_count);
  // "- 1" as these are the expected numbers of kRunning and kRunnable ThreadStateSlices *only
  // between* the first and last sleep.
  EXPECT_GE(running_slice_count, PuppetConstants::kSleepCount - 1);
  EXPECT_GE(runnable_slice_count, PuppetConstants::kSleepCount - 1);
  EXPECT_GE(interruptible_sleep_slice_count, PuppetConstants::kSleepCount);
}

TEST(LinuxTracingIntegrationTest, ThreadNames) {
  if (!CheckIsRunningAsRoot()) {
    GTEST_SKIP();
  }
  LinuxTracingIntegrationTestFixture fixture;

  // We also collect the initial name of each thread of the target at the start of the capture:
  // save the actual initial name so that we can later verify that it was received.
  std::string initial_puppet_name = orbit_base::GetThreadName(fixture.GetPuppetPid());

  std::vector<orbit_grpc_protos::ProducerCaptureEvent> events =
      TraceAndGetEvents(&fixture, PuppetConstants::kPthreadSetnameNpCommand);

  VerifyOrderOfAllEvents(events);

  std::vector<std::string> changed_thread_names;
  std::vector<std::string> initial_thread_names;
  for (const auto& event : events) {
    if (event.event_case() == orbit_grpc_protos::ProducerCaptureEvent::kThreadNamesSnapshot) {
      const orbit_grpc_protos::ThreadNamesSnapshot& thread_names_snapshot =
          event.thread_names_snapshot();
      for (const auto& thread_name : thread_names_snapshot.thread_names()) {
        if (thread_name.pid() != fixture.GetPuppetPid()) {
          continue;
        }

        // There is only one thread and it is the main thread.
        EXPECT_EQ(thread_name.tid(), fixture.GetPuppetPid());

        initial_thread_names.emplace_back(thread_name.name());
      }
    }
    if (event.event_case() != orbit_grpc_protos::ProducerCaptureEvent::kThreadName) {
      continue;
    }

    const orbit_grpc_protos::ThreadName& thread_name = event.thread_name();
    if (thread_name.pid() != fixture.GetPuppetPid()) {
      continue;
    }

    // There is only one thread and it is the main thread.
    EXPECT_EQ(thread_name.tid(), fixture.GetPuppetPid());

    changed_thread_names.emplace_back(thread_name.name());
  }

  EXPECT_THAT(initial_thread_names, ::testing::ElementsAre(initial_puppet_name));

  EXPECT_THAT(changed_thread_names, ::testing::ElementsAre(PuppetConstants::kNewThreadName));
}

TEST(LinuxTracingIntegrationTest, ModuleUpdateOnDlopen) {
  if (!CheckIsPerfEventParanoidAtMost(0)) {
    GTEST_SKIP();
  }
  LinuxTracingIntegrationTestFixture fixture;

  std::vector<orbit_grpc_protos::ProducerCaptureEvent> events =
      TraceAndGetEvents(&fixture, PuppetConstants::kDlopenCommand);

  VerifyOrderOfAllEvents(events);

  VerifyErrorsWithPerfEventOpenEvent(events);

  bool module_update_found = false;
  for (const auto& event : events) {
    if (event.event_case() != orbit_grpc_protos::ProducerCaptureEvent::kModuleUpdateEvent) {
      continue;
    }

    const orbit_grpc_protos::ModuleUpdateEvent& module_update = event.module_update_event();
    if (module_update.pid() != fixture.GetPuppetPid()) {
      continue;
    }

    const orbit_grpc_protos::ModuleInfo& module_info = module_update.module();
    if (module_info.name() != PuppetConstants::kSharedObjectFileName) {
      continue;
    }

    EXPECT_FALSE(module_update_found);
    module_update_found = true;
  }

  EXPECT_TRUE(module_update_found);
}

TEST(LinuxTracingIntegrationTest, GpuJobs) {
  if (!CheckIsStadiaInstance()) {
    GTEST_SKIP();
  }
  if (!CheckIsRunningAsRoot()) {
    GTEST_SKIP();
  }
  LinuxTracingIntegrationTestFixture fixture;

  std::vector<orbit_grpc_protos::ProducerCaptureEvent> events =
      TraceAndGetEvents(&fixture, PuppetConstants::kVulkanTutorialCommand);

  VerifyOrderOfAllEvents(events);

  bool another_process_used_gpu = false;
  for (const auto& event : events) {
    if (event.event_case() != orbit_grpc_protos::ProducerCaptureEvent::kFullGpuJob) {
      continue;
    }

    const orbit_grpc_protos::FullGpuJob& gpu_job = event.full_gpu_job();
    if (gpu_job.pid() != fixture.GetPuppetPid()) {
      another_process_used_gpu = true;
      break;
    }
  }
  LOG("another_process_used_gpu=%d", another_process_used_gpu);

  uint64_t gpu_job_count = 0;
  for (const auto& event : events) {
    if (event.event_case() != orbit_grpc_protos::ProducerCaptureEvent::kFullGpuJob) {
      continue;
    }

    const orbit_grpc_protos::FullGpuJob& gpu_job = event.full_gpu_job();
    if (gpu_job.pid() != fixture.GetPuppetPid()) {
      continue;
    }

    // The puppet is single-threaded.
    EXPECT_EQ(gpu_job.tid(), fixture.GetPuppetPid());

    if (!another_process_used_gpu) {
      EXPECT_EQ(gpu_job.depth(), 0);
    }

    EXPECT_LT(gpu_job.amdgpu_cs_ioctl_time_ns(), gpu_job.amdgpu_sched_run_job_time_ns());
    // If no other job is running on the GPU (which is the case if the puppet is the only process
    // using the GPU), then we assume (it's the best we can do) that the job starts running on the
    // hardware at the same time as it is scheduled by the driver, hence the EXPECT_EQ.
    // Otherwise, use EXPECT_LE.
    if (another_process_used_gpu) {
      EXPECT_LE(gpu_job.amdgpu_sched_run_job_time_ns(), gpu_job.gpu_hardware_start_time_ns());
    } else {
      EXPECT_EQ(gpu_job.amdgpu_sched_run_job_time_ns(), gpu_job.gpu_hardware_start_time_ns());
    }
    EXPECT_LT(gpu_job.gpu_hardware_start_time_ns(), gpu_job.dma_fence_signaled_time_ns());

    EXPECT_EQ(gpu_job.timeline(), "gfx");

    ++gpu_job_count;
  }

  LOG("gpu_job_count=%lu", gpu_job_count);
  EXPECT_GE(gpu_job_count, PuppetConstants::kFrameCount);
}

}  // namespace
}  // namespace orbit_linux_tracing_integration_tests
