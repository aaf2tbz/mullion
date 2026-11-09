#include "d3d12_upload_check.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include <atomic>
#include <cstdio>
#include <windows.h>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace mullion::upload_check {

bool
Enabled() {
  static const bool enabled = env::getEnvVar("MULLION_D3D12_UPLOAD_CHECK") == "1";
  return enabled;
}

static mullion::mutex uploads_lock;
static std::unordered_set<const BufferAllocation *> uploads;

void
AddUpload(const BufferAllocation *allocation) {
  std::lock_guard lock(uploads_lock);
  uploads.insert(allocation);
}

void
RemoveUpload(const BufferAllocation *allocation) {
  std::lock_guard lock(uploads_lock);
  uploads.erase(allocation);
}

bool
IsUpload(const BufferAllocation *allocation) {
  std::lock_guard lock(uploads_lock);
  return uploads.contains(allocation);
}

// FNV-1a over 8-byte words, then the bytes left
uint64_t
Sum(const uint8_t *data, uint64_t length) {
  uint64_t sum = 0xcbf29ce484222325ull;
  uint64_t i = 0;
  for (; i + 8 <= length; i += 8) {
    uint64_t word;
    std::memcpy(&word, data + i, 8);
    sum = (sum ^ word) * 0x100000001b3ull;
  }
  for (; i < length; i++)
    sum = (sum ^ data[i]) * 0x100000001b3ull;
  return sum;
}

void
Snapshot(Ranges &ranges) {
  auto now = GetTickCount64();
  for (auto &range : ranges) {
    range.sum = Sum(range.data, range.length);
    range.kept = std::make_shared<std::vector<uint8_t>>(range.data, range.data + std::min<uint64_t>(range.length, 4096));
    range.handed = now;
  }
}

static std::atomic<uint64_t> checked{0}, changed{0}, submissions{0};
static std::atomic<uint32_t> reported{0};

void
Verify(const Submission &submission) {
  auto &ranges = submission.ranges;
  // where the fence the application waits on stands: at or past its value, it said the work was done before it was
  std::string fence = "no fence signaled after it yet";
  if (submission.fence) {
    auto completed = submission.fence->GetCompletedValue();
    fence = "its fence " + std::to_string(submission.value) + " reads " + std::to_string(completed) +
            (completed >= submission.value && completed != UINT64_MAX ? " (ALREADY COMPLETE)" : " (pending)");
  }
  auto now = GetTickCount64();
  std::string timing = "committed " + std::to_string(submission.commit_began - submission.executed) + "-" +
                       std::to_string(submission.commit_ended - submission.executed) + " ms after execution, done at " +
                       std::to_string(now - submission.executed) + " ms";
  uint64_t changed_here = 0;
  for (auto &range : ranges) {
    if (Sum(range.data, range.length) == range.sum)
      continue;
    changed_here++;
    // the first few hundred, then the summaries alone
    if (reported.fetch_add(1) < 300) {
      // the first dword that differs among those kept, as it was and as it is
      std::string first = "beyond the first 4096 bytes";
      auto &kept = *range.kept;
      for (uint64_t at = 0; at + 4 <= kept.size(); at += 4) {
        uint32_t was, is;
        std::memcpy(&was, kept.data() + at, 4);
        std::memcpy(&is, range.data + at, 4);
        if (was != is) {
          char text[96];
          snprintf(text, sizeof(text), "first at +%llu: 0x%08x -> 0x%08x", (unsigned long long)at, was, is);
          first = text;
          break;
        }
      }
      WARN(
          "upload check: ", range.length, " bytes at ", (void *)range.va, " changed while the GPU ran a draw of ",
          range.count, " vertices x ", range.instances, " instances that reads them (root parameter ", range.parameter,
          range.descriptor == ~0u ? std::string(", root descriptor") : ", table descriptor " + std::to_string(range.descriptor),
          "), ", first, ", ", GetTickCount64() - range.handed, " ms after ExecuteCommandLists, ", fence, ", ", timing
      );
    }
  }
  auto total_checked = checked.fetch_add(ranges.size()) + ranges.size();
  auto total_changed = changed.fetch_add(changed_here) + changed_here;
  auto count = submissions.fetch_add(1) + 1;
  if (count % 500 == 0)
    WARN("upload check: ", count, " submissions, ", total_checked, " ranges checked, ", total_changed, " changed");
}

} // namespace mullion::upload_check
