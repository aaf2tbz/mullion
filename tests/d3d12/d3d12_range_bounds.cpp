// contract: a buffer load past its view's end reads zeros where the root signature keeps bounds checks, and may read
// what lies past the view where it lets the driver drop them. "the driver is not allowed to promote buffers to root
// descriptors as an optimization, because they must maintain bounds checks and root descriptors do not have those"
// (D3D12_DESCRIPTOR_RANGE_FLAGS, DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS): a root signature 1.1 range that
// is neither DESCRIPTORS_VOLATILE nor STATIC_KEEPING_BUFFER_BOUNDS_CHECKS may be promoted, and root signature 1.0's
// ranges are volatile. Mullion drops the checks of such a range's loads, so a view inside a larger buffer reads the
// buffer's bytes past its end there, and zeros everywhere else (D3D11.3 7.5 for constant buffers, 22.4.10 and 22.4.12
// for raw and structured ones). A constant buffer view is a multiple of 256 bytes (D3D12_CONSTANT_BUFFER_VIEW_DESC),
// so its first 16 rows read the buffer whatever the range, and a row past it, whether named by a constant or an
// index, reads zeros where checks are kept. A structured element past the view's end reads zeros as a whole, its
// last word and its first alike, while the last element inside it reads all of its words.
#include "d3d12_test.hpp"

static const char hlsl[] = R"hlsl(
StructuredBuffer<uint4> elements : register(t0);
ByteAddressBuffer bytes : register(t1);
cbuffer Rows : register(b0) { uint4 rows[32]; };
cbuffer Index : register(b1) { uint past_row, inside_row, past_element; };
RWStructuredBuffer<uint> results : register(u0);
[numthreads(1, 1, 1)] void cs() {
  results[0] = elements[past_element].x;
  results[1] = elements[past_element].w;
  results[2] = elements[3].x + elements[3].w;
  results[3] = bytes.Load(64);
  results[4] = rows[15].x;
  results[5] = rows[16].x;
  results[6] = rows[past_row].x;
  results[7] = rows[inside_row].x;
}
)hlsl";

