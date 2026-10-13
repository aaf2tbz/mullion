// contract: sampling reports whether the texels that contribute were mapped: "These 0 weight texels do not
// contribute to the filter result at all, so if they happen to fall on NULL tiles they don't count as an unmapped
// access" (D3D11.3 5.9.4.5.3). CheckAccessFullyMapped "returns true if all data being accessed was mapped in the
// resource; otherwise, CheckAccessFullyMapped returns false" (5.9.4.6); point sampling selects the nearest mip,
// clamped to the view (7.18.10, 7.18.11). SampleGrad and SampleLevel therefore give the same texels and residency
// for these footprints, including a gradient larger than the entire texture with its last mip mapped.
// both mips fill standard tiles: "Mipmaps that fill at least one standard tile in all dimensions are guaranteed
// to use the standard tiling" (D3D11.3 5.9.7.2).
// remapping precedes sampling: "tile mapping updates specified by this call must be completed before any
// subsequent D3D command can proceed" without NO_OVERWRITE (D3D11.3 5.9.3.1).
// the last phase leaves no tile mapped, and every read is of a NULL tile: "Reading from NULL-mapped tiles treat that
// sampled value as zero" (D3D12_TILED_RESOURCES_TIER_2).
// zero and nonzero texels share every tile; their values cannot stand in for its mapping status. R8G8B8A8_UNORM's
// n / (2^8 - 1) conversion is undone before comparing bytes (D3D11.3 3.2.3.5).
#include "d3d12_test.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

