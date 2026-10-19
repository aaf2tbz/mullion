#pragma once

#include "../airconv/airconv_sparse.h"

#ifdef __METAL__
#define CONSTANT constant
#else
#define CONSTANT
#endif

namespace dxmt {

constexpr CONSTANT auto kCustomBufferArgumentIndex0 = 30;
constexpr CONSTANT auto kCustomBufferArgumentIndex1 = 29;
constexpr CONSTANT auto kSparseBufferArgumentIndex = kCustomBufferArgumentIndex1 - 1;

struct RGB32CopyLayout {
  sm50_sparse_qword src_row, src_image, dst_row, dst_image;
};

struct SparseTexelClear {
  sm50_sparse_qword byte_offset;
  sm50_sparse_qword texel_size;
};

// the draw's pixel invocations and strip primitive correction, captured before its index buffer can change
struct PipelineStatisticsDraw {
  sm50_sparse_qword indices, index_bytes, result, skips, arguments;
  sm50_sparse_qword primitives;
  sm50_sparse_qword vertices, instances, start, index_stride;
  sm50_sparse_qword first, step, cut, threads;
};

struct PipelineStatisticsResult {
  sm50_sparse_qword pixels, primitives, vertices;
};

constexpr CONSTANT auto kPresentFCIndex_BackbufferSizeMatched = 0x100;
constexpr CONSTANT auto kPresentFCIndex_BackbufferIsSRGB = 0x103;
constexpr CONSTANT auto kPresentFCIndex_HDRPQ = 0x101;
constexpr CONSTANT auto kPresentFCIndex_WithHDRMetadata = 0x102;
constexpr CONSTANT auto kPresentFCIndex_BackbufferIsMS = 0x104;
constexpr CONSTANT auto kPresentFCIndex_GammaEnabled = 0x105;
constexpr CONSTANT auto kPresentFCIndex_BackbufferIsXRBias = kPresentFCIndex_GammaEnabled + 1;

} // namespace dxmt
