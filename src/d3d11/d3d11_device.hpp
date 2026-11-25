#pragma once

#include "com/com_guid.hpp"
#include "com/com_pointer.hpp"
#include "d3d11_3.h"
#include "d3d11_multithread.hpp"
#include "dxgi_interfaces.h"
#include "mullion_device.hpp"
#include "mullion_format.hpp"
#include "mullion_texture.hpp"
#include "thread.hpp"
#include <atomic>
#include <utility>
#include <vector>

struct MTL_GRAPHICS_PIPELINE_DESC;
struct MTL_COMPUTE_PIPELINE_DESC;
struct IMTLD3D11RasterizerState;

DEFINE_COM_INTERFACE("3a3f085a-d0fe-4324-b0ae-fe04de18571c",
                     IMTLD3D11DeviceContext)
    : public ID3D11DeviceContext4 {
  virtual void WaitUntilGPUIdle() = 0;
  virtual void PrepareFlush() = 0;
  virtual void UnbindResource(ID3D11Resource *pResource) = 0;
  virtual void Commit() = 0;
  virtual HRESULT WaitEvent(WMT::SharedEvent event, UINT64 value, ID3D12Fence *fence) = 0;
  virtual HRESULT SignalFence(ID3D12Fence *fence, UINT64 value) = 0;
  virtual HRESULT ReturnResource(ID3D11Resource *resource, std::function<void(mullion::ArgumentEncodingContext &)> &&before) = 0;
  virtual void SOGetTargetsWithOffsets(UINT NumBuffers, REFIID riid, void ** ppSOTargets, UINT *pOffsets) = 0;
  virtual HRESULT AcquireSync(ID3D11Resource *pResource, UINT64 Key, DWORD dwMilliseconds) = 0;
  virtual HRESULT ReleaseSync(ID3D11Resource *pResource, UINT64 Key) = 0;
};

namespace mullion {

class MTLCompiledGraphicsPipeline;
class MTLCompiledComputePipeline;
class MTLCompiledGeometryPipeline;
class MTLCompiledTessellationMeshPipeline;

// render targets released lately, with their memory resident: a texture of the same Metal description made next takes
// one instead of new memory, which the kernel wires and zeroes at the first submission naming it (5.1 ms a frame for
// four 4096x4096 RGBA8 targets on an M6, d3d11_perf_probe big_targets). such a texture is zeroed when made all the
// same (ResourceInitializer::initWithZero), as a new one is. it holds at most a sixteenth of the working set, and no
// more than 512 MiB, for 2 seconds, left out of QueryVideoMemoryInfo's usage; MULLION_D3D11_TEXTURE_POOL=0 is without
// it
class D3D11TexturePool final : public TextureRecycler, public std::enable_shared_from_this<D3D11TexturePool> {
public:
  D3D11TexturePool(WMT::Device device, IMTLDXGIAdapter *adapter) : device_(device), adapter_(adapter) {}
  bool recycle(TextureAllocation *allocation) override;
  // the latest given back of this description, or none
  Rc<TextureAllocation> take(const WMTTextureInfo &info);
  // gives back what was pooled before `age` milliseconds ago, everything with 0
  void age(uint64_t age);
  void
  age() {
    uint64_t oldest = oldest_.load(std::memory_order_relaxed);
    if (oldest && GetTickCount64() - oldest > kAge)
      age(kAge);
  }
  // the device goes: nothing more is pooled
  void close();
  static constexpr uint64_t kAge = 2000;
  static constexpr uint64_t kLeast = 256 << 10;

private:
  struct Entry {
    Rc<TextureAllocation> allocation;
    uint64_t at;
  };
  WMT::Device device_;
  IMTLDXGIAdapter *adapter_;
  mullion::mutex mutex_;
  std::vector<Entry> entries_;
  uint64_t bytes_ = 0;
  bool closed_ = false;
  std::atomic<uint64_t> oldest_ = 0;
  void evict(std::vector<Rc<TextureAllocation>> &freed, size_t index);
};

class MTLD3D11Device : public ID3D11Device5 {
public:

  virtual WMT::Device STDMETHODCALLTYPE GetMTLDevice() = 0;

  virtual HRESULT CreateGraphicsPipeline(MTL_GRAPHICS_PIPELINE_DESC * pDesc,
                                         MTLCompiledGraphicsPipeline *
                                             *ppPipeline) = 0;

  virtual HRESULT CreateComputePipeline(MTL_COMPUTE_PIPELINE_DESC * pDesc,
                                        MTLCompiledComputePipeline *
                                            *ppPipeline) = 0;

  virtual HRESULT CreateGeometryPipeline(MTL_GRAPHICS_PIPELINE_DESC * pDesc,
                                             MTLCompiledGeometryPipeline *
                                                 *ppPipeline) = 0;

  virtual HRESULT CreateTessellationMeshPipeline(MTL_GRAPHICS_PIPELINE_DESC * pDesc,
                                             MTLCompiledTessellationMeshPipeline *
                                                 *ppPipeline) = 0;

  virtual bool IsTraced() = 0;

  virtual IMTLD3D11RasterizerState *GetDefaultRasterizerState() = 0;

  virtual Device& GetMullionDevice() = 0;

  virtual void CreateCommandList(ID3D11CommandList** pCommandList) = 0;

  virtual FormatCapability GetMTLPixelFormatCapability(WMTPixelFormat Format) = 0;

  virtual IMTLD3D11DeviceContext *GetImmediateContextPrivate() = 0;

  virtual unsigned int GetDirectXVersion() = 0;

  d3d11_device_mutex mutex;
  bool shares_with_d3d12 = false;

  // a view of `texture` in `format` that casts it without Metal's format view (Texture::deferFormatView) makes the
  // allocation with it the immediate context moves the texture to at its next operation, before the view's first use
  // (TakeFormatViewMoves). false where the allocation cannot be made
  bool PrepareFormatView(Texture *texture, WMTPixelFormat format);
  bool FormatViewMovesPending() const { return format_view_pending_.load(std::memory_order_acquire); }
  std::vector<std::pair<Rc<Texture>, Rc<TextureAllocation>>> TakeFormatViewMoves();

  // released render targets (D3D11TexturePool), closed with the device
  std::shared_ptr<D3D11TexturePool> texture_pool;
  struct TexturePoolClose {
    MTLD3D11Device *device;
    ~TexturePoolClose() {
      if (device->texture_pool)
        device->texture_pool->close();
    }
  } texture_pool_close{this};

private:
  mullion::mutex format_view_mutex_;
  std::vector<std::pair<Rc<Texture>, Rc<TextureAllocation>>> format_view_moves_; // under format_view_mutex_
  std::atomic<bool> format_view_pending_ = false;
};

D3D_FEATURE_LEVEL GetMaxFeatureLevel(WMT::Device device);

Com<IMTLDXGIDevice> CreateD3D11Device(std::unique_ptr<Device> &&device,
                                      IMTLDXGIAdapter *pAdapter,
                                      D3D_FEATURE_LEVEL FeatureLevel,
                                      UINT Flags);
} // namespace mullion
