// contract: pipeline statistics count application draws and dispatches between BeginQuery and EndQuery, including
// overlapping ranges, and resolving a range writes one D3D12_QUERY_DATA_PIPELINE_STATISTICS per query.
// "only the difference between two independant statistic requests will provide meaningful information";
// "Number of individual Compute Shader threads invoked" (D3D11.3 20.4.7). A triangle list without indexing has
// "valid VSInvocations only 12" for four triangles (20.4.7): each submitted vertex runs once. Each quad has two
// triangles wholly within the clip volume: "1 primitive will be generated if the original triangle is not clipped
// at all" (20.4.7). Its pixels cover the target on every draw.
// sources: https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// https://learn.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_query_data_pipeline_statistics
// clears and copies are implementation work, excluded by 20.1. A seed varies draw and dispatch sizes; replay changes
// the indirect dispatch and its input, so counts or output left by an earlier execution cannot stand in for new work.
// "CSInvocations must honor Compute Shader invocations from both Dispatch() and DispatchIndirect() APIs."
// (20.4.7). The two dispatches write separate regions; an empty dispatch writes neither and counts no threads.
// "when a NULL pixel shader is bound to the pipeline, PSInvocations does not increment" (20.4.7).
// "The value of PSInvocations may include or exclude helper pixels for 2x2 stamps" (20.4.7). Each triangle
// can invoke at most all four lanes of every stamp touching the target, so two such stamp grids bound a quad.
#include "d3d12_test.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <random>

static const char hlsl[] = R"hlsl(
float4 vs(uint id : SV_VertexID) : SV_Position {
  const float2 corners[] = {
    float2(-1, 1), float2(1, 1), float2(-1, -1),
    float2(-1, -1), float2(1, 1), float2(1, -1)
  };
  return float4(corners[id % VERTICES], 0.5, 1);
}
uint ps() : SV_Target { return 1; }
StructuredBuffer<uint> values : register(t0);
RWStructuredBuffer<uint> output : register(u0);
[numthreads(TX, TY, TZ)]
void cs(uint3 id : SV_DispatchThreadID) {
  output[id.x + WIDTH * (id.y + HEIGHT * id.z)] = values[0];
}
)hlsl";