// the buffer's dword i holds kBase + i; the views take its first 64 bytes (4 elements of 16, 16 raw dwords) and its
// first 256 (a constant buffer of 16 rows)
static const UINT kBase = 0x1000, kResults = 8;
static UINT dword(UINT i) { return kBase + i; }

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

  auto upload = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 4096, D3D12_RESOURCE_STATE_GENERIC_READ);
  UINT *mapped;
  CHECK(upload->Map(0, nullptr, (void **)&mapped));
  for (UINT i = 0; i < 1024; i++)
    mapped[i] = dword(i);
  upload->Unmap(0, nullptr);
  auto results = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 256, D3D12_RESOURCE_STATE_COPY_DEST);

  ComPtr<ID3D12DescriptorHeap> heap;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  auto slot = [&](UINT n) {
    auto handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += n * increment;
    return handle;
  };
  D3D12_SHADER_RESOURCE_VIEW_DESC structured{DXGI_FORMAT_UNKNOWN, D3D12_SRV_DIMENSION_BUFFER,
                                             D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  structured.Buffer = {0, 4, 16, D3D12_BUFFER_SRV_FLAG_NONE};
  device->CreateShaderResourceView(upload.Get(), &structured, slot(0));
  D3D12_SHADER_RESOURCE_VIEW_DESC raw{DXGI_FORMAT_R32_TYPELESS, D3D12_SRV_DIMENSION_BUFFER,
                                      D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING};
  raw.Buffer = {0, 16, 0, D3D12_BUFFER_SRV_FLAG_RAW};
  device->CreateShaderResourceView(upload.Get(), &raw, slot(1));
  D3D12_CONSTANT_BUFFER_VIEW_DESC rows{upload->GetGPUVirtualAddress(), 256};
  device->CreateConstantBufferView(&rows, slot(2));

  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));

  // the ranges' flags, and whether their loads keep their checks
  struct Case {
    const char *name;
    D3D_ROOT_SIGNATURE_VERSION version;
    D3D12_DESCRIPTOR_RANGE_FLAGS flags;
    bool checked;
  };
  const Case cases[] = {
      {"1.0", D3D_ROOT_SIGNATURE_VERSION_1_0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, true},
      {"1.1 descriptors volatile", D3D_ROOT_SIGNATURE_VERSION_1_1, D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_VOLATILE, true},
      {"1.1 keeping bounds checks", D3D_ROOT_SIGNATURE_VERSION_1_1, D3D12_DESCRIPTOR_RANGE_FLAGS(0x10000), true},
      {"1.1 static", D3D_ROOT_SIGNATURE_VERSION_1_1, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, false},
      {"1.1 data static while set", D3D_ROOT_SIGNATURE_VERSION_1_1,
       D3D12_DESCRIPTOR_RANGE_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE, false},
  };
  for (auto &c : cases) {
    step("root signature %s", c.name);
    D3D12_DESCRIPTOR_RANGE1 ranges[2] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, c.flags, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, c.flags, 2},
    };
    D3D12_ROOT_PARAMETER1 params[3] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {2, ranges};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {1, 0, 3};
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[2].Descriptor = {0, 0, D3D12_ROOT_DESCRIPTOR_FLAG_NONE};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc{D3D_ROOT_SIGNATURE_VERSION_1_1};
    desc.Desc_1_1 = {3, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob, error;
    if (c.version == D3D_ROOT_SIGNATURE_VERSION_1_0) {
      // the same layout in 1.0's structures, which carry no flags
      D3D12_DESCRIPTOR_RANGE ranges0[2] = {
          {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, 2}
      };
      D3D12_ROOT_PARAMETER params0[3] = {};
      params0[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      params0[0].DescriptorTable = {2, ranges0};
      params0[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
      params0[1].Constants = {1, 0, 3};
      params0[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
      params0[2].Descriptor = {0, 0};
      D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc0{D3D_ROOT_SIGNATURE_VERSION_1_0};
      desc0.Desc_1_0 = {3, params0, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
      CHECK(D3D12SerializeVersionedRootSignature(&desc0, &blob, &error));
    } else {
      CHECK(D3D12SerializeVersionedRootSignature(&desc, &blob, &error));
    }
    ComPtr<ID3D12RootSignature> rs;
    CHECK(device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{rs.Get(), bytecode(cs)};
    ComPtr<ID3D12PipelineState> pso;
    CHECK(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&pso)));

    CHECK(allocator->Reset());
    if (!list) {
      CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    } else {
      CHECK(list->Reset(allocator.Get(), nullptr));
    }
    ID3D12DescriptorHeap *heaps[] = {heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(rs.Get());
    list->SetPipelineState(pso.Get());
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    const UINT indices[3] = {17, 3, 4}; // past_row, inside_row, past_element
    list->SetComputeRoot32BitConstants(1, 3, indices, 0);
    list->SetComputeRootUnorderedAccessView(2, results->GetGPUVirtualAddress());
    list->Dispatch(1, 1, 1);
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, results.Get(), 0, kResults * 4);
    transition(list.Get(), results.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));

    UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    auto past = [&](UINT value) { return c.checked ? 0u : value; };
    const UINT want[kResults] = {
        past(dword(16)),         // element 4's first word, past the 4 elements
        past(dword(19)),         // and its last
        dword(12) + dword(15),   // element 3's first and last words, inside
        past(dword(16)),         // the raw view's 17th dword
        dword(60),               // row 15, the view's last
        past(dword(64)),         // row 16, named by a constant
        past(dword(68)),         // row 17, by an index
        dword(12),               // row 3, by an index
    };
    for (UINT i = 0; i < kResults; i++)
      expect(got[i] == want[i], "result %u is %#x, want %#x", i, got[i], want[i]);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}
