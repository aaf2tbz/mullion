#pragma once

#include "Metal.hpp"
#include "log/log.hpp"
#include "thread.hpp"
#include "util_env.hpp"
#include "util_string.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace mullion {

// the GPU binaries of the application's pipelines, kept by Mullion: Metal binary archives in the shader cache's folder
// (pipelines_<Metal version>/chunk_<n>.metallib), so that a pipeline made before is loaded rather than compiled again,
// whatever Metal's own per-process compiler cache holds (it is capped, and a large game outgrows it). A pipeline is
// looked up in the archives saved before, which fail it on a miss (MTLPipelineOptionFailOnBinaryArchiveMiss); a miss
// compiles it and adds it to the chunk this process fills, saved every so many pipelines or seconds, and once full
// left for the next chunk. MULLION_D3D12_PIPELINE_ARCHIVE=0 turns it off, as does MULLION_SHADER_CACHE=0
class PipelineArchive {
public:
  // the process's one, made by its first pipeline and never destroyed: a game ends with compile threads still adding
  static PipelineArchive &
  Get(WMT::Device device, uint32_t metal_version) {
    static PipelineArchive *archive = Made() = new PipelineArchive(device, metal_version);
    return *archive;
  }

  // its pipelines not saved yet are saved, if it was made: a device's release (an application that ends normally)
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

  struct Counts {
    uint64_t hits, misses, saved;
  };

  Counts
  Count() const {
    return {hits_.load(), misses_.load(), saved_.load()};
  }

  // the pipelines added so far are saved now (a test's end)
  void
  Save() {
    if (!enabled_)
      return;
    std::unique_lock lock(chunk_mutex_);
    SaveLocked();
  }

private:
  static PipelineArchive *&
  Made() {
    static PipelineArchive *made = nullptr;
    return made;
  }

  static constexpr uint32_t kChunkPipelines = 512;
  static constexpr auto kSaveInterval = std::chrono::seconds(20);
  // binary_archives_for_lookup counts them in a byte
  static constexpr uint32_t kMaxChunks = 200;

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
    for (chunk_ = 0; chunk_ < kMaxChunks; chunk_++) {
      WMT::Reference<WMT::Error> error;
      auto saved = device_.newBinaryArchive(ChunkPath(chunk_).c_str(), error);
      if (!saved)
        break;
      lookup_.push_back(saved);
      lookup_handles_.push_back(saved.handle);
    }
    if (chunk_ == kMaxChunks) {
      WARN("pipeline archive: ", folder_, " has ", kMaxChunks, " chunks, which is all it looks up; new pipelines are not kept");
      return;
    }
    WMT::Reference<WMT::Error> error;
    filling_ = device_.newBinaryArchive(nullptr, error);
    if (!filling_) {
      ERR("pipeline archive: no archive to fill: ", error ? error.description().getUTF8String() : "");
      return;
    }
    last_save_ = std::chrono::steady_clock::now();
    enabled_ = true;
    Logger::info(str::format("pipeline archive: ", folder_, ", ", lookup_.size(), " saved chunks"));
  }

  std::string
  ChunkPath(uint32_t chunk) const {
    return str::format(folder_, "chunk_", chunk, ".metallib");
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
    decltype(create(info, error)) pipeline;
    bool full;
    {
      std::shared_lock lock(chunk_mutex_);
      info.binary_archive_for_serialization = filling_ ? filling_.handle : NULL_OBJECT_HANDLE;
      pipeline = create(info, error);
      full = pipeline && filling_ && ++filled_ >= kChunkPipelines;
    }
    if (pipeline && filling_ && (full || std::chrono::steady_clock::now() - last_save_.load() >= kSaveInterval)) {
      std::unique_lock lock(chunk_mutex_, std::try_to_lock);
      if (lock.owns_lock())
        SaveLocked();
    }
    return pipeline;
  }

  // the chunk being filled is written (whole, as a new file put in place of the last), and once full the next begun
  void
  SaveLocked() {
    last_save_ = std::chrono::steady_clock::now();
    if (!filling_ || filled_ == saved_in_chunk_)
      return;
    WMT::Reference<WMT::Error> error;
    filling_.serialize(ChunkPath(chunk_).c_str(), error);
    if (error) {
      ERR("pipeline archive: ", ChunkPath(chunk_), " was not saved: ", error.description().getUTF8String());
      filling_ = {};
      return;
    }
    saved_ += filled_ - saved_in_chunk_;
    saved_in_chunk_ = filled_;
    Logger::info(str::format(
        "pipeline archive: chunk ", chunk_, " saved with ", filled_.load(), " pipelines (", hits_.load(), " loaded, ",
        misses_.load(), " compiled this run)"
    ));
    if (filled_ < kChunkPipelines)
      return;
    // this process looks the full chunk's pipelines up no more: it has them
    filling_ = {};
    filled_ = 0;
    saved_in_chunk_ = 0;
    if (++chunk_ >= kMaxChunks)
      return;
    filling_ = device_.newBinaryArchive(nullptr, error);
  }

  WMT::Device device_;
  bool enabled_ = false;
  std::string folder_;
  // the chunks saved before this process, which it only looks up
  std::vector<WMT::Reference<WMT::BinaryArchive>> lookup_;
  std::vector<obj_handle_t> lookup_handles_;
  // adding to the chunk takes it shared, and saving or replacing it exclusive
  mullion::shared_mutex chunk_mutex_;
  WMT::Reference<WMT::BinaryArchive> filling_;
  uint32_t chunk_ = 0;
  std::atomic<uint32_t> filled_ = 0;
  uint32_t saved_in_chunk_ = 0;
  std::atomic<std::chrono::steady_clock::time_point> last_save_;
  std::atomic<uint64_t> hits_ = 0, misses_ = 0, saved_ = 0;
};

} // namespace mullion