static const char hlsl[] = R"hlsl(
Texture2D<float4> tex : register(t0);
SamplerState s : register(s0);
cbuffer Args : register(b0) { float2 extent; float lod; float least_lod; uint parallel; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint2 ps_grad(float4 pos : SV_Position) : SV_Target {
  float2 dx = float2(exp2(lod) / extent.x, 0), dy = float2(0, exp2(lod) / extent.y);
  if (parallel) dx = dy = float2(1, 1);
  uint status;
  float4 v = tex.SampleGrad(s, pos.xy / extent, dx, dy, int2(0, 0), least_lod, status);
  return uint2(round(v.x * TEXEL_MAX), CheckAccessFullyMapped(status));
}
uint2 ps_level(float4 pos : SV_Position) : SV_Target {
  uint status;
  float4 v = tex.SampleLevel(s, pos.xy / extent, max(lod, least_lod), int2(0, 0), status);
  return uint2(round(v.x * TEXEL_MAX), CheckAccessFullyMapped(status));
}
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  const std::vector<std::string> defines{"TEXEL_MAX=" + std::to_string(UINT8_MAX)};
  auto vs = compiler.compile(hlsl, "vs", "vs", defines);
  const std::string ps[] = {compiler.compile(hlsl, "ps_grad", "ps", defines),
                            compiler.compile(hlsl, "ps_level", "ps", defines)};
  if (vs.empty() || ps[0].empty() || ps[1].empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options)));
  if (options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2) {
    printf("skipped: tiled resources tier 2 unavailable\n");
    return 77;
  }

  const UINT tile_texels = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES / sizeof(uint8_t[4]);
  D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, tile_texels, tile_texels, 1, 1,
                           DXGI_FORMAT_R8G8B8A8_UNORM,
                           {1, 0}, D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS};
  ComPtr<ID3D12Resource> texture;
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
  D3D12_TILE_SHAPE shape{};
  device->GetResourceTiling(texture.Get(), nullptr, nullptr, &shape, nullptr, 0, nullptr);
  if (!expect(shape.WidthInTexels && shape.HeightInTexels, "empty tile shape"))
    return verdict();
  desc.Width = 2 * shape.WidthInTexels, desc.Height = 2 * shape.HeightInTexels, desc.MipLevels = 2;
  CHECK(device->CreateReservedResource(&desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texture)));
  const UINT width = desc.Width, height = desc.Height;
  UINT total = 0;
  D3D12_PACKED_MIP_INFO packed{};
  device->GetResourceTiling(texture.Get(), &total, &packed, nullptr, nullptr, 0, nullptr);
  const UINT tiles_wide = width / shape.WidthInTexels;
  const UINT first_mip_tiles = tiles_wide * (height / shape.HeightInTexels);
  if (!expect(packed.NumStandardMips == desc.MipLevels && total == first_mip_tiles + 1,
              "the two standard mips do not have their expected tiles"))
    return verdict();
  ComPtr<ID3D12Heap> heap;
  D3D12_HEAP_DESC heap_desc{UINT64(total) * D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES, {D3D12_HEAP_TYPE_DEFAULT}};
  CHECK(device->CreateHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
  D3D12_TILED_RESOURCE_COORDINATE origin{};
  D3D12_TILE_REGION_SIZE all{total};
  queue->UpdateTileMappings(texture.Get(), 1, &origin, &all, heap.Get(), 1, nullptr, nullptr, nullptr,
                            D3D12_TILE_MAPPING_FLAG_NONE);

  D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed[2];
  UINT64 bytes;
  device->GetCopyableFootprints(&desc, 0, desc.MipLevels, 0, placed, nullptr, nullptr, &bytes);
  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  char *staged;
  CHECK(upload->Map(0, nullptr, (void **)&staged));
  for (UINT mip = 0; mip < desc.MipLevels; mip++) {
    for (UINT y = 0; y < height >> mip; y++)
      for (UINT x = 0; x < width >> mip; x++) {
        const uint8_t value[4] = {uint8_t(((x + y) & 1) ? mip + 1 : 0), 0, 0, 0};
        memcpy(staged + placed[mip].Offset + y * placed[mip].Footprint.RowPitch + x * sizeof(value), value,
               sizeof(value));
      }
    D3D12_TEXTURE_COPY_LOCATION to{texture.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = mip}},
        from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = placed[mip]}};
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  }
  upload->Unmap(0, nullptr);
  transition(list.Get(), texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
  CHECK(submit(device.Get(), queue.Get(), list.Get()));

  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1},
      sampler_range{D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE},
                                    {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
                                    {D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}};
  params[0].DescriptorTable = {1, &range};
  struct Args { float extent[2], lod, least_lod; UINT parallel; };
  params[1].Constants = {0, 0, sizeof(Args) / sizeof(UINT)};
  params[2].DescriptorTable = {1, &sampler_range};
  D3D12_SAMPLER_DESC sampler{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                             D3D12_TEXTURE_ADDRESS_MODE_CLAMP, D3D12_TEXTURE_ADDRESS_MODE_CLAMP};
  sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER, sampler.MaxAnisotropy = 1;
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params});
  if (!expect(bool(rs), "root signature creation failed"))
    return verdict();
  ComPtr<ID3D12DescriptorHeap> views, rtvs, samplers;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE},
      rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  views_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&samplers)));
  device->CreateShaderResourceView(texture.Get(), nullptr, views->GetCPUDescriptorHandleForHeapStart());
  desc.MipLevels = 1, desc.Format = DXGI_FORMAT_R32G32_UINT, desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const D3D12_HEAP_PROPERTIES props{D3D12_HEAP_TYPE_DEFAULT};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT result;
  device->GetCopyableFootprints(&desc, 0, 1, 0, &result, nullptr, nullptr, &bytes);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12PipelineState> pipelines[std::size(ps)];
  for (UINT way = 0; way < std::size(ps); way++) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{rs.Get(), bytecode(vs), bytecode(ps[way])};
    pipeline.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pipeline.SampleMask = ~0u;
    pipeline.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    pipeline.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pipeline.NumRenderTargets = 1, pipeline.RTVFormats[0] = desc.Format, pipeline.SampleDesc = {1, 0};
    CHECK(device->CreateGraphicsPipelineState(&pipeline, IID_PPV_ARGS(&pipelines[way])));
  }
  const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
  const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
  const float large = std::log2(float(std::max(width, height)));
  const Args cases[] = {{{float(width), float(height)}, 0, 0, 0},
                        {{float(width), float(height)}, -1, 0, 0},
                        {{float(width), float(height)}, 0.25f, 0, 0},
                        {{float(width), float(height)}, 0.75f, 0, 0},
                        {{float(width), float(height)}, 1, 0, 0},
                        {{float(width), float(height)}, large, 0, 0},
                        {{float(width), float(height)}, large, 0, 1},
                        {{float(width), float(height)}, 0, 1, 0}};
  for (UINT phase = 0; phase < 3; phase++) {
    sampler.MinLOD = phase ? -D3D12_FLOAT32_MAX : -2 * large;
    sampler.MaxLOD = phase ? D3D12_FLOAT32_MAX : 2 * large;
    device->CreateSampler(&sampler, samplers->GetCPUDescriptorHandleForHeapStart());
    for (UINT tile = 0; tile < total; tile++) {
      const UINT mip = tile == first_mip_tiles, x = mip ? 0 : tile % tiles_wide, y = mip ? 0 : tile / tiles_wide;
      const bool mapped = phase < 2 && (mip ? !phase : ((x == y) != bool(phase)));
      D3D12_TILED_RESOURCE_COORDINATE coord{x, y, 0, mip};
      D3D12_TILE_RANGE_FLAGS flag = mapped ? D3D12_TILE_RANGE_FLAG_NONE : D3D12_TILE_RANGE_FLAG_NULL;
      queue->UpdateTileMappings(texture.Get(), 1, &coord, nullptr, heap.Get(), 1, &flag, &tile, nullptr,
                                D3D12_TILE_MAPPING_FLAG_NONE);
    }
    for (UINT variant = 0; variant < std::size(cases); variant++)
      for (UINT way = 0; way < std::size(ps); way++) {
        step("phase %u, variant %u, %s, lod %g, clamp %g, parallel %u", phase, variant,
             way ? "SampleLevel" : "SampleGrad", cases[variant].lod, cases[variant].least_lod, cases[variant].parallel);
        CHECK(forget(readback.Get()));
        CHECK(allocator->Reset());
        CHECK(list->Reset(allocator.Get(), pipelines[way].Get()));
        ID3D12DescriptorHeap *bound[] = {views.Get(), samplers.Get()};
        list->SetDescriptorHeaps(std::size(bound), bound);
        list->SetGraphicsRootSignature(rs.Get());
        list->SetGraphicsRootDescriptorTable(0, views->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRootDescriptorTable(2, samplers->GetGPUDescriptorHandleForHeapStart());
        list->SetGraphicsRoot32BitConstants(1, sizeof(Args) / sizeof(UINT), &cases[variant], 0);
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissor);
        list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        list->DrawInstanced(3, 1, 0, 0);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
            to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = result}};
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        CHECK(submit(device.Get(), queue.Get(), list.Get()));
        const char *got;
        CHECK(readback->Map(0, nullptr, (void **)&got));
        const UINT mip = std::max(cases[variant].lod, cases[variant].least_lod) >= 0.5f;
        UINT wrong = 0;
        for (UINT y = 0; y < height; y++)
          for (UINT x = 0; x < width; x++) {
            const bool mapped =
                phase < 2 && (mip ? !phase : ((x / shape.WidthInTexels == y / shape.HeightInTexels) != bool(phase)));
            const UINT wanted = mapped && (((x >> mip) + (y >> mip)) & 1) ? mip + 1 : 0;
            const UINT *value = (const UINT *)(got + result.Offset + y * result.Footprint.RowPitch) + 2 * x;
            if ((value[0] != wanted || value[1] != UINT(mapped)) && wrong++ < 3)
              expect(false, "pixel %u,%u: value %u, residency %u; want %u, %u", x, y, value[0], value[1],
                     wanted, UINT(mapped));
          }
        expect(!wrong, "%u pixels differ", wrong);
        readback->Unmap(0, nullptr);
      }
  }
  return verdict();
}
