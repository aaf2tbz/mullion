#pragma once

#include "Metal.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <vector>

namespace mullion {

// the GPU binaries of the application's pipelines, kept by Mullion: Metal binary archives in the shader cache's folder
// (pipelines_<Metal version>/chunk_<n>.metallib), so that a pipeline made before is loaded rather than compiled again,
// whatever Metal's own per-process compiler cache holds (it is capped, and a large game outgrows it). A pipeline is
// looked up in the chunks saved before, which fail it on a miss (MTLPipelineOptionFailOnBinaryArchiveMiss); a miss is
// compiled into the chunk this process fills and made from it (winemetal compiles it once for both). A chunk is closed
// once it holds kChunkPipelines, or after a while with fewer, and written when the last compile into it ends: no lock
// is held while Metal compiles, so a save waits for nothing. MULLION_D3D12_PIPELINE_ARCHIVE=0 turns it off, as does
// MULLION_SHADER_CACHE=0
class PipelineArchive {
public:
  // the process's one, made by its first pipeline and never destroyed: a game ends with compile threads still adding
  static PipelineArchive &
  Get(WMT::Device device, uint32_t metal_version) {
    static PipelineArchive *archive = Made() = new PipelineArchive(device, metal_version);
    return *archive;
  }

  // the chunk being filled is closed and written, if there is one: a device's release (an application that ends
  // normally)
  static void
  SaveMade() {
    if (auto archive = Made())
      archive->Save();
  }

  template <typename Info>
  auto
  Make(WMT::Device device, const Info &in, WMT::Reference<WMT::Error> &error) {
    if (!enabled_)
      return device.newRenderPipelineState(in, error);
    return MakeWith(in, error, [&](const Info &info, WMT::Reference<WMT::Error> &err) {
      return device.newRenderPipelineState(info, err);
    });
  }

  WMT::Reference<WMT::ComputePipelineState>
  MakeCompute(WMT::Device device, const WMTComputePipelineInfo &in, WMT::Reference<WMT::Error> &error) {
    if (!enabled_)
      return device.newComputePipelineState(in, error);
    return MakeWith(in, error, [&](const WMTComputePipelineInfo &info, WMT::Reference<WMT::Error> &err) {
      return device.newComputePipelineState(info, err);
    });
  }

  void
  Save() {
    if (enabled_)
      Close(nullptr);
  }

private:
  static PipelineArchive *&
  Made() {
    static PipelineArchive *made = nullptr;
    return made;
  }

  // MULLION_D3D12_PIPELINE_ARCHIVE_CHUNK sets it, for a test of chunks closed while compiles go on
  static inline const uint32_t kChunkPipelines = [] {
    auto given = env::getEnvVar("MULLION_D3D12_PIPELINE_ARCHIVE_CHUNK");
    return given.empty() ? 512u : std::max(1u, uint32_t(std::stoul(given)));
  }();
  // a chunk with fewer is closed after these, so that a game killed later keeps them
  static constexpr uint32_t kChunkPipelinesSoon = 64;
  static constexpr auto kSoon = std::chrono::seconds(60), kLate = std::chrono::seconds(300);
  // binary_archives_for_lookup counts them in a byte
  static constexpr uint32_t kMaxChunks = 200;

  // a chunk being filled, written when the last compile into it ends once it is closed
  struct Chunk {
    PipelineArchive *owner;
    uint32_t index;
    WMT::Reference<WMT::BinaryArchive> archive;
    std::chrono::steady_clock::time_point begun = std::chrono::steady_clock::now();
    std::atomic<uint32_t> added = 0;

    Chunk(PipelineArchive *owner, uint32_t index, WMT::Reference<WMT::BinaryArchive> archive)
        : owner(owner), index(index), archive(std::move(archive)) {}

    ~Chunk() {
      if (added)
        owner->Write(*this);
    }
  };

