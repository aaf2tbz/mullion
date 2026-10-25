#pragma once

#include "Metal.hpp"
#include "../airconv/airconv_public.h"
#include "mullion_buffer.hpp"
#include "mullion_command_constants.hpp"
#include "mullion_texture.hpp"
#include "rc/util_rc_ptr.hpp"
#include <array>
#include <cmath>
#include <span>
#include <unordered_map>

namespace mullion {

template <typename Emit>
void EncodePipelineStatisticsDraw(Emit emit, WMT::RenderPipelineState count, WMT::RenderPipelineState restore,
                                  WMT::Buffer arguments, uint64_t offset, WMT::Buffer result, uint64_t result_offset,
                                  WMT::RenderPipelineState scan = {}, uint32_t scanners = 0) {
  auto &resource = emit.template operator()<wmtcmd_render_useresource>();
  resource.type = WMTRenderCommandUseResource;
  resource.resource = result;
  resource.usage = WMTResourceUsageRead | WMTResourceUsageWrite;
  resource.stages = WMTRenderStageVertex | WMTRenderStageMesh | WMTRenderStageFragment;
  auto &pipeline = emit.template operator()<wmtcmd_render_setpso>();
  pipeline.type = WMTRenderCommandSetPSO;
  pipeline.pso = count;
  auto &input = emit.template operator()<wmtcmd_render_setbuffer>();
  input.type = WMTRenderCommandSetVertexBuffer;
  input.buffer = arguments;
  input.offset = offset;
  input.index = kCustomBufferArgumentIndex0;
  auto &draw = emit.template operator()<wmtcmd_render_draw>();
  draw.type = WMTRenderCommandDraw;
  draw.primitive_type = WMTPrimitiveTypePoint;
  draw.vertex_start = draw.base_instance = 0;
  draw.vertex_count = draw.instance_count = 1;
  auto &zeroed = emit.template operator()<wmtcmd_render_memory_barrier>();
  zeroed.type = WMTRenderCommandMemoryBarrier;
  zeroed.scope = WMTBarrierScopeBuffers;
  zeroed.stages_after = WMTRenderStageVertex;
  zeroed.stages_before = WMTRenderStageVertex | WMTRenderStageMesh | WMTRenderStageFragment;
  if (scan) {
    auto &strip_pipeline = emit.template operator()<wmtcmd_render_setpso>();
    strip_pipeline.type = WMTRenderCommandSetPSO;
    strip_pipeline.pso = scan;
    auto &strips = emit.template operator()<wmtcmd_render_draw>();
    strips.type = WMTRenderCommandDraw;
    strips.primitive_type = WMTPrimitiveTypePoint;
    strips.vertex_start = strips.base_instance = 0;
    strips.vertex_count = scanners;
    strips.instance_count = 1;
  }
  auto &pixel = emit.template operator()<wmtcmd_render_setbuffer>();
  pixel.type = WMTRenderCommandSetFragmentBuffer;
  pixel.buffer = result;
  pixel.offset = result_offset;
  pixel.index = SM50_BINDING_INDEX_PIPELINE_STATISTICS;
  auto &geometry = emit.template operator()<wmtcmd_render_setbuffer>();
  geometry.type = WMTRenderCommandSetMeshBuffer;
  geometry.buffer = result;
  geometry.offset = result_offset;
  geometry.index = SM50_BINDING_INDEX_PIPELINE_STATISTICS;
  auto &previous = emit.template operator()<wmtcmd_render_setpso>();
  previous.type = WMTRenderCommandSetPSO;
  previous.pso = restore;
}

// Metal clips a triangle that reaches 1.5 of a viewport past its center into pieces, and on their seams runs a pixel
// shader once for each piece. D3D allows that (D3D11.3 15.4), but applications draw
// past the viewport as if it never happened, a full-screen triangle reaches 3. so Metal's viewports are made larger
// about their centers, and the stage before the rasterizer multiplies x and y by the inverse
// (SM50_BINDING_INDEX_RASTER_SCALE): positions land on the same pixels, and the cut moves out by the factor. it is the
// largest power of two, which keeps the scale exact, that leaves the viewports in the bounds an application may give
// them itself. returns the inverse
inline float
EnlargeViewports(std::span<WMTViewport> viewports, double bound_min, double bound_max) {
  double reach = INFINITY;
  for (auto &v : viewports)
    for (auto [origin, size] : {std::pair{v.originX, v.width}, std::pair{v.originY, v.height}})
      if (size > 0)
        reach = std::min({reach, (origin - bound_min) / (size / 2) + 1, (bound_max - origin) / (size / 2) - 1});
  double factor = std::isfinite(reach) && reach >= 1 ? std::exp2(std::floor(std::log2(reach))) : 1;
  for (auto &v : viewports) {
    v.originX -= (factor - 1) * v.width / 2, v.originY -= (factor - 1) * v.height / 2;
    v.width *= factor, v.height *= factor;
  }
  return float(1 / factor);
}

// what a line drawn as triangles keeps of the rasterizer state, which those of SM50_SHADER_PSO_GEOMETRY_SHADER_DATA's
// quad_lines are: a line is not culled, filled or biased, and faces front (D3D11.3 15.10, 15.13, 15.14)
template <typename State>
inline void
RasterizeAsLines(State &state) {
  state.fill_mode = WMTTriangleFillModeFill;
  state.cull_mode = WMTCullModeNone;
  state.depth_bias = state.scole_scale = state.depth_bias_clamp = 0;
  state.winding = WMTWindingCounterClockwise;
}

// Metal's blender drops the product of a Zero weight, where D3D propagates NaNs and signed zeros through it (D3D11.3
// 17.2, "including 0.0 blend weights"), and multiplies by a weight of the blend color at 0. a float
// target's Zero weights become that, when no target weighs by the blend color; the pipeline then draws with a blend
// color of 0, which this says. Metal folds one weight of both sides into (s op d) * w, which loses the sign of a zero,
// so the color channels' two take the blend color and its alpha; the alpha channel has no second weight that is 0
inline bool
MultiplyZeroWeights(std::span<WMTColorAttachmentBlendInfo> colors) {
  auto constant = [](WMTBlendFactor f) {
    return f == WMTBlendFactorBlendColor || f == WMTBlendFactorOneMinusBlendColor || f == WMTBlendFactorBlendAlpha ||
           f == WMTBlendFactorOneMinusBlendAlpha;
  };
  auto floating = [](WMTPixelFormat f) {
    switch (f) {
    case WMTPixelFormatR16Float:
    case WMTPixelFormatRG16Float:
    case WMTPixelFormatRGBA16Float:
    case WMTPixelFormatR32Float:
    case WMTPixelFormatRG32Float:
    case WMTPixelFormatRGBA32Float:
    case WMTPixelFormatRG11B10Float:
      return true;
    default:
      return false;
    }
  };
  bool zero = false;
  for (auto &c : colors)
    if (c.blending_enabled) {
      if (constant(c.src_rgb_blend_factor) || constant(c.dst_rgb_blend_factor) || constant(c.src_alpha_blend_factor) ||
          constant(c.dst_alpha_blend_factor))
        return false;
      zero |= floating(c.pixel_format) &&
              (c.src_rgb_blend_factor == WMTBlendFactorZero || c.dst_rgb_blend_factor == WMTBlendFactorZero ||
               c.src_alpha_blend_factor == WMTBlendFactorZero || c.dst_alpha_blend_factor == WMTBlendFactorZero);
    }
  if (!zero)
    return false;
  for (auto &c : colors)
    if (c.blending_enabled && floating(c.pixel_format))
      for (auto [factor, by] : {std::pair{&c.src_rgb_blend_factor, WMTBlendFactorBlendColor},
                                {&c.dst_rgb_blend_factor, WMTBlendFactorBlendAlpha},
                                {&c.src_alpha_blend_factor, WMTBlendFactorBlendAlpha},
                                {&c.dst_alpha_blend_factor, WMTBlendFactorBlendAlpha}})
        if (*factor == WMTBlendFactorZero)
          *factor = by;
  return true;
}

struct TileBarrierPSOKey {
  WMTPixelFormat color_formats[8];
  unsigned raster_sample_count;
};
} // namespace mullion

