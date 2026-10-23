#pragma once

#include "d3d11_device.hpp"
#include "d3d11_fence.hpp"
#include "d3d11on12.h"
#include "dxgi_interfaces.h"
#include "com/com_pointer.hpp"
#include "com/com_guid.hpp"

// ID3D11On12Device1 and 2, which the bundled headers (include/native/directx) predate. as Wine declares them
// (include/d3d11on12.idl), the same ABI as the Windows SDK's
#ifndef __ID3D11On12Device1_INTERFACE_DEFINED__
#define __ID3D11On12Device1_INTERFACE_DEFINED__
DEFINE_COM_INTERFACE("bdb64df4-ea2f-4c70-b861-aaab1258bb5d", ID3D11On12Device1) : public ID3D11On12Device {
  virtual HRESULT STDMETHODCALLTYPE GetD3D12Device(REFIID riid, ID3D12Device * *ppvDevice) = 0;
};
#endif
#ifndef __ID3D11On12Device2_INTERFACE_DEFINED__
#define __ID3D11On12Device2_INTERFACE_DEFINED__
DEFINE_COM_INTERFACE("dc90f331-4740-43fa-866e-67f12cb58223", ID3D11On12Device2) : public ID3D11On12Device1 {
  virtual HRESULT STDMETHODCALLTYPE UnwrapUnderlyingResource(
      ID3D11Resource * pResource11, ID3D12CommandQueue * pCommandQueue, REFIID riid, void **ppvResource12
  ) = 0;
  virtual HRESULT STDMETHODCALLTYPE ReturnUnderlyingResource(
      ID3D11Resource * pResource11, UINT NumSync, UINT64 * pSignalValues, ID3D12Fence * *ppFences
  ) = 0;
};
#endif

namespace mullion {

// a Metal shared event in the shape of a D3D11 fence, which the immediate context's Signal and Wait take: they read
// its event and keep nothing. it is no device's child and holds no reference to one, as a D3D11 fence would
class D3D11On12Event final : public MTLD3D11Fence {
public:
  explicit D3D11On12Event(WMT::Device device) {
    event = device.newSharedEvent();
  }

  HRESULT STDMETHODCALLTYPE
  QueryInterface(REFIID riid, void **ppvObject) final {
    *ppvObject = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE
  AddRef() final {
    return 1;
  }
  ULONG STDMETHODCALLTYPE
  Release() final {
    return 1;
  }
  void STDMETHODCALLTYPE
  GetDevice(ID3D11Device **ppDevice) final {
    *ppDevice = nullptr;
  }
  HRESULT STDMETHODCALLTYPE
  GetPrivateData(REFGUID, UINT *, void *) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  SetPrivateData(REFGUID, UINT, const void *) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  SetPrivateDataInterface(REFGUID, const IUnknown *) final {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE
  CreateSharedHandle(const SECURITY_ATTRIBUTES *, DWORD, LPCWSTR, HANDLE *) final {
    return E_NOTIMPL;
  }
  UINT64 STDMETHODCALLTYPE
  GetCompletedValue() final {
    return event.signaledValue();
  }
  HRESULT STDMETHODCALLTYPE
  SetEventOnCompletion(UINT64, HANDLE) final {
    return E_NOTIMPL;
  }
};

// a D3D11 device made by D3D11On12CreateDevice: a Mullion D3D11 device on the Metal device of a Mullion D3D12 one,
// part of the D3D11 device's object (its reference count is the device's). D3D11's work and a D3D12 queue's meet in
// Metal shared events, a new one each time: the D3D12 side signals one the D3D11 context waits for
// (AcquireWrappedResources, ReturnUnderlyingResource), or the context signals one the queue waits for
// (ReleaseWrappedResources, UnwrapUnderlyingResource). a wrapped resource is the D3D12 resource's own Metal texture or
// buffer, so both see one memory
class D3D11On12Device final : public ID3D11On12Device2 {
public:
  D3D11On12Device(
      IUnknown *container, MTLD3D11Device *device, ID3D12Device *d3d12, IMTLD3D11On12Source *source,
      ID3D12CommandQueue *queue
  );

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) final;
  ULONG STDMETHODCALLTYPE AddRef() final;
  ULONG STDMETHODCALLTYPE Release() final;

  HRESULT STDMETHODCALLTYPE CreateWrappedResource(
      IUnknown *pResource12, const D3D11_RESOURCE_FLAGS *pFlags11, D3D12_RESOURCE_STATES InState,
      D3D12_RESOURCE_STATES OutState, REFIID riid, void **ppResource11
  ) final;
  void STDMETHODCALLTYPE ReleaseWrappedResources(ID3D11Resource *const *ppResources, UINT NumResources) final;
  void STDMETHODCALLTYPE AcquireWrappedResources(ID3D11Resource *const *ppResources, UINT NumResources) final;

  HRESULT STDMETHODCALLTYPE GetD3D12Device(REFIID riid, ID3D12Device **ppvDevice) final;

  HRESULT STDMETHODCALLTYPE UnwrapUnderlyingResource(
      ID3D11Resource *pResource11, ID3D12CommandQueue *pCommandQueue, REFIID riid, void **ppvResource12
  ) final;
  HRESULT STDMETHODCALLTYPE ReturnUnderlyingResource(
      ID3D11Resource *pResource11, UINT NumSync, UINT64 *pSignalValues, ID3D12Fence **ppFences
  ) final;

private:
  // the queue's work so far is done before the D3D11 context's later work starts
  HRESULT ContextAfterQueue(ID3D12CommandQueue *queue);
  // the D3D11 context's work so far is done before the queue's later work starts
  HRESULT QueueAfterContext(ID3D12CommandQueue *queue);
  Com<ID3D11DeviceContext4> Context();

  IUnknown *container_;
  MTLD3D11Device *device_;
  Com<ID3D12Device> d3d12_;
  Com<IMTLD3D11On12Source> source_;
  Com<ID3D12CommandQueue> queue_;
};

// the private data key under which a wrapped D3D11 resource keeps its D3D12 resource
// {7c4bf8f6-3b56-4ea4-9a1e-58a3b2c4e9d1}
static constexpr GUID D3D11On12WrappedResource = {
    0x7c4bf8f6, 0x3b56, 0x4ea4, {0x9a, 0x1e, 0x58, 0xa3, 0xb2, 0xc4, 0xe9, 0xd1}
};

// the D3D11On12 part of a D3D11 device made on a D3D12 one (d3d11_device.cpp)
HRESULT CreateD3D11On12Device(
    IMTLD3D11On12Source *source, ID3D12Device *d3d12, ID3D12CommandQueue *queue, D3D_FEATURE_LEVEL feature_level,
    UINT flags, ID3D11Device **ppDevice
);

} // namespace mullion
