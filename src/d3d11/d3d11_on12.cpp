#include "d3d11_on12.hpp"
#include "d3d11_resource.hpp"
#include "log/log.hpp"

namespace mullion {

D3D11On12Device::D3D11On12Device(
    IUnknown *container, MTLD3D11Device *device, ID3D12Device *d3d12, IMTLD3D11On12Source *source,
    ID3D12CommandQueue *queue
) :
    container_(container),
    device_(device),
    d3d12_(d3d12),
    source_(source),
    queue_(queue) {}

HRESULT STDMETHODCALLTYPE
D3D11On12Device::QueryInterface(REFIID riid, void **ppvObject) {
  if (!ppvObject)
    return E_POINTER;
  *ppvObject = nullptr;
  if (riid == __uuidof(ID3D11On12Device) || riid == __uuidof(ID3D11On12Device1) ||
      riid == __uuidof(ID3D11On12Device2)) {
    *ppvObject = ref(this);
    return S_OK;
  }
  return container_->QueryInterface(riid, ppvObject);
}

ULONG STDMETHODCALLTYPE
D3D11On12Device::AddRef() {
  return container_->AddRef();
}

ULONG STDMETHODCALLTYPE
D3D11On12Device::Release() {
  return container_->Release();
}

Com<ID3D11DeviceContext4>
D3D11On12Device::Context() {
  Com<ID3D11DeviceContext> context;
  Com<ID3D11DeviceContext4> context4;
  device_->GetImmediateContext(&context);
  context->QueryInterface(IID_PPV_ARGS(&context4));
  return context4;
}

HRESULT
D3D11On12Device::ContextAfterQueue(ID3D12CommandQueue *queue) {
  D3D11On12Event event(device_->GetMTLDevice());
  HRESULT hr = source_->SignalEventAfterQueue(queue, event.event, 1);
  if (FAILED(hr))
    return hr;
  return Context()->Wait(&event, 1);
}

HRESULT
D3D11On12Device::QueueAfterContext(ID3D12CommandQueue *queue) {
  D3D11On12Event event(device_->GetMTLDevice());
  HRESULT hr = Context()->Signal(&event, 1);
  if (FAILED(hr))
    return hr;
  return source_->QueueWaitForEvent(queue, event.event, 1);
}

// the D3D11 bindings a D3D12 resource's flags allow, which a wrapped resource without D3D11_RESOURCE_FLAGS takes
static UINT
DefaultBindFlags(const D3D12_RESOURCE_DESC &desc) {
  UINT bind = 0;
  if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
    bind |= D3D11_BIND_SHADER_RESOURCE;
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
    bind |= D3D11_BIND_RENDER_TARGET;
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
    bind |= D3D11_BIND_DEPTH_STENCIL;
  if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)
    bind |= D3D11_BIND_UNORDERED_ACCESS;
  return bind;
}

