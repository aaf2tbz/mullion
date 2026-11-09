// contract: a particle system's sprites, drawn as Unreal's Niagara draws them, come out where and as the particle data
// of their own frame says. each frame the CPU writes the particles into a pooled upload buffer at a place of the
// frame's (positions as R32_FLOAT, sizes and colors as R16_FLOAT, the draw order as R32_UINT sorted indices), makes
// typed buffer views of them whose FirstElement is that place, and a compute pass writes the frame's indexed draw
// arguments, its instance count among them, which ExecuteIndirect then draws with one command and no count buffer.
// a sprite finds its particle as sorted[offset + SV_InstanceID] and reads attribute c of particle p at element
// c * stride + p of the float or the half buffer, and its corner from a per-vertex attribute.
// "SV_InstanceID ... starts at 0 for the first instance" (D3D11.3 8.18); a typed buffer view begins at
// FirstElement (D3D12_BUFFER_SRV); ExecuteIndirect "instruct[s] the GPU to interpret the contents of the indirect
// argument buffer" when it runs (Indirect Drawing). an upload buffer the GPU still reads is not written: frames reuse
// their memory only after their fence (ID3D12Fence), with two frames in flight.
// the target is a grid of cells, one a particle, each of whose pixels has the color of the particle there and its
// frame; a cell no particle has keeps the clear color. every frame's picture is read back and checked.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
cbuffer Frame : register(b0) {
  uint float_offset; uint half_offset; uint sorted_offset; uint stride;
  uint count; uint frame; uint args_at; uint pad;
};
Buffer<float> floats : register(t0);
Buffer<float> halves : register(t1);
Buffer<uint> sorted : register(t2);
RWByteAddressBuffer args : register(u0);

// the frame's one indexed draw: 6 indices, `count` instances
[numthreads(1, 1, 1)] void cs() {
  args.Store(args_at, 6);
  args.Store(args_at + 4, count);
  args.Store(args_at + 8, 0);
  args.Store(args_at + 12, 0);
  args.Store(args_at + 16, 0);
}

