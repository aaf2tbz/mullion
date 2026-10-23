// contract: the operations shader model 6.7 requires, and the device reporting it. QuadAny and QuadAll vote over a
// quad's active lanes: a compute wave gives each quad a pattern of its own, and a branch then leaves one lane of each
// quad out, which the vote must not count. a pixel shader with [WaveOpsIncludeHelperLanes] runs one live lane per
// quad, whose three helpers then count in its wave operations and vote in its quad's; without it they count in the
// quad's operations only.
// HLSL Shader Model 6.7, "QuadAny and QuadAll": "compare boolean across a quad" (DXIL.rst QuadVote, opcode 222:
// QuadVoteOpKind 0 = Any, 1 = All), and "Helper Lanes in Wave Ops Mode": with the attribute, "helper lanes are
// included in wave operations".
// https://microsoft.github.io/DirectX-Specs/d3d/HLSL_ShaderModel6_7.html
#include "d3d12_test.hpp"
#include <map>

static const char hlsl[] = R"hlsl(
RWStructuredBuffer<uint> o : register(u0);
// each quad's conditions are a 4-bit pattern of its own, lane i of the quad taking bit i
bool condition(uint lane) { return ((lane / 4 * 5 + 3) % 16 >> (lane % 4)) & 1; }
[numthreads(LANES, 1, 1)]
void cs(uint t : SV_GroupIndex) {
  uint lane = WaveGetLaneIndex();
  bool c = condition(lane);
  o[t * 4 + 0] = lane;
  o[t * 4 + 1] = QuadAny(c) | QuadAll(c) << 1;
  // the quad's last lane sits out
  if (lane % 4 != 3)
    o[t * 4 + 2] = QuadAny(c) | QuadAll(c) << 1;
  else
    o[t * 4 + 2] = 0xffffffff;
  o[t * 4 + 3] = QuadAll(true);
}

