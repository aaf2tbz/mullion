// the runs tests/flight.py reads the flight recorder's dumps of. argv: <removed|exception|quiet|many|d3d11|cost>.
// each says what it did in 'fact' lines, which the dump must hold in the same order.
// removed: a list executes, the queue signals a fence and waits for it, and the device is removed
// (ID3D12Device5::RemoveDevice). exception: two continuable access violations (RaiseException), which a handler of
// the program's own, added last, continues. quiet: the work of removed with no removal. many: the queue signals the
// fence more times than the ring is said to hold, then the device is removed. d3d11: a cleared and flushed context,
// waited for with an event query. cost: how long a record takes.
#include "d3d12_test.hpp"
#include <d3d11.h>
#include <cstring>

static LONG CALLBACK
resume(EXCEPTION_POINTERS *pointers) {
  return pointers->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_CONTINUE_EXECUTION
                                                                                : EXCEPTION_CONTINUE_SEARCH;
}

static int
d3d11() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device,
                          nullptr, &context));
  D3D11_TEXTURE2D_DESC desc{4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                            D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> texture;
  ComPtr<ID3D11RenderTargetView> view;
  CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
  CHECK(device->CreateRenderTargetView(texture.Get(), nullptr, &view));
  const float color[4] = {1, 0, 0, 1};
  context->ClearRenderTargetView(view.Get(), color);
  D3D11_QUERY_DESC query_desc{D3D11_QUERY_EVENT};
  ComPtr<ID3D11Query> query;
  CHECK(device->CreateQuery(&query_desc, &query));
  context->End(query.Get());
  context->Flush();
  BOOL done = FALSE;
  while (context->GetData(query.Get(), &done, sizeof(done), 0) != S_OK || !done)
    Sleep(1);
  printf("fact flushed\n");
  return verdict();
}

int
main(int argc, char **argv) {
  if (!expect(argc == 2, "usage: d3d12_flight <removed|exception|quiet|many|d3d11|cost>"))
    return verdict();
  std::string mode = argv[1];
  if (mode == "exception") {
    AddVectoredExceptionHandler(0, resume);
    for (ULONG_PTR address : {0x1000, 0x2000}) {
      const ULONG_PTR information[2] = {1, address};
      RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 2, information);
      printf("fact exception %#llx\n", (unsigned long long)address);
    }
    return verdict();
  }
  if (mode == "d3d11")
    return d3d11();
  if (mode == "cost") {
    // what a record costs its thread: records for a second of QueryPerformanceCounter time, in rounds of a ring's
    // worth said generously, and the time a record took on average
    auto metal = LoadLibraryA("winemetal.dll");
    auto site = (const void *(*)(const char *))GetProcAddress(metal, "WMTFlightSite");
    auto record = (void (*)(const void *, uint64_t, uint64_t, uint64_t, uint64_t))GetProcAddress(metal, "WMTFlightRecord");
    if (!expect(site && record, "winemetal.dll has no flight recorder"))
      return verdict();
    auto at = site("cost %llu %llu %llu %llu");
    LARGE_INTEGER frequency, began, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&began);
    uint64_t records = 0;
    do {
      for (uint64_t i = 0; i < 65536; i++)
        record(at, records + i, 1, 2, 3);
      records += 65536;
      QueryPerformanceCounter(&now);
    } while (now.QuadPart - began.QuadPart < frequency.QuadPart);
    printf("fact cost %.1f\n", double(now.QuadPart - began.QuadPart) * 1e9 / frequency.QuadPart / records);
    return verdict();
  }

  ComPtr<ID3D12Device5> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  CHECK(list->Close());
  ID3D12CommandList *lists[] = {list.Get()};
  queue->ExecuteCommandLists(1, lists);
  printf("fact execute %#llx\n", (unsigned long long)(uintptr_t)queue.Get());
  UINT64 signals = mode == "many" ? strtoull(getenv("FLIGHT_SIGNALS"), nullptr, 10) : 1;
  for (UINT64 value = 1; value <= signals; value++)
    CHECK(queue->Signal(fence.Get(), value));
  CHECK(queue->Wait(fence.Get(), signals));
  HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  CHECK(fence->SetEventOnCompletion(signals, event));
  WaitForSingleObject(event, INFINITE);
  CloseHandle(event);
  // pointers as the dump prints them
  printf("fact signal %#llx %#llx %llu\n", (unsigned long long)(uintptr_t)queue.Get(),
         (unsigned long long)(uintptr_t)fence.Get(), (unsigned long long)signals);
  if (mode != "quiet") {
    device->RemoveDevice();
    printf("fact removed\n");
  }
  return verdict();
}