namespace std {
template <> struct hash<mullion::TileBarrierPSOKey> {
  size_t
  operator()(const mullion::TileBarrierPSOKey &v) const noexcept {
    constexpr size_t binsize = sizeof(v);
    return std::hash<string_view>{}({reinterpret_cast<const char *>(&v), binsize});
  };
};

template <> struct equal_to<mullion::TileBarrierPSOKey> {
  bool
  operator()(const mullion::TileBarrierPSOKey &x, const mullion::TileBarrierPSOKey &y) const {
    constexpr size_t binsize = sizeof(x);
    return std::string_view({reinterpret_cast<const char *>(&x), binsize}) ==
           std::string_view({reinterpret_cast<const char *>(&y), binsize});
  }
};
}; // namespace std

namespace mullion {

class ArgumentEncodingContext;

class InternalCommandLibrary {
public:
  InternalCommandLibrary(WMT::Device device);

  WMT::Library
  getLibrary() {
    return library_;
  }
  WMT::RenderPipelineState PipelineStatistics(bool scan = false);
  // zeroes a stream output draw's scratch inside its render pass (zero_words)
  WMT::RenderPipelineState ZeroWords();

private:
  // a vertex function that runs without rasterizing, made when first used
  WMT::RenderPipelineState VertexPipeline(WMT::Reference<WMT::RenderPipelineState> &pipeline, const char *name);

