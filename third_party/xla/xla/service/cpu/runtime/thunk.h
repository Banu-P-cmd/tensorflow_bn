/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_SERVICE_CPU_RUNTIME_THUNK_H_
#define XLA_SERVICE_CPU_RUNTIME_THUNK_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "absl/container/inlined_vector.h"
#include "absl/functional/any_invocable.h"
#include "absl/status/statusor.h"
#include "xla/executable_run_options.h"
#include "xla/ffi/execution_context.h"
#include "xla/runtime/buffer_use.h"
#include "xla/service/cpu/collectives_interface.h"
#include "xla/service/cpu/runtime/buffer_allocations.h"
#include "xla/service/cpu/runtime/resource_use.h"
#include "xla/service/cpu/xfeed_manager.h"
#include "xla/service/global_device_id.h"
#include "xla/stream_executor/host/host_kernel_c_api.h"
#include "xla/stream_executor/stream.h"
#include "xla/tsl/concurrency/async_value_ref.h"
#include "xla/tsl/concurrency/chain.h"
#include "tsl/platform/statusor.h"

namespace Eigen {
struct ThreadPoolDevice;
}  // namespace Eigen

namespace xla::cpu {

// WARNING: This is under construction. Long term plan for XLA is to unify
// runtimes between different backends and have a shared Thunk interface,
// however for now we chose to have separate Thunk implementations in xla::cpu
// and xla::gpu namespaces with a plan to unify them in the future.

// Thunk is the basic unit of execution for the XLA CPU runtime.
//
// This is thread-compatible. Thunk implementation should expect that it will be
// called concurrently from multiple threads, for different run ids and for
// different devices. For partitioned XLA programs the expectation is that all
// local participants execute simultaneously on different threads and coordinate
// resource acquisition via rendezvous.
//
// This is XLA CPU's counterpart of the XLA GPU runtime Thunk.
class Thunk {
 public:
  enum class Kind {
    kAllGather,
    kAllReduce,
    kAllToAll,
    kCall,
    kCollectivePermute,
    kCopy,
    kConditional,
    kConvolution,
    kCustomCall,
    kDot,
    kFft,
    kInfeed,
    kKernel,
    kOutfeed,
    kPartitionId,
    kReduceScatter,
    kReplicaId,
    kRngGetAndUpdateState,
    kWhile,
  };

  struct Info {
    std::string op_name;
    std::string module_name;
    int64_t module_id;
  };

  // An abstract task runner that can be used by a ThunkExecutor (including
  // thunk executors for nested computations in conditional or while thunks) for
  // running tasks corresponding to thunk execution. It can be a simple inline
  // executor that runs tasks on the same thread, or a runner backed by a thread
  // pool. By default XLA:CPU uses task runner that shares underlying thread
  // pool with the intra-op thread pool used for compute tasks. We deliberately
  // do not prescribe task runner to be Eigen or any other particular thread
  // pool, and let users make the choice.
  using Task = std::function<void()>;
  using TaskRunner = absl::AnyInvocable<void(Task)>;

  Thunk(Kind kind, Info info) : kind_(kind), info_(std::move(info)) {}

  Thunk(const Thunk&) = delete;
  Thunk& operator=(const Thunk&) = delete;

  virtual ~Thunk() = default;

  Kind kind() const { return kind_; }
  const Info& info() const { return info_; }

  static std::string_view KindToString(Kind kind);

  // Returns the list of buffers used by a thunk. Thunk executor relies on this
  // information to execute thunks concurrently and to avoid data races.
  using BufferUses = absl::InlinedVector<BufferUse, 4>;
  virtual BufferUses buffer_uses() const = 0;

  // Returns the list of resources used by a thunk. Thunk executor relies on
  // this information to execute thunks concurrently and to avoid data races. In
  // contrast to buffer uses, only a handful of thunks are expected to use
  // resources, so we define a default implementation for `resource_uses()`
  // that returns an empty vector.
  using ResourceUses = absl::InlinedVector<ResourceUse, 4>;
  virtual ResourceUses resource_uses() const { return {}; }

  //===--------------------------------------------------------------------===//
  // HostKernels
  //===--------------------------------------------------------------------===//

  // Interface for finding host kernels (function pointers with host kernel API)
  // by name. At run time this is typically backed by an LLVM jit compiler that
  // compiles LLVM IR to executables on demand.
  class HostKernels {
   public:
    virtual ~HostKernels() = default;

    virtual absl::StatusOr<SE_HOST_Kernel*> Find(std::string_view name) = 0;
  };

