// contract: "All methods for creating descriptors are free-threaded" (Microsoft Learn, Creating descriptors), and each
// view reads the subresource its description names.
// eight threads make views of one texture at once, each of its own mip and slice, into descriptors of their own; a
// dispatch then reads every descriptor, and each must give its subresource's value. a texture whose views two
// threads add at once must neither lose nor repeat one, which would give later descriptors another subresource's view.
// many fresh textures, as the window is small.
// https://learn.microsoft.com/en-us/windows/win32/direct3d12/creating-descriptors
#include "d3d12_test.hpp"
#include <thread>

static const char hlsl[] = R"(
Texture2DArray<uint> views[64] : register(t0);
RWStructuredBuffer<uint> read : register(u0);
[numthreads(64, 1, 1)] void cs(uint i : SV_DispatchThreadID) {
  read[i] = views[NonUniformResourceIndex(i)].Load(int4(0, 0, 0, 0));
}
)";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler))
    return 77;
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  CHECK(list->Close());
  auto code = compiler.compile(hlsl, "cs", "cs");
  if (!expect(!code.empty(), "the shader does not compile"))
    return verdict();
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 64, 0};
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  parameters[0].DescriptorTable = {1, &range};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[1].Descriptor = {0, 0};
  auto root = root_signature(device.Get(), {2, parameters});
  CHECK(root ? S_OK : E_FAIL);
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{root.Get(), bytecode(code)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  constexpr UINT mips = 8, slices = 8, threads = 8, textures = 48;
  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, mips * slices,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, 1u << (mips - 1), 1u << (mips - 1), slices, mips,
                           DXGI_FORMAT_R32_UINT, {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_NONE};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[mips * slices];
  UINT64 total;
  device->GetCopyableFootprints(&desc, 0, mips * slices, 0, footprints, nullptr, nullptr, &total);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto read = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, mips * slices * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, mips * slices * 4, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(upload && read && readback, "the buffers are refused"))
    return verdict();
  // every texel of a subresource holds the subresource's own value
  auto value = [](UINT mip, UINT slice) { return 0xa0000000u | mip << 8 | slice; };
  BYTE *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  for (UINT slice = 0; slice < slices; slice++)
    for (UINT mip = 0; mip < mips; mip++) {
      auto &footprint = footprints[slice * mips + mip];
      for (UINT y = 0; y < footprint.Footprint.Height; y++)
        for (UINT x = 0; x < footprint.Footprint.Width; x++) {
          UINT texel = value(mip, slice);
          memcpy(mapped + footprint.Offset + y * footprint.Footprint.RowPitch + x * 4, &texel, 4);
        }
    }
  upload->Unmap(0, nullptr);

  for (UINT round = 0; round < textures; round++) {
    step("texture %u", round);
    D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
    ComPtr<ID3D12Resource> texture;
    CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                          IID_PPV_ARGS(&texture)));
    // each thread makes the views of one slice, every mip, into its own descriptors, all at once
    std::vector<std::thread> makers;
    for (UINT t = 0; t < threads; t++)
      makers.emplace_back([&, slice = t] {
        for (UINT mip = 0; mip < mips; mip++) {
          D3D12_SHADER_RESOURCE_VIEW_DESC view{DXGI_FORMAT_R32_UINT, D3D12_SRV_DIMENSION_TEXTURE2DARRAY,
                                               D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
          view.Texture2DArray = {mip, 1, slice, 1, 0, 0};
          auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
          cpu.ptr += (slice * mips + mip) * increment;
          device->CreateShaderResourceView(texture.Get(), &view, cpu);
        }
      });
    for (auto &maker : makers)
      maker.join();
    CHECK(forget(readback.Get()));
    CHECK(allocator->Reset());
    CHECK(list->Reset(allocator.Get(), pso.Get()));
    for (UINT i = 0; i < mips * slices; i++) {
      D3D12_TEXTURE_COPY_LOCATION dst{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
          src{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
      dst.SubresourceIndex = i;
      src.PlacedFootprint = footprints[i];
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(root.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    list->SetComputeRootUnorderedAccessView(1, read->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), read.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(readback.Get(), read.Get());
    transition(list.Get(), read.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    for (UINT slice = 0; slice < slices; slice++)
      for (UINT mip = 0; mip < mips; mip++)
        expect(got[slice * mips + mip] == value(mip, slice), "the view of mip %u, slice %u reads %08x, want %08x", mip,
               slice, got[slice * mips + mip], value(mip, slice));
    readback->Unmap(0, nullptr);
  }

  return verdict();
}