  WMT::Reference<WMT::Library> library_;
  WMT::Device device_;
  mullion::mutex statistics_mutex_;
  WMT::Reference<WMT::RenderPipelineState> pipeline_statistics_draw_[2], zero_words_;
};

class EmulatedCommandContext {
  friend class CommandQueue;
public:
  EmulatedCommandContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void
  ClearBufferUint(
      WMT::Buffer buffer, uint64_t byte_offset, uint64_t elements_uint, const std::array<uint32_t, 4> &value
  ) {
    setComputePipelineState(clear_buffer_uint_pipeline, {32, 1, 1});
    setComputeBuffer(buffer, byte_offset, 0);
    setComputeBytes(value.data(), 16, 1);
    setComputeBytes(&elements_uint, 4, 2);
    dispatchThreads({elements_uint, 1, 1});
  }

  void
  ClearTextureBufferUint(WMT::Texture texture, uint32_t offset, uint32_t size, const std::array<uint32_t, 4> &value) {
    setComputePipelineState(clear_texture_buffer_uint_pipeline, {32, 1, 1});
    setComputeTexture(texture, 0);
    struct CLEAR_UINT_DATA {
      const std::array<uint32_t, 4> value;
      uint32_t offset;
      uint32_t size;
      uint32_t padding[2];
    } data {value, offset, size};
    setComputeBytes(&data, sizeof(data), 1);
    dispatchThreads({size, 1, 1});
  }

  void
  ClearBufferFloat(
      WMT::Buffer buffer, uint64_t byte_offset, uint64_t elements_uint, const std::array<float, 4> &value
  ) {
    // just reinterpret float as uint
    setComputePipelineState(clear_buffer_uint_pipeline, {32, 1, 1});
    setComputeBuffer(buffer, byte_offset, 0);
    setComputeBytes(value.data(), 16, 1);
    setComputeBytes(&elements_uint, 4, 2);
    dispatchThreads({elements_uint, 1, 1});
  }

  void
  ClearTextureBufferFloat(WMT::Texture texture, uint32_t offset, uint32_t size, const std::array<float, 4> &value) {
    setComputePipelineState(clear_texture_buffer_float_pipeline, {32, 1, 1});
    struct CLEAR_FLOAT_DATA {
      const std::array<float, 4> value;
      uint32_t offset;
      uint32_t size;
      uint32_t padding[2];
    } data {value, offset, size};
    setComputeTexture(texture, 0);
    setComputeBytes(&data, sizeof(data), 1);
    dispatchThreads({size, 1, 1});
  }