  //===--------------------------------------------------------------------===//
  // CollectiveExecuteParams
  //===--------------------------------------------------------------------===//

  // Parameters capturing all the details required for collective execution of
  // XLA executables (multiple partitions and replicas).
  struct CollectiveExecuteParams {
    static absl::StatusOr<CollectiveExecuteParams> Create(
        const ExecutableRunOptions* run_options);

    RunId run_id;

    int64_t local_device_ordinal;
    GlobalDeviceId global_device_id;

    const DeviceAssignment* device_assignment = nullptr;
    CollectivesInterface* collectives = nullptr;

   private:
    CollectiveExecuteParams(RunId run_id, int64_t local_device_ordinal,
                            GlobalDeviceId global_device_id,
                            const DeviceAssignment* device_assignment,
                            CollectivesInterface* collectives);
  };

  //===--------------------------------------------------------------------===//
  // CustomCallExecuteParams
  //===--------------------------------------------------------------------===//

  // Parameters capturing all the details required for custom call execution of
  // XLA executables.
  struct CustomCallExecuteParams {
    static absl::StatusOr<CustomCallExecuteParams> Create(
        const ExecutableRunOptions* run_options);

    int32_t device_ordinal;
    stream_executor::Stream* stream = nullptr;
    stream_executor::DeviceMemoryAllocator* allocator = nullptr;
    const ffi::ExecutionContext* ffi_execution_context = nullptr;

   private:
    CustomCallExecuteParams(int32_t device_ordinal,
                            stream_executor::Stream* stream,
                            stream_executor::DeviceMemoryAllocator* allocator,
                            const ffi::ExecutionContext* ffi_execution_context);
  };

  //===--------------------------------------------------------------------===//
  // ExecuteParams
  //===--------------------------------------------------------------------===//

  // Parameters passed to Execute. Execute is responsible for launching "work"
  // on device, i.e., it launches host kernels, calls into libraries, etc.
  struct ExecuteParams {
    HostKernels* host_kernels = nullptr;
    const BufferAllocations* buffer_allocations = nullptr;
    runtime::XfeedManager* xfeed = nullptr;
    const Eigen::ThreadPoolDevice* intra_op_threadpool = nullptr;
    TaskRunner* task_runner = nullptr;
    CollectiveExecuteParams* collective_params = nullptr;
    CustomCallExecuteParams* custom_call_params = nullptr;
  };

  // An execute event that becomes ready when all tasks are completed.
  using ExecuteEvent = tsl::Chain;

  // Returns non-reference-counted async value ref for thunks executed in the
  // caller thread to avoid reference counting overhead.
  static tsl::AsyncValueRef<ExecuteEvent> OkExecuteEvent();

  // Thunk execution must be asynchronous and never block the caller thread,
  // especially waiting for work submitted into the `intra_op_threadpool`,
  // because thunks themselves are executed on the same thread pool.
  //
  // Thunk execution completion must be reported via the `ExecuteEvent`.
  virtual tsl::AsyncValueRef<ExecuteEvent> Execute(
      const ExecuteParams& params) = 0;

 protected:
  // Encodes thunk info into the TraceMe compatible format.
  std::string TraceMeEncode() const;

 private:
  Kind kind_;
  Info info_;
};

std::ostream& operator<<(std::ostream& os, Thunk::Kind kind);

// A sequence of thunks to execute.
class ThunkSequence : public std::vector<std::unique_ptr<Thunk>> {
 public:
  ThunkSequence() = default;

  // Returns an empty thunk sequence.
  static ThunkSequence Empty() { return ThunkSequence(); }

  // Returns a thunk sequence that contains a single thunk of type `T`. Uses
  // factory constructor `T::Create()` to create the thunk.
  template <typename T, typename... Args>
  static absl::StatusOr<ThunkSequence> Of(Args&&... args) {
    static_assert(std::is_base_of_v<Thunk, T>,
                  "ThunkSequence::Of() requires `T` to be a `Thunk` subclass.");
    TF_ASSIGN_OR_RETURN(auto thunk, T::Create(std::forward<Args>(args)...));
    return ThunkSequence(std::move(thunk));
  }

  using BufferUses = Thunk::BufferUses;
  BufferUses buffer_uses() const;

  using ResourceUses = Thunk::ResourceUses;
  ResourceUses resource_uses() const;

  void Append(ThunkSequence other);

 private:
  explicit ThunkSequence(std::unique_ptr<Thunk> thunk);
};

}  // namespace xla::cpu

#endif  // XLA_SERVICE_CPU_RUNTIME_THUNK_H_