  PipelineArchive(WMT::Device device, uint32_t metal_version) : device_(device) {
    if (env::getEnvVar("MULLION_SHADER_CACHE") == "0" || env::getEnvVar("MULLION_D3D12_PIPELINE_ARCHIVE") == "0")
      return;
    std::string path = env::getEnvVar("MULLION_SHADER_CACHE_PATH");
    if (!path.empty() && path.starts_with("/")) {
      if (!path.ends_with('/'))
        path += "/";
    } else {
      // relative to the user's cache folder, as the shader cache's
      path = str::format("mullion/", env::getExeName(), "/");
    }
    folder_ = str::format(path, "pipelines_", metal_version, "/");
    for (next_ = 0; next_ < kMaxChunks; next_++) {
      WMT::Reference<WMT::Error> error;
      auto saved = device_.newBinaryArchive(ChunkPath(next_).c_str(), error);
      if (!saved)
        break;
      lookup_.push_back(saved);
      lookup_handles_.push_back(saved.handle);
    }
    if (next_ == kMaxChunks)
      WARN("pipeline archive: ", folder_, " has ", kMaxChunks, " chunks, which is all it looks up; new pipelines are not kept");
    enabled_ = true;
    Logger::info(str::format("pipeline archive: ", folder_, ", ", lookup_.size(), " saved chunks"));
  }

  std::string
  ChunkPath(uint32_t chunk) const {
    return str::format(folder_, "chunk_", chunk, ".metallib");
  }

  // the chunk to compile into, begun if there is none; null once there is no room for another
  std::shared_ptr<Chunk>
  Filling() {
    std::lock_guard lock(mutex_);
    if (!filling_ && next_ < kMaxChunks) {
      WMT::Reference<WMT::Error> error;
      auto archive = device_.newBinaryArchive(nullptr, error);
      if (!archive) {
        ERR("pipeline archive: no archive to fill: ", error ? error.description().getUTF8String() : "");
        next_ = kMaxChunks;
        return nullptr;
      }
      filling_ = std::make_shared<Chunk>(this, next_++, std::move(archive));
    }
    return filling_;
  }

  // `chunk` (or whichever is being filled, when null) takes no more pipelines; it is written once the compiles into
  // it end, here when none is under way
  void
  Close(const std::shared_ptr<Chunk> &chunk) {
    std::shared_ptr<Chunk> closed;
    std::lock_guard lock(mutex_);
    if (filling_ && (!chunk || filling_ == chunk))
      closed = std::move(filling_);
    // `closed` goes after the lock, as declared before it
  }

  template <typename Info, typename Create>
  decltype(std::declval<Create>()(std::declval<const Info &>(), std::declval<WMT::Reference<WMT::Error> &>()))
  MakeWith(const Info &in, WMT::Reference<WMT::Error> &error, Create create) {
    Info info = in;
    if (!lookup_handles_.empty()) {
      info.binary_archives_for_lookup.set(lookup_handles_.data());
      info.num_binary_archives_for_lookup = lookup_handles_.size();
      info.fail_on_binary_archive_miss = true;
      WMT::Reference<WMT::Error> miss;
      if (auto pipeline = create(info, miss)) {
        hits_++;
        return pipeline;
      }
      info.binary_archives_for_lookup.set(nullptr);
      info.num_binary_archives_for_lookup = 0;
      info.fail_on_binary_archive_miss = false;
    }
    misses_++;
    auto chunk = Filling();
    info.binary_archive_for_serialization = chunk ? chunk->archive.handle : NULL_OBJECT_HANDLE;
    auto pipeline = create(info, error);
    if (pipeline && chunk) {
      auto added = ++chunk->added;
      auto age = std::chrono::steady_clock::now() - chunk->begun;
      if (added >= kChunkPipelines || (added >= kChunkPipelinesSoon && age >= kSoon) || age >= kLate)
        Close(chunk);
    }
    return pipeline;
  }

  // a closed chunk written, as a new file put in place of any partial one
  void
  Write(Chunk &chunk) {
    WMT::Reference<WMT::Error> error;
    chunk.archive.serialize(ChunkPath(chunk.index).c_str(), error);
    if (error) {
      ERR("pipeline archive: ", ChunkPath(chunk.index), " was not saved: ", error.description().getUTF8String());
      return;
    }
    Logger::info(str::format(
        "pipeline archive: chunk ", chunk.index, " saved with ", chunk.added.load(), " pipelines (", hits_.load(),
        " loaded, ", misses_.load(), " compiled this run)"
    ));
  }

  WMT::Device device_;
  bool enabled_ = false;
  std::string folder_;
  // the chunks saved before this process, which it only looks up
  std::vector<WMT::Reference<WMT::BinaryArchive>> lookup_;
  std::vector<obj_handle_t> lookup_handles_;
  mullion::mutex mutex_;
  std::shared_ptr<Chunk> filling_;
  uint32_t next_ = 0;
  std::atomic<uint64_t> hits_ = 0, misses_ = 0;
};

} // namespace mullion
