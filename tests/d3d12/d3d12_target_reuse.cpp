// contract: a committed render target or depth buffer made after another of the same description was released is a
// texture of its own: its views name its own subresources, and what a clear writes there is what a copy reads back,
// whatever views the released one had. "Render Target and Depth Stencil ... resources must be initialized" by a clear,
// a discard or a copy before other use (ID3D12GraphicsCommandList::DiscardResource), so only what was written after
// that is read; a committed texture that is neither a target nor a depth buffer starts zeroed, as committed resources
// do unless made with D3D12_HEAP_FLAG_CREATE_NOT_ZEROED. each round makes the texture again, makes its views in another
// order than the round before (a view of the last round's subresource order would clear the wrong one) and checks
// every subresource; RGBA8 targets, typeless ones with sRGB views and D32 depth buffers, with mips and array layers;
// a texture made with CREATE_NOT_ZEROED then one without, of one description, and the second reads zero. the
// textures are of more than a MiB each.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ne-d3d12-d3d12_heap_flags
#include "d3d12_test.hpp"
#include <d3d12.h>
#include <vector>

static constexpr UINT width = 512, height = 256, mips = 3, layers = 2, subs = mips * layers, rounds = 6;

int
main() {
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, subs};
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV, subs};
  ComPtr<ID3D12DescriptorHeap> rtvs, dsvs;
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  CHECK(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsvs)));
  const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const UINT dsv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

  // each subresource's value in a round, as four bytes or a depth
  auto value = [](UINT round, UINT sub) { return 0xff000000u | (round * 37 + 11) << 8 | (sub * 29 + 5); };
  auto depth = [](UINT round, UINT sub) { return float(round * subs + sub + 1) / float(rounds * subs + 1); };

  for (UINT kind = 0; kind < 4; kind++)
    for (UINT round = 0; round < rounds; round++) {
      step("kind %u round %u", kind, round);
      const bool is_depth = kind == 2, plain = kind == 3;
      const DXGI_FORMAT format = kind == 1 ? DXGI_FORMAT_R8G8B8A8_TYPELESS
                                 : is_depth ? DXGI_FORMAT_D32_FLOAT
                                            : DXGI_FORMAT_R8G8B8A8_UNORM;
      D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, width, height, layers, mips, format, {1, 0},
                               D3D12_TEXTURE_LAYOUT_UNKNOWN,
                               plain      ? D3D12_RESOURCE_FLAG_NONE
                               : is_depth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                          : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
      D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      const D3D12_RESOURCE_STATES state = plain      ? D3D12_RESOURCE_STATE_COPY_SOURCE
                                          : is_depth ? D3D12_RESOURCE_STATE_DEPTH_WRITE
                                                     : D3D12_RESOURCE_STATE_RENDER_TARGET;
      ComPtr<ID3D12Resource> texture;
      // a plain texture's odd rounds start undefined, and are not read
      const bool undefined = plain && round % 2;
      CHECK(device->CreateCommittedResource(&heap, undefined ? D3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE,
                                            &desc, state, nullptr, IID_PPV_ARGS(&texture)));
      D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[subs];
      UINT64 total = 0;
      device->GetCopyableFootprints(&desc, 0, subs, 0, footprints, nullptr, nullptr, &total);
      auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
      auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
      CHECK(forget(readback.Get()));
      CHECK(allocator->Reset());
      CHECK(list->Reset(allocator.Get(), nullptr));
      // the views, in an order that turns with the round
      for (UINT k = 0; k < subs; k++) {
        const UINT sub = (k + round) % subs, mip = sub % mips, layer = sub / mips;
        if (plain)
          break;
        if (is_depth) {
          D3D12_DEPTH_STENCIL_VIEW_DESC view{DXGI_FORMAT_D32_FLOAT, D3D12_DSV_DIMENSION_TEXTURE2DARRAY};
          view.Texture2DArray = {mip, layer, 1};
          D3D12_CPU_DESCRIPTOR_HANDLE at{dsvs->GetCPUDescriptorHandleForHeapStart().ptr + k * dsv_step};
          device->CreateDepthStencilView(texture.Get(), &view, at);
          list->ClearDepthStencilView(at, D3D12_CLEAR_FLAG_DEPTH, depth(round, sub), 0, 0, nullptr);
        } else {
          D3D12_RENDER_TARGET_VIEW_DESC view{kind == 1 ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,
                                             D3D12_RTV_DIMENSION_TEXTURE2DARRAY};
          view.Texture2DArray = {mip, layer, 1, 0};
          D3D12_CPU_DESCRIPTOR_HANDLE at{rtvs->GetCPUDescriptorHandleForHeapStart().ptr + k * rtv_step};
          device->CreateRenderTargetView(texture.Get(), &view, at);
          // an sRGB view's clear is encoded: the values are those that encode to the bytes wanted, 0 and 1
          // exactly, so a typeless round clears its subresources to black or white by the parity of the value
          const uint32_t v = value(round, sub);
          float color[4];
          for (int c = 0; c < 4; c++)
            color[c] = kind == 1 ? float((v >> (8 * c) & 1) | (c == 3)) : float(v >> (8 * c) & 0xff) / 255.0f;
          list->ClearRenderTargetView(at, color, 0, nullptr);
        }
      }
      if (!plain)
        transition(list.Get(), texture.Get(), state, D3D12_RESOURCE_STATE_COPY_SOURCE);
      for (UINT sub = 0; sub < subs; sub++) {
        D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        dst.PlacedFootprint = footprints[sub];
        D3D12_TEXTURE_COPY_LOCATION src{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        src.SubresourceIndex = sub;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      }
      if (plain) {
        // the plain texture is given data after it is read, for the next round's to start zeroed all the same
        transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        void *bytes;
        CHECK(upload->Map(0, nullptr, &bytes));
        memset(bytes, 0x5a, total);
        upload->Unmap(0, nullptr);
        for (UINT sub = 0; sub < subs; sub++) {
          D3D12_TEXTURE_COPY_LOCATION src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
          src.PlacedFootprint = footprints[sub];
          D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
          dst.SubresourceIndex = sub;
          list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
      }
      CHECK(submit(device.Get(), queue.Get(), list.Get()));
      const uint8_t *bytes;
      CHECK(readback->Map(0, nullptr, (void **)&bytes));
      bool ok = true;
      for (UINT sub = 0; sub < subs && ok && !undefined; sub++) {
        const auto &fp = footprints[sub];
        for (UINT y = 0; y < fp.Footprint.Height && ok; y++)
          for (UINT x = 0; x < fp.Footprint.Width && ok; x++) {
            uint32_t got;
            memcpy(&got, bytes + fp.Offset + y * fp.Footprint.RowPitch + x * 4, 4);
            uint32_t want = value(round, sub);
            if (plain)
              want = 0;
            else if (kind == 1) {
              uint32_t bits = want;
              want = 0;
              for (int c = 0; c < 4; c++)
                want |= uint32_t((bits >> (8 * c) & 1) | (c == 3) ? 0xff : 0) << (8 * c);
            } else if (is_depth) {
              float d = depth(round, sub);
              memcpy(&want, &d, 4);
            }
            ok = expect(got == want, "subresource %u texel (%u,%u) is %08x, want %08x", sub, x, y, got, want);
          }
      }
      readback->Unmap(0, nullptr);
      if (!ok)
        return verdict();
    }
  return verdict();
}
