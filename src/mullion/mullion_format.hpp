#pragma once
#include <array>
#include <functional>

#include "Metal.hpp"
#include "dxgi.h"
#include <map>

namespace mullion {
enum class FormatCapability : int {
  None = 0,
  Atomic = 0x1,
  Filter = 0x2,
  Write = 0x4,
  Color = 0x8,
  Blend = 0x10,
  MSAA = 0x20,
  Sparse = 0x40,
  Resolve = 0x80,
  DepthStencil = 0x100,
  TextureBufferRead = 0x200,
  TextureBufferWrite = 0x400,
  TextureBufferReadWrite = 0x800
};

class FormatCapabilityInspector {
public:
  std::map<WMTPixelFormat, FormatCapability> textureCapabilities{};
  void Inspect(WMT::Device device);
};

WMTPixelFormat ConvertSwapChainFormat(DXGI_FORMAT format);
bool IsFlipSwapChainFormat(DXGI_FORMAT format);

WMTPixelFormat Forget_sRGB(WMTPixelFormat format);
WMTPixelFormat Recall_sRGB(WMTPixelFormat format);

uint32_t DXGIFormatType(uint32_t format);

inline bool
Is_sRGBVariant(WMTPixelFormat format) {
  return Forget_sRGB(format) != format;
}

bool IsBlockCompressionFormat(WMTPixelFormat format);

uint32_t DepthStencilPlanarFlags(WMTPixelFormat format);

enum MTL_DXGI_FORMAT_FLAG {
  MTL_DXGI_FORMAT_TYPELESS = 1,
  MTL_DXGI_FORMAT_BC = 2,
  MTL_DXGI_FORMAT_BACKBUFFER = 4,
  MTL_DXGI_FORMAT_DEPTH_PLANER = 16,
  MTL_DXGI_FORMAT_STENCIL_PLANER = 32,
  MTL_DXGI_FORMAT_EMULATED_D24 = 256,
  MTL_DXGI_FORMAT_EMULATED_LINEAR_DEPTH_STENCIL = 512,
  MTL_DXGI_FORMAT_VIDEO_PLANAR = 1024,
  MTL_DXGI_FORMAT_EMULATED_RGB32 = 2048,
};

struct MTL_DXGI_FORMAT_DESC {
  WMTPixelFormat PixelFormat;
  WMTPixelFormat BufferPixelFormat;
  WMTAttributeFormat AttributeFormat;
  union {
    uint32_t BytesPerTexel;
    uint32_t BlockSize;
  };
  uint16_t PlanarCount;
  uint16_t Flag;
  uint32_t PlaneFormat = 0;
  uint16_t SubsampleWidth = 1;
  uint16_t SubsampleHeight = 1;
};

int32_t MTLQueryDXGIFormat(
    WMT::Device device, uint32_t format, MTL_DXGI_FORMAT_DESC &description, uint32_t plane = ~0u
);

int32_t MTLQueryDXGIFormatSupport(
    WMT::Device device, uint32_t format, const std::function<FormatCapability(WMTPixelFormat)> &GetMTLPixelFormatCapability,
    uint32_t &support1, uint32_t &support2
);

void CopyRGB32(void *dst, const void *src, size_t count, bool expand);

uint32_t MTLGetTexelSize(WMTPixelFormat format);

FormatCapability MTLGetDXGIFormatCapability(const MTL_DXGI_FORMAT_DESC &format, FormatCapability capability);

WMTPixelFormat MTLGetUnsignedIntegerFormat(WMTPixelFormat format);

bool IsUnorm8RenderTargetFormat(WMTPixelFormat format);

bool IsIntegerFormat(WMTPixelFormat format);

void SanitizeRTVClearColor(WMTPixelFormat format, WMTClearColor &color);

// the range a border color clamps to before filtering (D3D11.3 7.18.9.1): normalized formats' own, the rest as their
// clear colors clamp
std::array<float, 2> BorderColorRange(const MTL_DXGI_FORMAT_DESC &description);

} // namespace mullion