  void
  ClearTexture2DUint(WMT::Texture texture, const std::array<uint32_t, 4> &value) {
    setComputePipelineState(clear_texture_2d_uint_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), 1});
  }

  void
  ClearTexture2DFloat(WMT::Texture texture, const std::array<float, 4> &value) {
    setComputePipelineState(clear_texture_2d_float_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), 1});
  }

  void
  ClearTexture2DArrayUint(WMT::Texture texture, const std::array<uint32_t, 4> &value) {
    setComputePipelineState(clear_texture_2d_array_uint_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), texture.arrayLength()});
  }

  void
  ClearTexture2DArrayFloat(WMT::Texture texture, const std::array<float, 4> &value) {
    setComputePipelineState(clear_texture_2d_array_float_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    dispatchThreads({texture.width(), texture.height(), texture.arrayLength()});
  }

  void
  ClearTexture3DFloat(
      WMT::Texture texture, const std::array<float, 4> &value, uint32_t firstSlice, uint32_t sliceCount
  ) {
    setComputePipelineState(clear_texture_3d_float_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    setComputeBytes(&firstSlice, sizeof(firstSlice), 2);
    dispatchThreads({texture.width(), texture.height(), sliceCount});
  }

  void
  ClearTexture3DUint(
      WMT::Texture texture, const std::array<uint32_t, 4> &value, uint32_t firstSlice, uint32_t sliceCount
  ) {
    setComputePipelineState(clear_texture_3d_uint_pipeline, {8, 4, 1});
    setComputeTexture(texture, 0);
    setComputeBytes(value.data(), 16, 1);
    setComputeBytes(&firstSlice, sizeof(firstSlice), 2);
    dispatchThreads({texture.width(), texture.height(), sliceCount});
  }

  void
  MarshalGSDispatchArguments(WMT::RenderCommandEncoder encoder, WMT::Buffer commands, uint32_t commands_offset) {
    encoder.setRenderPipelineState(gs_draw_arguments_marshal);
    encoder.setVertexBuffer(commands, commands_offset, kCustomBufferArgumentIndex0);
    encoder.drawPrimitives(WMTPrimitiveTypePoint, 0, 1);
    encoder.setVertexBuffer({}, 0, kCustomBufferArgumentIndex0);
  }

  void
  MarshalDrawAutoArguments(WMT::RenderCommandEncoder encoder, WMT::Buffer commands, uint32_t commands_offset) {
    encoder.setRenderPipelineState(draw_auto_arguments_marshal);
    encoder.setVertexBuffer(commands, commands_offset, kCustomBufferArgumentIndex0);
    encoder.drawPrimitives(WMTPrimitiveTypePoint, 0, 1);
    encoder.setVertexBuffer({}, 0, kCustomBufferArgumentIndex0);
  }

  void
  MarshalTSDispatchArguments(WMT::RenderCommandEncoder encoder, WMT::Buffer commands, uint32_t commands_offset) {
    encoder.setRenderPipelineState(ts_draw_arguments_marshal);
    encoder.setVertexBuffer(commands, commands_offset, kCustomBufferArgumentIndex0);
    encoder.drawPrimitives(WMTPrimitiveTypePoint, 0, 1);
    encoder.setVertexBuffer({}, 0, kCustomBufferArgumentIndex0);
  }

  uint32_t SIMDWidth() const { return clear_buffer_uint_pipeline.threadExecutionWidth(); }

private:
  void setComputePipelineState(WMT::ComputePipelineState state, const WMTSize &threadgroup_size);

  void dispatchThreads(const WMTSize &grid_size);

  void setComputeBuffer(WMT::Buffer buffer, uint64_t offset, uint8_t index);

  void setComputeTexture(WMT::Texture texture, uint8_t index);

  void setComputeBytes(const void *buf, uint64_t length, uint8_t index);

  ArgumentEncodingContext &ctx;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_array_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_array_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_3d_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_buffer_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_buffer_uint_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_1d_array_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_2d_array_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_3d_float_pipeline;
  WMT::Reference<WMT::ComputePipelineState> clear_texture_buffer_float_pipeline;

  WMT::Reference<WMT::RenderPipelineState> gs_draw_arguments_marshal;
  WMT::Reference<WMT::RenderPipelineState> ts_draw_arguments_marshal;
  WMT::Reference<WMT::RenderPipelineState> draw_auto_arguments_marshal;
};

class DepthStencilBlitContext {

public:
  DepthStencilBlitContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void copyFromBuffer(
      const Rc<Buffer> &src, uint64_t src_offset, uint64_t src_length, uint32_t bytes_per_row, uint32_t bytes_per_image,
      const Rc<Texture> &depth_stencil, uint32_t level, uint32_t slice, bool from_d24s8
  );

  void copyFromTexture(
      const Rc<Texture> &depth_stencil, uint32_t level, uint32_t slice, const Rc<Buffer> &dst, uint64_t dst_offset,
      uint64_t dst_length, uint32_t bytes_per_row, uint32_t bytes_per_image, bool to_d24s8
  );

private:
  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::DepthStencilState> depth_stencil_state_;
  WMT::Reference<WMT::RenderPipelineState> pso_copy_d24s8_;
  WMT::Reference<WMT::RenderPipelineState> pso_copy_d32s8_;
  WMT::Reference<WMT::ComputePipelineState> pso_copy_to_buffer_d24s8_;
  WMT::Reference<WMT::ComputePipelineState> pso_copy_to_buffer_d32s8_;
};

class MTLFXMVScaleContext {
public:
  MTLFXMVScaleContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void dispatch(
      const Rc<Texture> &dilated, TextureViewKey view_dilated, const Rc<Texture> &downscaled,
      TextureViewKey view_downscaled, float mv_scale_x, float mv_scale_y
  );

private:
  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::ComputePipelineState> pso_downscale_dilated_mv_;
};

constexpr auto kBarrierTileSize = 16;

class TileBarrierContext {
public:
  TileBarrierContext(WMT::Device device, InternalCommandLibrary &lib, ArgumentEncodingContext &ctx);

  void dispatch();

private:
  WMT::RenderPipelineState getPSO(TileBarrierPSOKey &format);

  ArgumentEncodingContext &ctx_;
  WMT::Device device_;
  WMT::Reference<WMT::Function> tile_function_;
  std::unordered_map<TileBarrierPSOKey, WMT::Reference<WMT::RenderPipelineState>> psos_;
};

} // namespace mullion