HRESULT STDMETHODCALLTYPE
D3D11On12Device::CreateWrappedResource(
    IUnknown *pResource12, const D3D11_RESOURCE_FLAGS *pFlags11, D3D12_RESOURCE_STATES InState,
    D3D12_RESOURCE_STATES OutState, REFIID riid, void **ppResource11
) {
  InitReturnPtr(ppResource11);
  if (!pResource12 || !ppResource11)
    return E_INVALIDARG;
  Com<ID3D12Resource> resource12;
  if (FAILED(pResource12->QueryInterface(IID_PPV_ARGS(&resource12))))
    return E_INVALIDARG;

  obj_handle_t texture = NULL_OBJECT_HANDLE, buffer = NULL_OBJECT_HANDLE;
  UINT64 gpu_address = 0;
  void *mapped = nullptr;
  if (FAILED(source_->GetResourceObjects(resource12.ptr(), &texture, &buffer, &gpu_address, &mapped))) {
    ERR("CreateWrappedResource: not a resource of this device, or one without a Metal object of its own");
    return E_INVALIDARG;
  }
  WMT::Reference<WMT::Texture> metal_texture(texture);
  WMT::Reference<WMT::Buffer> metal_buffer(buffer);

  auto desc12 = resource12->GetDesc();
  D3D11_RESOURCE_FLAGS flags11 = {};
  if (pFlags11)
    flags11 = *pFlags11;
  else
    flags11.BindFlags = DefaultBindFlags(desc12);

  // D3D11's bindings must be ones the D3D12 resource allows
  UINT allowed = DefaultBindFlags(desc12);
  UINT bind_needing_flag =
      D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_UNORDERED_ACCESS;
  if (flags11.BindFlags & bind_needing_flag & ~allowed) {
    ERR("CreateWrappedResource: bind flags ", flags11.BindFlags, " the D3D12 resource's flags ", desc12.Flags,
        " don't allow");
    return E_INVALIDARG;
  }

  HRESULT hr = E_INVALIDARG;
  Com<ID3D11Resource> resource11;
  switch (desc12.Dimension) {
  case D3D12_RESOURCE_DIMENSION_BUFFER: {
    if (!metal_buffer || desc12.Width > UINT_MAX)
      return E_INVALIDARG;
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = UINT(desc12.Width);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = flags11.BindFlags;
    desc.CPUAccessFlags = flags11.CPUAccessFlags;
    desc.MiscFlags = flags11.MiscFlags;
    desc.StructureByteStride = flags11.StructureByteStride;
    hr = WrapBuffer(device_, &desc, std::move(metal_buffer), gpu_address, mapped, IID_PPV_ARGS(&resource11));
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE1D: {
    if (!metal_texture || flags11.CPUAccessFlags)
      return E_INVALIDARG;
    D3D11_TEXTURE1D_DESC desc = {};
    desc.Width = UINT(desc12.Width);
    desc.MipLevels = desc12.MipLevels;
    desc.ArraySize = desc12.DepthOrArraySize;
    desc.Format = desc12.Format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = flags11.BindFlags;
    desc.MiscFlags = flags11.MiscFlags;
    hr = WrapTexture(
        device_, D3D11_RESOURCE_DIMENSION_TEXTURE1D, &desc, std::move(metal_texture), IID_PPV_ARGS(&resource11)
    );
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE2D: {
    if (!metal_texture || flags11.CPUAccessFlags)
      return E_INVALIDARG;
    D3D11_TEXTURE2D_DESC1 desc = {};
    desc.Width = UINT(desc12.Width);
    desc.Height = desc12.Height;
    desc.MipLevels = desc12.MipLevels;
    desc.ArraySize = desc12.DepthOrArraySize;
    desc.Format = desc12.Format;
    desc.SampleDesc = desc12.SampleDesc;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = flags11.BindFlags;
    desc.MiscFlags = flags11.MiscFlags;
    desc.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
    hr = WrapTexture(
        device_, D3D11_RESOURCE_DIMENSION_TEXTURE2D, &desc, std::move(metal_texture), IID_PPV_ARGS(&resource11)
    );
    break;
  }
  case D3D12_RESOURCE_DIMENSION_TEXTURE3D: {
    if (!metal_texture || flags11.CPUAccessFlags)
      return E_INVALIDARG;
    D3D11_TEXTURE3D_DESC1 desc = {};
    desc.Width = UINT(desc12.Width);
    desc.Height = desc12.Height;
    desc.Depth = desc12.DepthOrArraySize;
    desc.MipLevels = desc12.MipLevels;
    desc.Format = desc12.Format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = flags11.BindFlags;
    desc.MiscFlags = flags11.MiscFlags;
    desc.TextureLayout = D3D11_TEXTURE_LAYOUT_UNDEFINED;
    hr = WrapTexture(
        device_, D3D11_RESOURCE_DIMENSION_TEXTURE3D, &desc, std::move(metal_texture), IID_PPV_ARGS(&resource11)
    );
    break;
  }
  default:
    break;
  }
  if (FAILED(hr))
    return hr;

  resource11->SetPrivateDataInterface(D3D11On12WrappedResource, resource12.ptr());
  // a wrapped resource is created acquired: D3D11 sees what the queue's work so far left in it
  if (FAILED(hr = ContextAfterQueue(queue_.ptr())))
    return hr;
  return resource11->QueryInterface(riid, ppResource11);
}

void STDMETHODCALLTYPE
D3D11On12Device::ReleaseWrappedResources(ID3D11Resource *const *ppResources, UINT NumResources) {
  // D3D11 has the queue's place: the queue's later work starts after D3D11's work so far
  if (FAILED(QueueAfterContext(queue_.ptr())))
    ERR("ReleaseWrappedResources: failed to order the queue after D3D11");
}

void STDMETHODCALLTYPE
D3D11On12Device::AcquireWrappedResources(ID3D11Resource *const *ppResources, UINT NumResources) {
  if (FAILED(ContextAfterQueue(queue_.ptr())))
    ERR("AcquireWrappedResources: failed to order D3D11 after the queue");
}

HRESULT STDMETHODCALLTYPE
D3D11On12Device::GetD3D12Device(REFIID riid, ID3D12Device **ppvDevice) {
  if (!ppvDevice)
    return E_POINTER;
  return d3d12_->QueryInterface(riid, reinterpret_cast<void **>(ppvDevice));
}

// the D3D12 resource a D3D11 resource wraps, or null
static Com<ID3D12Resource>
WrappedResource(ID3D11Resource *resource11) {
  IUnknown *resource12 = nullptr;
  UINT size = sizeof(resource12);
  if (!resource11 || FAILED(resource11->GetPrivateData(D3D11On12WrappedResource, &size, &resource12)) || !resource12)
    return nullptr;
  Com<ID3D12Resource> result;
  resource12->QueryInterface(IID_PPV_ARGS(&result));
  resource12->Release();
  return result;
}

HRESULT STDMETHODCALLTYPE
D3D11On12Device::UnwrapUnderlyingResource(
    ID3D11Resource *pResource11, ID3D12CommandQueue *pCommandQueue, REFIID riid, void **ppvResource12
) {
  InitReturnPtr(ppvResource12);
  auto resource12 = WrappedResource(pResource11);
  if (!resource12 || !pCommandQueue || !ppvResource12)
    return E_INVALIDARG;
  HRESULT hr = QueueAfterContext(pCommandQueue);
  if (FAILED(hr))
    return hr;
  return resource12->QueryInterface(riid, ppvResource12);
}

HRESULT STDMETHODCALLTYPE
D3D11On12Device::ReturnUnderlyingResource(
    ID3D11Resource *pResource11, UINT NumSync, UINT64 *pSignalValues, ID3D12Fence **ppFences
) {
  if (!WrappedResource(pResource11) || (NumSync && (!pSignalValues || !ppFences)))
    return E_INVALIDARG;
  // D3D11's later work waits for each fence, each with an event of its own: fences reach their values in any order
  auto context = Context();
  for (UINT i = 0; i < NumSync; i++) {
    D3D11On12Event event(device_->GetMTLDevice());
    HRESULT hr = source_->SignalEventAtFence(ppFences[i], pSignalValues[i], event.event, 1);
    if (FAILED(hr))
      return hr;
    if (FAILED(hr = context->Wait(&event, 1)))
      return hr;
  }
  return S_OK;
}

} // namespace mullion
