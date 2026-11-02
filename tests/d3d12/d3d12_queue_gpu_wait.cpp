// contract: "ID3D12CommandQueue::Wait: Queues a GPU-side wait, and returns immediately" — the work a queue is given
// after a Wait runs after the fence reaches the value, whichever queue brings it and whenever that queue's Signal is
// submitted. a compute queue writes a buffer, long enough to be still running, and signals a fence; a direct queue
// waits for it, copies the buffer and signals a fence of its own, which the CPU waits for and then reads the copy:
// - the Wait submitted after the compute queue's Signal is committed (Mullion waits on the GPU);
// - the Wait submitted before the Signal, which the compute queue holds behind a fence the CPU signals later (Mullion
//   holds the direct queue's work until the fence is there);
// - the direct queue's Signal straight after its Wait, with no work between, reaches its value after the compute
//   queue's work too.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
RWByteAddressBuffer written : register(u0);
cbuffer C : register(b0) { uint value, rounds; };
[numthreads(64, 1, 1)] void cs(uint i : SV_DispatchThreadID) {
  // a running sum the compiler cannot fold, so the dispatch takes a while
  uint sum = value;
  for (uint r = 0; r < rounds; r++)
    sum = sum * 1664525u + 1013904223u + i;
  written.Store(i * 4, sum == 0x12345678u ? 0 : value);
}
)hlsl";

static const UINT kThreads = 64 * 256, kRounds = 1 << 16;

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  auto cs = compiler.compile(hlsl, "cs", "cs", {});
  if (!expect(!cs.empty(), "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_UAV}, {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}};
  params[1].Constants.Num32BitValues = 2;
  auto rs = root_signature(device.Get(), {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
  ComPtr<ID3D12PipelineState> pso;
  CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

  auto written = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, kThreads * 4, D3D12_RESOURCE_STATE_COMMON,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, kThreads * 4, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12CommandQueue> compute, direct;
  D3D12_COMMAND_QUEUE_DESC compute_desc{D3D12_COMMAND_LIST_TYPE_COMPUTE}, direct_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&compute_desc, IID_PPV_ARGS(&compute)));
  CHECK(device->CreateCommandQueue(&direct_desc, IID_PPV_ARGS(&direct)));
  ComPtr<ID3D12CommandAllocator> compute_allocator, direct_allocator;
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&compute_allocator)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&direct_allocator)));
  ComPtr<ID3D12GraphicsCommandList> writes, copies;
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, compute_allocator.Get(), nullptr, IID_PPV_ARGS(&writes)));
  CHECK(writes->Close());
  CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, direct_allocator.Get(), nullptr, IID_PPV_ARGS(&copies)));
  CHECK(copies->Close());

  ComPtr<ID3D12Fence> written_fence, copied_fence, gate_fence;
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&written_fence)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copied_fence)));
  CHECK(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate_fence)));
  UINT64 written_value = 0, copied_value = 0, gate_value = 0;
  auto event = CreateEventA(nullptr, FALSE, FALSE, nullptr);

  enum Order { SignalFirst, WaitFirst, SignalAfterWait };
  const char *names[] = {"Wait after the committed Signal", "Wait before the Signal", "Signal straight after the Wait"};
  for (UINT round = 0; round < 6; round++) {
    auto order = Order(round % 3);
    const UINT value = 0x100 + round;
    step("%s, value %#x", names[order], value);
    // the compute queue's work: the buffer written with `value`
    CHECK(compute_allocator->Reset());
    CHECK(writes->Reset(compute_allocator.Get(), pso.Get()));
    writes->SetComputeRootSignature(rs.Get());
    writes->SetComputeRootUnorderedAccessView(0, written->GetGPUVirtualAddress());
    const UINT constants[2] = {value, kRounds};
    writes->SetComputeRoot32BitConstants(1, 2, constants, 0);
    writes->Dispatch(kThreads / 64, 1, 1);
    CHECK(writes->Close());
    // the direct queue's: the buffer copied for the CPU
    CHECK(direct_allocator->Reset());
    CHECK(copies->Reset(direct_allocator.Get(), nullptr));
    copies->CopyBufferRegion(readback.Get(), 0, written.Get(), 0, kThreads * 4);
    CHECK(copies->Close());

    ID3D12CommandList *write_lists[] = {writes.Get()}, *copy_lists[] = {copies.Get()};
    auto signal_written = [&] {
      compute->ExecuteCommandLists(1, write_lists);
      return compute->Signal(written_fence.Get(), ++written_value);
    };
    if (order == WaitFirst) {
      // the compute queue's work is held behind the CPU's fence, so its Signal is not committed at the Wait
      CHECK(compute->Wait(gate_fence.Get(), ++gate_value));
      CHECK(signal_written());
      CHECK(direct->Wait(written_fence.Get(), written_value));
      direct->ExecuteCommandLists(1, copy_lists);
      CHECK(direct->Signal(copied_fence.Get(), ++copied_value));
      Sleep(10);
      CHECK(gate_fence->Signal(gate_value));
    } else if (order == SignalFirst) {
      CHECK(signal_written());
      CHECK(direct->Wait(written_fence.Get(), written_value));
      direct->ExecuteCommandLists(1, copy_lists);
      CHECK(direct->Signal(copied_fence.Get(), ++copied_value));
    } else {
      // the direct queue's fence follows the compute queue's work with nothing between: the CPU then copies itself
      CHECK(signal_written());
      CHECK(direct->Wait(written_fence.Get(), written_value));
      CHECK(direct->Signal(copied_fence.Get(), ++copied_value));
    }
    CHECK(copied_fence->SetEventOnCompletion(copied_value, event));
    WaitForSingleObject(event, INFINITE);
    if (order == SignalAfterWait) {
      expect(written_fence->GetCompletedValue() >= written_value,
             "the direct queue's fence reached %llu before the compute queue's work ended (fence at %llu of %llu)",
             copied_value, written_fence->GetCompletedValue(), written_value);
      CHECK(written_fence->SetEventOnCompletion(written_value, event));
      WaitForSingleObject(event, INFINITE);
      // the copy, now that the work is done
      CHECK(direct_allocator->Reset());
      CHECK(copies->Reset(direct_allocator.Get(), nullptr));
      copies->CopyBufferRegion(readback.Get(), 0, written.Get(), 0, kThreads * 4);
      CHECK(submit(device.Get(), direct.Get(), copies.Get()));
    }
    UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    UINT wrong = 0;
    for (UINT i = 0; i < kThreads; i++)
      wrong += got[i] != value;
    expect(!wrong, "%u of %u copied words are not %#x (word 0 is %#x)", wrong, kThreads, value, got[0]);
    readback->Unmap(0, nullptr);
  }
  CloseHandle(event);
  return verdict();
}