// one pixel center, (1.5, 1.5) of a SIZE x SIZE target, inside a small triangle
float4 vs(uint id : SV_VertexID) : SV_Position {
  float2 p = float2(1.4 + 0.2 * (id == 1) + 0.1 * (id == 2), 1.4 + 0.2 * (id == 2));
  return float4(p / SIZE * float2(2, -2) + float2(-1, 1), 0, 1);
}
// the wave's lanes in the low byte; then the quad's votes on its helpers
uint votes() {
  bool h = IsHelperLane();
  return WaveActiveCountBits(true) | QuadAny(h) << 8 | QuadAll(h) << 9 | QuadAny(!h) << 10 | QuadAll(true) << 11;
}
[WaveOpsIncludeHelperLanes] uint ps() : SV_Target { return votes(); }
uint ps_live() : SV_Target { return votes(); }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (!compiler.dxc) {
    printf("skipped: shader model 6.7 operations are DXIL only\n");
    return 77;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  unsigned failures = 0;
  D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_7};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model)));
  if (model.HighestShaderModel != D3D_SHADER_MODEL_6_7 && ++failures)
    printf("the highest shader model asked up to 6.7 is %#x\n", model.HighestShaderModel);
  D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
  CHECK(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options, sizeof(options)));
  const UINT lanes = options.WaveLaneCountMin, size = 4;
  std::vector<std::string> defines = {"LANES=" + std::to_string(lanes), "SIZE=" + std::to_string(size)};
  auto cs = compiler.compile(hlsl, "cs", "cs_6_7", defines), vs = compiler.compile(hlsl, "vs", "vs_6_7", defines),
       ps = compiler.compile(hlsl, "ps", "ps_6_7", defines),
       ps_live = compiler.compile(hlsl, "ps_live", "ps_6_7", defines);
  if (cs.empty() || vs.empty() || ps.empty() || ps_live.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }

  D3D12_ROOT_PARAMETER param{D3D12_ROOT_PARAMETER_TYPE_UAV};
  auto rs = root_signature(device.Get(), {1, &param});
  auto draw_rs = root_signature(device.Get(), {});
  D3D12_COMPUTE_PIPELINE_STATE_DESC cs_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> cs_pso, draw_pso[2];
  CHECK(device->CreateComputePipelineState(&cs_desc, IID_PPV_ARGS(&cs_pso)));
  D3D12_GRAPHICS_PIPELINE_STATE_DESC draw_desc{draw_rs.Get(), bytecode(vs), bytecode(ps)};
  draw_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  draw_desc.SampleMask = ~0u;
  draw_desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  draw_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  draw_desc.NumRenderTargets = 1;
  draw_desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
  draw_desc.SampleDesc = {1, 0};
  CHECK(device->CreateGraphicsPipelineState(&draw_desc, IID_PPV_ARGS(&draw_pso[0])));
  draw_desc.PS = bytecode(ps_live);
  CHECK(device->CreateGraphicsPipelineState(&draw_desc, IID_PPV_ARGS(&draw_pso[1])));

  const UINT64 bytes = lanes * 16, row = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
  const UINT64 pixels_at =
      (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~UINT64(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  auto out = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  // a target for each pixel shader, one after the other
  const UINT64 target_bytes = row * size;
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, pixels_at + 2 * target_bytes, D3D12_RESOURCE_STATE_COPY_DEST);
  ComPtr<ID3D12Resource> target;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D,
                                  0,
                                  size,
                                  size,
                                  1,
                                  1,
                                  DXGI_FORMAT_R32_UINT,
                                  {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN,
                                  D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  CHECK(device->CreateCommittedResource(
      &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
  ));
  ComPtr<ID3D12DescriptorHeap> rtvs;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
  auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
  CHECK(
      device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), cs_pso.Get(), IID_PPV_ARGS(&list))
  );
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootUnorderedAccessView(0, out->GetGPUVirtualAddress());
  list->Dispatch(1, 1, 1);
  transition(list.Get(), out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyBufferRegion(readback.Get(), 0, out.Get(), 0, bytes);
  const UINT untouched = 7;
  const float clear_value[4] = {untouched, untouched, untouched, untouched};
  for (UINT i = 0; i < 2; i++) {
    if (i)
      transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list->ClearRenderTargetView(rtv, clear_value, 0, nullptr);
    list->SetPipelineState(draw_pso[i].Get());
    list->SetGraphicsRootSignature(draw_rs.Get());
    D3D12_VIEWPORT viewport{0, 0, (float)size, (float)size, 0, 1};
    D3D12_RECT scissor{0, 0, (LONG)size, (LONG)size};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dst.PlacedFootprint = {pixels_at + i * target_bytes, {DXGI_FORMAT_R32_UINT, size, size, 1, (UINT)row}};
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  CHECK(submit(device.Get(), queue.Get(), list.Get()));
  UINT *o;
  CHECK(readback->Map(0, nullptr, (void **)&o));

  auto condition = [](UINT lane) { return ((lane / 4 * 5 + 3) % 16 >> (lane % 4)) & 1; };
  std::map<UINT, UINT> thread_of;
  for (UINT t = 0; t < lanes; t++)
    thread_of[o[t * 4]] = t;
  if (thread_of.size() != lanes || thread_of.rbegin()->first != lanes - 1)
    printf("failed: %zu distinct lanes of %u\n", thread_of.size(), lanes), failures++;
  for (auto [lane, t] : thread_of) {
    UINT first = lane & ~3u;
    bool any = false, all = true, any_live = false, all_live = true;
    for (UINT l = first; l < first + 4; l++) {
      any |= condition(l), all &= condition(l);
      if (l % 4 != 3)
        any_live |= condition(l), all_live &= condition(l);
    }
    UINT want = any | all << 1, want_live = lane % 4 != 3 ? any_live | all_live << 1 : 0xffffffff;
    if (o[t * 4 + 1] != want && failures++ < 8)
      printf(
          "lane %u (quad pattern %#x): QuadAny | QuadAll << 1 is %#x, want %#x\n", lane, (lane / 4 * 5 + 3) % 16,
          o[t * 4 + 1], want
      );
    if (o[t * 4 + 2] != want_live && failures++ < 8)
      printf(
          "lane %u without the quad's last lane: QuadAny | QuadAll << 1 is %#x, want %#x\n", lane, o[t * 4 + 2],
          want_live
      );
    if (o[t * 4 + 3] != 1 && failures++ < 8)
      printf("lane %u: QuadAll(true) is %u\n", lane, o[t * 4 + 3]);
  }
  // the pixel at (1, 1) is live, its quad's other three helpers: four lanes with the attribute, one without, and
  // either way some helpers, not all, one live lane, all true. nothing else runs
  for (UINT i = 0; i < 2; i++) {
    auto pixels = o + (pixels_at + i * target_bytes) / 4;
    UINT votes = 1 << 8 | 0 << 9 | 1 << 10 | 1 << 11, counted = i ? 1 : 4;
    for (UINT py = 0; py < size; py++)
      for (UINT px = 0; px < size; px++) {
        UINT got = pixels[py * row / 4 + px], want = px == 1 && py == 1 ? counted | votes : untouched;
        if (got != want && failures++ < 8)
          printf(
              "pixel (%u, %u) %s helpers in wave operations: %#x, want %#x (lanes in the low byte, then "
              "QuadAny(helper), QuadAll(helper), QuadAny(live), QuadAll(true))\n",
              px, py, i ? "without" : "with", got, want
          );
      }
  }
  readback->Unmap(0, nullptr);
  printf(
      "%s: %u mismatches over %u lanes and %u pixels\n", failures ? "failed" : "passed", failures, lanes,
      2 * size * size
  );
  return failures != 0;
}
