#pragma once

#include "d3d12.h"
#include "mullion_buffer.hpp"
#include "rc/util_rc_ptr.hpp"
#include <cstdint>
#include <memory>
#include <vector>

namespace mullion::upload_check {

// MULLION_D3D12_UPLOAD_CHECK=1 (read once): what draws of few vertices an instance and more than one instance (a
// particle system's sprites) read from upload heap buffers is summed when their lists are executed and again when the
// GPU has run them; a range whose bytes changed in between is logged. D3D12 lets the application write that memory
// until ExecuteCommandLists, and not again until the work completes, so a change is the CPU rewriting what the GPU may
// still read
bool Enabled();

// the allocations of buffers in upload heaps, the only memory checked
void AddUpload(const BufferAllocation *allocation);
void RemoveUpload(const BufferAllocation *allocation);
bool IsUpload(const BufferAllocation *allocation);

struct Range {
  Rc<BufferAllocation> allocation;
  const uint8_t *data = nullptr;
  uint64_t length = 0;
  uint64_t va = 0;
  // the draw's shape and the root parameter, and the table's descriptor (~0u for a root descriptor), that reach it
  uint32_t count = 0, instances = 0, parameter = 0, descriptor = ~0u;
  uint64_t sum = 0;
  // the first bytes as they were handed over, to show what changed, and when (GetTickCount64)
  std::shared_ptr<std::vector<uint8_t>> kept;
  uint64_t handed = 0;
};

using Ranges = std::vector<Range>;

// a submission's ranges, and the first fence the application signals on the queue after it (what tells it the work
// is done): its value then, and the fence's completed value read as the ranges are verified
struct Submission {
  Ranges ranges;
  ID3D12Fence *fence = nullptr; // referenced (AddRef) while set
  uint64_t value = 0;
  // when the lists were executed, and when the queue's thread began and ended committing them (GetTickCount64)
  uint64_t executed = 0, commit_began = 0, commit_ended = 0;
  ~Submission() {
    if (fence)
      fence->Release();
  }
};

uint64_t Sum(const uint8_t *data, uint64_t length);

// sums `ranges` as their lists are executed
void Snapshot(Ranges &ranges);

// sums them again once the GPU has run them, logging those that changed
void Verify(const Submission &submission);

} // namespace mullion::upload_check