struct Out { float4 at : SV_Position; nointerpolation float4 color : COLOR; };
Out vs(float2 corner : CORNER, uint instance : SV_InstanceID) {
  uint p = sorted[instance];
  float x = floats[0 * stride + p], y = floats[1 * stride + p];
  float size = halves[0 * stride + p], shade = halves[1 * stride + p];
  // the cell's center, and half a cell each way times its size (1 covers the cell, less stays inside it)
  float2 center = float2((x + 0.5) * 2 / CELLS - 1, 1 - (y + 0.5) * 2 / CELLS);
  Out o;
  o.at = float4(center + (corner * 2 - 1) * size / CELLS, 0, 1);
  o.color = float4(p + 1, frame, shade, 1);
  return o;
}
float4 ps(Out i) : SV_Target { return i.color; }
)hlsl";

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  // a CELLS x CELLS grid of PIXELS-pixel cells; particles of at most CELLS * CELLS, stride the most of them
  const UINT cells = 8, pixels = 8, side = cells * pixels, most = cells * cells, stride = most, frames = 90,
             in_flight = 2;
  const std::vector<std::string> defines = {"CELLS=" + std::to_string(cells)};
  auto cs = compiler.compile(hlsl, "cs", "cs", defines), vs = compiler.compile(hlsl, "vs", "vs", defines),
       ps = compiler.compile(hlsl, "ps", "ps", defines);
  if (cs.empty() || vs.empty() || ps.empty()) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));

  // root: the frame's constants, a table of the three views (as Unreal's parameter 0), the arguments' UAV
  D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0};
  D3D12_ROOT_PARAMETER params[3] = {{D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS},
                                    {D3D12_ROOT_PARAMETER_TYPE_UAV}};
  params[0].DescriptorTable = {1, &range};
  params[1].Constants = {0, 0, 8};
  params[2].Descriptor = {0, 0};
  auto rs = root_signature(device.Get(), {(UINT)std::size(params), params, 0, nullptr,
                                          D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT});
  const DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  const D3D12_INPUT_ELEMENT_DESC element{"CORNER", 0, DXGI_FORMAT_R32G32_FLOAT};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = rs.Get();
  desc.VS = bytecode(vs), desc.PS = bytecode(ps);
  desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  desc.SampleMask = ~0u;
  desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
  desc.InputLayout = {&element, 1};
  desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  desc.NumRenderTargets = 1, desc.RTVFormats[0] = format;
  desc.SampleDesc = {1, 0};
  ComPtr<ID3D12PipelineState> draw, write;
  CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&draw)));
  D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc{rs.Get(), bytecode(cs)};
  CHECK(device->CreateComputePipelineState(&compute_desc, IID_PPV_ARGS(&write)));
  const D3D12_INDIRECT_ARGUMENT_DESC kind{D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED};
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{5 * sizeof(UINT), 1, &kind};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, nullptr, IID_PPV_ARGS(&signature)));

  // the target and its readbacks, one a frame in flight
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, side, side, 1, 1, format, {1, 0},
                                  D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
  ComPtr<ID3D12Resource> target;
  CHECK(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                        nullptr, IID_PPV_ARGS(&target)));
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
  CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap)));
  auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  device->CreateRenderTargetView(target.Get(), nullptr, rtv);
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
  UINT64 bytes;
  device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  ComPtr<ID3D12Resource> readbacks[in_flight];
  for (auto &readback : readbacks)
    readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST);

  // the corners and the indices of a quad
  auto corners = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 4 * 2 * sizeof(float), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto indices = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 6 * sizeof(UINT16), D3D12_RESOURCE_STATE_GENERIC_READ);
  {
    float *corner;
    UINT16 *index;
    CHECK(corners->Map(0, nullptr, (void **)&corner));
    CHECK(indices->Map(0, nullptr, (void **)&index));
    const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    const UINT16 tris[6] = {0, 1, 2, 2, 1, 3};
    memcpy(corner, quad, sizeof(quad));
    memcpy(index, tris, sizeof(tris));
  }
  // the pool the particles of every frame go into, each frame at a place of its own that moves around the pool
  const UINT64 pool_bytes = 1 << 20;
  auto pool = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, pool_bytes, D3D12_RESOURCE_STATE_GENERIC_READ);
  uint8_t *pool_bytes_at;
  CHECK(pool->Map(0, nullptr, (void **)&pool_bytes_at));
  // the arguments, a slot a frame in flight at an offset that moves
  const UINT64 args_bytes = 64 * 1024;
  auto args = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, args_bytes, D3D12_RESOURCE_STATE_COMMON,
                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

  // a shader-visible heap with a table of three views a frame in flight
  ComPtr<ID3D12DescriptorHeap> views;
  D3D12_DESCRIPTOR_HEAP_DESC views_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3 * in_flight,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&views_desc, IID_PPV_ARGS(&views)));
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  ComPtr<ID3D12CommandQueue> queue;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12Fence> fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
  ComPtr<ID3D12CommandAllocator> allocators[in_flight];
  ComPtr<ID3D12GraphicsCommandList> lists[in_flight];
  for (UINT i = 0; i < in_flight; i++) {
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i])));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                    IID_PPV_ARGS(&lists[i])));
    CHECK(lists[i]->Close());
  }
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
  auto wait = [&](UINT64 value) {
    if (fence->GetCompletedValue() >= value)
      return true;
    fence->SetEventOnCompletion(value, event);
    return WaitForSingleObject(event, 10 * 1000) == WAIT_OBJECT_0;
  };

  // what each frame in flight drew, to check once its fence is there: the cell each particle is in, and its shade
  struct Frame {
    std::vector<int> owner; // per cell: the particle drawn there, -1 for none
    std::vector<float> shade;
    UINT number = 0;
  } expected[in_flight];
  uint32_t seed = 12345;
  auto random = [&] { return seed = seed * 1664525u + 1013904223u, seed >> 8; };
  auto to_half = [](float value) {
    // exact for the small integers and halves the test uses
    uint32_t bits;
    memcpy(&bits, &value, 4);
    uint32_t sign = (bits >> 16) & 0x8000, exponent = ((bits >> 23) & 0xff), mantissa = bits & 0x7fffff;
    if (!exponent)
      return (uint16_t)sign;
    return (uint16_t)(sign | ((exponent - 112) << 10) | (mantissa >> 13));
  };
  UINT failures = 0, checked = 0;
  auto check = [&](UINT slot) {
    auto &frame = expected[slot];
    float *texels;
    if (FAILED(readbacks[slot]->Map(0, nullptr, (void **)&texels)))
      return;
    UINT wrong = 0;
    for (UINT cell = 0; cell < most; cell++) {
      // the cell's center pixel
      UINT x = (cell % cells) * pixels + pixels / 2, y = (cell / cells) * pixels + pixels / 2;
      const float *texel = texels + (y * footprint.Footprint.RowPitch / sizeof(float)) + x * 4;
      int owner = frame.owner[cell];
      float want[4] = {0, 0, 0, 0};
      if (owner >= 0)
        want[0] = float(owner + 1), want[1] = float(frame.number), want[2] = frame.shade[cell], want[3] = 1;
      if (memcmp(texel, want, sizeof(want))) {
        if (wrong++ < 4)
          printf("frame %u cell %u: (%g %g %g %g), want (%g %g %g %g)\n", frame.number, cell, texel[0], texel[1],
                 texel[2], texel[3], want[0], want[1], want[2], want[3]);
      }
    }
    readbacks[slot]->Unmap(0, nullptr);
    checked++;
    if (wrong) {
      failures++;
      printf("frame %u: %u of %u cells wrong\n", frame.number, wrong, most);
    }
  };

  UINT64 place = 0;
  for (UINT number = 1; number <= frames; number++) {
    const UINT slot = number % in_flight;
    // the slot's frame before is done: its picture is checked, and its memory is free again
    if (number > in_flight) {
      if (!wait(number - in_flight)) {
        printf("failed: frame %u did not complete\n", number - in_flight);
        return 1;
      }
      check(slot);
    }
    // this frame's particles: how many, which cell each is in, its size and shade, and the order they are drawn in
    const UINT count = 1 + random() % most;
    std::vector<UINT> cells_of(most);
    for (UINT i = 0; i < most; i++)
      cells_of[i] = i;
    for (UINT i = most - 1; i > 0; i--)
      std::swap(cells_of[i], cells_of[random() % (i + 1)]);
    std::vector<UINT> order(count);
    for (UINT i = 0; i < count; i++)
      order[i] = i;
    for (UINT i = count - 1; i > 0; i--)
      std::swap(order[i], order[random() % (i + 1)]);
    // the frame's place in the pool, a different one each frame: floats, then halves, then the order. it moves on
    // past what the frames in flight hold, and wraps
    const UINT64 float_bytes = 2 * stride * sizeof(float), half_bytes = 2 * stride * sizeof(uint16_t),
                 order_bytes = count * sizeof(UINT), frame_bytes = float_bytes + half_bytes + order_bytes + 256;
    place = (place + 4 * (1 + random() % 64)) % (pool_bytes / 2);
    const UINT64 float_at = place, half_at = float_at + float_bytes, order_at = half_at + half_bytes;
    place += frame_bytes;
    float *floats = (float *)(pool_bytes_at + float_at);
    uint16_t *halves = (uint16_t *)(pool_bytes_at + half_at);
    UINT *sorted = (UINT *)(pool_bytes_at + order_at);
    auto &frame = expected[slot];
    frame.number = number;
    frame.owner.assign(most, -1);
    frame.shade.assign(most, 0);
    for (UINT p = 0; p < count; p++) {
      UINT cell = cells_of[p];
      floats[0 * stride + p] = float(cell % cells);
      floats[1 * stride + p] = float(cell / cells);
      // a size that keeps the sprite inside its cell but over its center, and a shade that a half holds exactly
      halves[0 * stride + p] = to_half(0.5f + 0.25f * (random() % 2));
      float shade = float(random() % 64) / 2;
      halves[1 * stride + p] = to_half(shade);
      frame.owner[cell] = p;
      frame.shade[cell] = shade;
    }
    for (UINT i = 0; i < count; i++)
      sorted[i] = order[i];

    // the frame's views, at the place of its particles (FirstElement in elements of each format)
    auto table = views->GetCPUDescriptorHandleForHeapStart();
    table.ptr += 3 * slot * increment;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{DXGI_FORMAT_R32_FLOAT, D3D12_SRV_DIMENSION_BUFFER,
                                        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
    srv.Buffer = {float_at / sizeof(float), 2 * stride, 0, D3D12_BUFFER_SRV_FLAG_NONE};
    device->CreateShaderResourceView(pool.Get(), &srv, table);
    table.ptr += increment;
    srv.Format = DXGI_FORMAT_R16_FLOAT;
    srv.Buffer = {half_at / sizeof(uint16_t), 2 * stride, 0, D3D12_BUFFER_SRV_FLAG_NONE};
    device->CreateShaderResourceView(pool.Get(), &srv, table);
    table.ptr += increment;
    srv.Format = DXGI_FORMAT_R32_UINT;
    srv.Buffer = {order_at / sizeof(UINT), count, 0, D3D12_BUFFER_SRV_FLAG_NONE};
    device->CreateShaderResourceView(pool.Get(), &srv, table);

    // the arguments' place, a slot of the frame's at an offset that moves
    const UINT args_at = UINT((slot * (args_bytes / in_flight)) + 20 * (random() % 64));
    const UINT constants[8] = {0, 0, 0, stride, count, number, args_at, 0};

    auto &list = lists[slot];
    CHECK(allocators[slot]->Reset());
    CHECK(list->Reset(allocators[slot].Get(), write.Get()));
    ID3D12DescriptorHeap *heaps[] = {views.Get()};
    list->SetDescriptorHeaps(1, heaps);
    auto gpu_table = views->GetGPUDescriptorHandleForHeapStart();
    gpu_table.ptr += 3 * slot * increment;
    // the compute pass writes the frame's arguments; the draw reads them as arguments after the barrier
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootDescriptorTable(0, gpu_table);
    list->SetComputeRoot32BitConstants(1, 8, constants, 0);
    list->SetComputeRootUnorderedAccessView(2, args->GetGPUVirtualAddress());
    transition(list.Get(), args.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->Dispatch(1, 1, 1);
    transition(list.Get(), args.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

    const float clear[4] = {0, 0, 0, 0};
    list->ClearRenderTargetView(rtv, clear, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{0, 0, float(side), float(side), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(side), LONG(side)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->SetPipelineState(draw.Get());
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootDescriptorTable(0, gpu_table);
    list->SetGraphicsRoot32BitConstants(1, 8, constants, 0);
    list->SetGraphicsRootUnorderedAccessView(2, args->GetGPUVirtualAddress());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv{corners->GetGPUVirtualAddress(), 4 * 2 * sizeof(float), 2 * sizeof(float)};
    D3D12_INDEX_BUFFER_VIEW ibv{indices->GetGPUVirtualAddress(), 6 * sizeof(UINT16), DXGI_FORMAT_R16_UINT};
    list->IASetVertexBuffers(0, 1, &vbv);
    list->IASetIndexBuffer(&ibv);
    list->ExecuteIndirect(signature.Get(), 1, args.Get(), args_at, nullptr, 0);
    transition(list.Get(), args.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COMMON);

    // the picture, read back
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, to{
        readbacks[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    from.SubresourceIndex = 0;
    to.PlacedFootprint = footprint;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    CHECK(list->Close());
    ID3D12CommandList *one[] = {list.Get()};
    queue->ExecuteCommandLists(1, one);
    CHECK(queue->Signal(fence.Get(), number));
  }
  // the last frames in flight
  for (UINT number = frames - in_flight + 1; number <= frames; number++) {
    if (!wait(number)) {
      printf("failed: frame %u did not complete\n", number);
      return 1;
    }
    check(number % in_flight);
  }
  CloseHandle(event);
  CHECK(device->GetDeviceRemovedReason());
  if (failures) {
    printf("failed: %u of %u frames drew sprites wrongly\n", failures, checked);
    return 1;
  }
  printf("passed: %u frames of indirect sprites\n", checked);
  return 0;
}