int
main(int argc, char **argv) {
  const UINT seed = argc > 2 ? strtoul(argv[2], nullptr, 0) : 0;
  std::minstd_rand next(seed);
  const UINT width = 11, height = 7, replays = 3, vertices_per_triangle = 3, triangles_per_quad = 2,
             vertices_per_quad = vertices_per_triangle * triangles_per_quad;
  const std::array<UINT, 3> threads{2, 3, 2}, flat_threads{threads[0], threads[1], 1},
      groups{UINT(1 + next() % 3), UINT(1 + next() % 2), UINT(1 + next() % 2)};
  const UINT cs_width = threads[0] * (groups[0] + replays - 1), cs_height = threads[1] * groups[1];
  const UINT64 threads_per_group = UINT64(threads[0]) * threads[1] * threads[2],
               direct_invocations = UINT64(flat_threads[0]) * flat_threads[1] * flat_threads[2] *
                                    groups[0] * groups[1] * groups[2],
               plane_words = UINT64(cs_width) * cs_height * threads[2] * groups[2], output_words = 2 * plane_words;
  const UINT instances = 1 + next() % 3, first = 1 + next() % 3, nested = 1 + next() % 3, discarded = 1 + next() % 3,
             no_pixel = 1 + next() % 3;
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  std::vector<std::string> defines{
      "TX=" + std::to_string(threads[0]), "TY=" + std::to_string(threads[1]),
      "WIDTH=" + std::to_string(cs_width), "HEIGHT=" + std::to_string(cs_height),
      "VERTICES=" + std::to_string(vertices_per_quad), "TZ=" + std::to_string(threads[2])};
  step("seed %u, %s: compile VS, PS and CS", seed, compiler.dxc ? "dxil" : "dxbc");
  auto vs = compiler.compile(hlsl, "vs", "vs", defines), ps = compiler.compile(hlsl, "ps", "ps", defines),
       cs = compiler.compile(hlsl, "cs", "cs", defines);
  defines.back() = "TZ=" + std::to_string(flat_threads[2]);
  auto cs_flat = compiler.compile(hlsl, "cs", "cs", defines);
  if (vs.empty() || ps.empty() || cs.empty() || cs_flat.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  enum { Empty, All, First, Nested, Reused, Compute, NoPixel, Queries };
  step("seed %u: create a pipeline statistics heap", seed);
  D3D12_QUERY_HEAP_DESC query_desc{D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, Queries};
  ComPtr<ID3D12QueryHeap> queries;
  const HRESULT created = device->CreateQueryHeap(&query_desc, IID_PPV_ARGS(&queries));
  if (!expect(created == S_OK && queries, "CreateQueryHeap returned %#lx", created))
    return verdict();
  const UINT64 result_stride = sizeof(D3D12_QUERY_DATA_PIPELINE_STATISTICS),
               result_bytes = (Queries + 2) * result_stride;
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, result_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  auto source = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, (1 + output_words) * sizeof(UINT),
                       D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, output_words * sizeof(UINT),
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto words =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, output_words * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(results && source && output && words, "buffers could not be made"))
    return verdict();

  auto graphics_rs = root_signature(device.Get(), {});
  D3D12_ROOT_PARAMETER parameters[2]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  D3D12_ROOT_SIGNATURE_DESC root_desc{UINT(std::size(parameters)), parameters};
  auto compute_rs = root_signature(device.Get(), root_desc);
  if (!expect(graphics_rs && compute_rs, "root signatures could not be made"))
    return verdict();
  D3D12_GRAPHICS_PIPELINE_STATE_DESC graphics{graphics_rs.Get(), bytecode(vs), bytecode(ps)};
  graphics.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  graphics.SampleMask = ~0u;
  graphics.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  graphics.RasterizerState.DepthClipEnable = TRUE;
  graphics.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  graphics.NumRenderTargets = 1;
  graphics.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  graphics.SampleDesc = {1, 0};
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute{compute_rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> draw_pso, dispatch_pso, flat_dispatch_pso, no_pixel_pso;
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&draw_pso)));
  graphics.PS = {};
  CHECK(device->CreateGraphicsPipelineState(&graphics, IID_PPV_ARGS(&no_pixel_pso)));
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&dispatch_pso)));
  compute.CS = bytecode(cs_flat);
  CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&flat_dispatch_pso)));
  D3D12_INDIRECT_ARGUMENT_DESC argument{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(D3D12_DISPATCH_ARGUMENTS), 1, &argument};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));
  auto arguments =
      buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, signature_desc.ByteStride, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(bool(arguments), "indirect arguments could not be made"))
    return verdict();
  D3D12_DISPATCH_ARGUMENTS *dispatch_arguments;
  CHECK(arguments->Map(0, nullptr, (void **)&dispatch_arguments));
  *dispatch_arguments = {groups[0], groups[1], groups[2]};
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                              0,
                              width,
                              height,
                              1,
                              1,
                              DXGI_FORMAT_R32_UINT,
                              {1, 0},
                              D3D12_TEXTURE_LAYOUT_UNKNOWN,
                              D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  ComPtr<ID3D12DescriptorHeap> rtvs;
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 pixel_bytes;
  device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &pixel_bytes);
  auto pixels = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, pixel_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  if (!expect(bool(pixels), "pixel readback could not be made"))
    return verdict();
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), draw_pso.Get(),
                                  IID_PPV_ARGS(&list)));
  D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
  D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
  list->SetGraphicsRootSignature(graphics_rs.Get());
  list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
  list->RSSetViewports(1, &viewport);
  list->RSSetScissorRects(1, &scissor);
  list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  auto begin = [&](UINT index) { list->BeginQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, index); };
  auto end = [&](UINT index) { list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, index); };
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyBufferRegion(output.Get(), 0, source.Get(), sizeof(UINT), output_words * sizeof(UINT));
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  step("seed %u: empty, overlapping, reused and compute ranges", seed);
  begin(Empty);
  const float black[4]{};
  list->ClearRenderTargetView(rtv, black, 0, nullptr);
  end(Empty);
  begin(All);
  begin(First);
  list->DrawInstanced(vertices_per_quad * first, instances, 0, 0);
  begin(Nested);
  list->DrawInstanced(vertices_per_quad * nested, instances, 0, 0);
  end(Nested);
  end(First);
  begin(Reused);
  list->DrawInstanced(vertices_per_quad * discarded, instances, 0, 0);
  end(Reused);
  begin(Reused);
  end(Reused);
  begin(Compute);
  list->SetPipelineState(flat_dispatch_pso.Get());
  list->SetComputeRootSignature(compute_rs.Get());
  list->SetComputeRootShaderResourceView(0, source->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress());
  list->Dispatch(groups[0], groups[1], groups[2]);
  list->SetComputeRootUnorderedAccessView(1, output->GetGPUVirtualAddress() + plane_words * sizeof(UINT));
  list->SetPipelineState(dispatch_pso.Get());
  list->Dispatch(0, groups[1], groups[2]);
  list->ExecuteIndirect(signature.Get(), 1, arguments.Get(), 0, nullptr, 0);
  end(Compute);
  end(All);
  begin(NoPixel);
  list->SetPipelineState(no_pixel_pso.Get());
  list->DrawInstanced(vertices_per_quad * no_pixel, instances, 0, 0);
  end(NoPixel);
  list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, First, results.Get(), result_stride);
  list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_PIPELINE_STATISTICS, First, Queries - First, results.Get(),
                         (First + 1) * result_stride);
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(words.Get(), 0, output.Get(), 0, output_words * sizeof(UINT));
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX},
      to{pixels.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
  to.PlacedFootprint = footprint;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  CHECK(list->Close());

  UINT64 quads[Queries]{};
  quads[All] = UINT64(first + nested + discarded) * instances;
  quads[First] = UINT64(first + nested) * instances;
  quads[Nested] = UINT64(nested) * instances;
  quads[NoPixel] = UINT64(no_pixel) * instances;
  UINT *input;
  CHECK(source->Map(0, nullptr, (void **)&input));
  memset(input + 1, 0, output_words * sizeof(UINT));
  for (UINT replay = 0; replay < replays; replay++) {
    step("seed %u, replay %u: counts, resolve guards, pixels and compute writes", seed, replay);
    const UINT indirect_groups_x = groups[0] + replay;
    const UINT64 indirect_invocations = threads_per_group * indirect_groups_x * groups[1] * groups[2];
    *dispatch_arguments = {indirect_groups_x, groups[1], groups[2]};
    *input = next();
    CHECK(forget(results.Get()));
    CHECK(forget(words.Get()));
    CHECK(forget(pixels.Get()));
    CHECK(execute(device.Get(), queue.Get(), list.Get()));
    const unsigned char *bytes;
    CHECK(results->Map(0, nullptr, (void **)&bytes));
    auto guard = [](auto byte) { return byte == 0xff; };
    expect(std::all_of(bytes, bytes + result_stride, guard) &&
               std::all_of(bytes + (Queries + 1) * result_stride, bytes + result_bytes, guard),
           "resolve wrote beyond its region");
    for (UINT q = 0; q < Queries; q++) {
      const UINT64 primitives = triangles_per_quad * quads[q];
      D3D12_QUERY_DATA_PIPELINE_STATISTICS got;
      memcpy(&got, bytes + (q + 1) * result_stride, sizeof(got));
      expect(got.IAVertices == vertices_per_triangle * primitives, "query %u: IA vertices %llu, want %llu", q,
             got.IAVertices, vertices_per_triangle * primitives);
      expect(got.IAPrimitives == primitives, "query %u: IA primitives %llu, want %llu", q, got.IAPrimitives,
             primitives);
      expect(got.VSInvocations == vertices_per_triangle * primitives, "query %u: VS invocations %llu, want %llu", q,
             got.VSInvocations, vertices_per_triangle * primitives);
      expect(got.CInvocations == primitives && got.CPrimitives == primitives,
             "query %u: clip invocations %llu, primitives %llu, want %llu", q, got.CInvocations, got.CPrimitives,
             primitives);
      const UINT64 shaded_quads = q == NoPixel ? 0 : quads[q];
      const UINT stamp_width = 2, stamp_height = 2;
      const UINT64 minimum_pixels = UINT64(width) * height * shaded_quads,
                   maximum_pixels = UINT64((width + stamp_width - 1) / stamp_width) *
                                    ((height + stamp_height - 1) / stamp_height) * stamp_width * stamp_height *
                                    triangles_per_quad * shaded_quads;
      expect(got.PSInvocations >= minimum_pixels && got.PSInvocations <= maximum_pixels,
             "query %u: PS count %llu, want [%llu, %llu]", q, got.PSInvocations, minimum_pixels, maximum_pixels);
      expect(!got.GSPrimitives && !got.HSInvocations && !got.DSInvocations && (quads[q] || !got.GSInvocations),
             "query %u: unused stage count", q);
      const UINT64 compute_invocations = q == All || q == Compute ? direct_invocations + indirect_invocations : 0;
      expect(got.CSInvocations == compute_invocations, "query %u: CS invocations %llu, want %llu", q, got.CSInvocations,
             compute_invocations);
    }
    results->Unmap(0, nullptr);
    const UINT *written;
    CHECK(words->Map(0, nullptr, (void **)&written));
    for (UINT64 i = 0; i < output_words; i++) {
      const UINT x = i % cs_width, dispatched_width = threads[0] * (i < plane_words ? groups[0] : indirect_groups_x);
      const UINT64 z = (i % plane_words) / (UINT64(cs_width) * cs_height),
                   dispatched_depth = UINT64(i < plane_words ? flat_threads[2] : threads[2]) * groups[2];
      const UINT want = x < dispatched_width && z < dispatched_depth ? *input : 0;
      if (!expect(written[i] == want, "compute word %llu: %u, want %u", i, written[i], want))
        break;
    }
    words->Unmap(0, nullptr);
    CHECK(pixels->Map(0, nullptr, (void **)&bytes));
    for (UINT y = 0; y < height; y++) {
      auto row = reinterpret_cast<const UINT *>(bytes + footprint.Offset + y * footprint.Footprint.RowPitch);
      expect(std::all_of(row, row + width, [](UINT value) { return value == 1; }), "row %u was not drawn", y);
    }
    pixels->Unmap(0, nullptr);
  }
  source->Unmap(0, nullptr);
  arguments->Unmap(0, nullptr);
  return verdict();
}
