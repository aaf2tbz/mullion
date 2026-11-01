// contract: a compute shader writes what its root arguments name, whichever converter made its kernel. Mullion hands
// DXIL compute shaders whose root signatures hold only root constants and descriptors to Metal Shader Converter when
// MULLION_MSC=1 (and libmetalirconverter loads), and its kernels read the root arguments from a binding of their own,
// so this runs where both converters' kernels take turns: a converter's pipeline after Mullion's with the root
// arguments unchanged, root constants that ExecuteIndirect changes per command (ID3D12Device::CreateCommandSignature,
// D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT), a pipeline loaded from a pipeline library
// (ID3D12PipelineLibrary::LoadComputePipeline, from a serialized library), and a root signature whose two blocks of constants the converter lays
// out apart from Mullion (an odd count before another block), which stays with airconv, as a ray query does.
// "Root constants are 32-bit values that show up in shaders as a constant buffer" (Microsoft Learn, Using constants
// directly in the root signature).
// the conversions go to the shader cache as airconv's do, and so do the converter's refusals (64-bit atomics here): a
// process with an empty store converts each shader once, the pipeline library's copy converts nothing, and a process after it on the same store converts nothing and writes the same.
#include "d3d12_test.hpp"
#include "../temporary.hpp"
#include <fstream>
#include <iterator>
#include <vector>

static const char hlsl[] = R"hlsl(
cbuffer Selection : register(b0) { uint offset; uint step; uint region; };
cbuffer Scale : register(b1) { uint4 scale; };
StructuredBuffer<uint> input : register(t0);
RWStructuredBuffer<uint> output : register(u0);
[numthreads(64, 1, 1)] void cs(uint id : SV_DispatchThreadID) {
  output[(region + TAG) * 256 + id] = input[id] * scale.x + offset + step * id;
}
)hlsl";

// 64-bit atomics, which the converter refuses ("unsupported instruction"): airconv converts it, and the refusal is
// kept with the conversions. threads write pairs of the values the others write one at a time
static const char atomic_hlsl[] = R"hlsl(
cbuffer Selection : register(b0) { uint offset; uint step; uint region; };
cbuffer Scale : register(b1) { uint4 scale; };
StructuredBuffer<uint> input : register(t0);
RWByteAddressBuffer output : register(u0);
uint value(uint id) { return input[id] * scale.x + offset + step * id; }
[numthreads(64, 1, 1)] void cs(uint id : SV_DispatchThreadID) {
  output.InterlockedAdd64(((region + 11) * 256 + 2 * id) * 4, uint64_t(value(2 * id)) | (uint64_t(value(2 * id + 1)) << 32));
}
)hlsl";

// a ray query, which stays with airconv: the converter reads an acceleration structure's root SRV as a header of its
// own. made and not dispatched, so a kernel that would read Mullion's acceleration structure that way never runs
static const char query_hlsl[] = R"hlsl(
RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> output : register(u0);
[numthreads(1, 1, 1)] void cs() {
  RayDesc ray = {float3(0, 0, -1), 0, float3(0, 0, 1), 2};
  RayQuery<RAY_FLAG_NONE> query;
  query.TraceRayInline(scene, 0, 0xff, ray);
  query.Proceed();
  output[0] = query.CommittedStatus();
}
)hlsl";

constexpr UINT threads = 256, regions = 12;

static UINT
expected(UINT id, UINT scale, UINT offset, UINT step) {
  return (id * 7 + 3) * scale + offset + step * id;
}

alignas(sizeof(LONG64)) static volatile LONG64 conversions;
static uint64_t (*original_convert)(
    const void *, uint64_t, const void *, uint32_t, const char *, char *, uint32_t
);

static uint64_t
count_convert(
    const void *dxil, uint64_t size, const void *parameters, uint32_t count, const char *entry, char *error,
    uint32_t error_size
) {
  InterlockedIncrement64(&conversions);
  return original_convert(dxil, size, parameters, count, entry, error, error_size);
}

// counts d3d12core's calls of winemetal's WMTMSCConvert, as d3d12_shadercache counts airconv's
static bool
count_imports() {
  auto module = (BYTE *)GetModuleHandleA("d3d12core.dll");
  if (!expect(module != nullptr, "no d3d12core module"))
    return false;
  auto nt = (IMAGE_NT_HEADERS *)(module + ((IMAGE_DOS_HEADER *)module)->e_lfanew);
  auto imports =
      (IMAGE_IMPORT_DESCRIPTOR *)(module +
                                  nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
  for (; imports->Name; imports++) {
    auto names = (IMAGE_THUNK_DATA *)(module + imports->OriginalFirstThunk);
    auto functions = (IMAGE_THUNK_DATA *)(module + imports->FirstThunk);
    for (; names->u1.AddressOfData; names++, functions++) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal) ||
          strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(module + names->u1.AddressOfData))->Name, "WMTMSCConvert"))
        continue;
      DWORD protection;
      if (!expect(
              VirtualProtect(&functions->u1.Function, sizeof(functions->u1.Function), PAGE_READWRITE, &protection),
              "cannot count WMTMSCConvert"
          ))
        return false;
      original_convert = reinterpret_cast<decltype(original_convert)>(functions->u1.Function);
      functions->u1.Function = uintptr_t(count_convert);
      VirtualProtect(&functions->u1.Function, sizeof(functions->u1.Function), protection, &protection);
      return true;
    }
  }
  return expect(false, "d3d12core does not import WMTMSCConvert");
}

static int processes(int argc, char **argv);

int
main(int argc, char **argv) {
  Compiler dxc, fxc;
  if (!front_end(argc, argv, dxc) || !dxc.dxc)
    return 77;
  // without the converter (a Mullion without MetalSharp's library) there is nothing of its to test
  auto identity = reinterpret_cast<uint64_t (*)()>(GetProcAddress(LoadLibraryA("winemetal.dll"), "WMTMSCIdentity"));
  if (!identity || !identity()) {
    printf("Metal Shader Converter is not available\n");
    return 77;
  }
  if (argc <= 3)
    return processes(argc, argv);
  if (!count_imports())
    return verdict();
  const bool warm = !strcmp(argv[2], "warm");

  // TAG keeps each kernel's writes apart: the converter's at 0 and 6, airconv's (DXBC and the root signature it cannot
  // lay out) at 8 and 10
  auto converted = dxc.compile(hlsl, "cs", "cs_6_0", {"TAG=0"});
  auto other = dxc.compile(hlsl, "cs", "cs_6_0", {"TAG=6"});
  auto fxc_code = fxc.compile(hlsl, "cs", "cs_5_1", {"TAG=8"});
  auto packed_code = dxc.compile(hlsl, "cs", "cs_6_0", {"TAG=10"});
  auto query_code = dxc.compile(query_hlsl, "cs", "cs_6_5");
  auto atomic_code = dxc.compile(atomic_hlsl, "cs", "cs_6_6");
  if (!expect(!converted.empty() && !other.empty() && !fxc_code.empty() && !packed_code.empty(),
              "the shaders do not compile"))
    return verdict();

  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  ComPtr<ID3D12Device1> device1;
  CHECK(device.As(&device1));
  ComPtr<ID3D12CommandQueue> queue;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
  ComPtr<ID3D12CommandAllocator> allocator;
  CHECK(device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&allocator)));
  ComPtr<ID3D12GraphicsCommandList> list;
  CHECK(device->CreateCommandList(0, queue_desc.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));

  // the converter's: three constants, then a root CBV, SRV and UAV
  D3D12_ROOT_PARAMETER parameters[4]{};
  parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  parameters[0].Constants = {0, 0, 3};
  parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
  parameters[1].Descriptor = {1, 0};
  parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  parameters[2].Descriptor = {0, 0};
  parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  parameters[3].Descriptor = {0, 0};
  auto rs = root_signature(device.Get(), {4, parameters});
  // airconv's: the scale as four constants right after the three
  D3D12_ROOT_PARAMETER packed_parameters[4];
  memcpy(packed_parameters, parameters, sizeof(parameters));
  packed_parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  packed_parameters[1].Constants = {1, 0, 4};
  auto packed_rs = root_signature(device.Get(), {4, packed_parameters});
  if (!expect(rs && packed_rs, "the root signatures are refused"))
    return verdict();

  ComPtr<ID3D12PipelineState> pipeline, other_pipeline, fxc_pipeline, packed_pipeline, atomic_pipeline, loaded;
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{rs.Get(), bytecode(converted)};
  CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline)));
  desc.CS = bytecode(other);
  CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&other_pipeline)));
  desc.CS = bytecode(fxc_code);
  CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&fxc_pipeline)));
  desc = {packed_rs.Get(), bytecode(packed_code)};
  CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&packed_pipeline)));
  D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
  if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) &&
      model.HighestShaderModel >= D3D_SHADER_MODEL_6_6 && expect(!atomic_code.empty(), "the atomics do not compile")) {
    desc = {rs.Get(), bytecode(atomic_code)};
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&atomic_pipeline)));
  }
  // a serialized pipeline library's copy is made again from the kernel it holds, so it converts nothing
  ComPtr<ID3D12PipelineLibrary> library;
  CHECK(device1->CreatePipelineLibrary(nullptr, 0, IID_PPV_ARGS(&library)));
  CHECK(library->StorePipeline(L"converted", pipeline.Get()));
  std::vector<BYTE> serialized(library->GetSerializedSize());
  CHECK(library->Serialize(serialized.data(), serialized.size()));
  library.Reset();
  CHECK(device1->CreatePipelineLibrary(serialized.data(), serialized.size(), IID_PPV_ARGS(&library)));
  desc = {rs.Get(), bytecode(converted)};
  CHECK(library->LoadComputePipeline(L"converted", &desc, IID_PPV_ARGS(&loaded)));
  expect(loaded.Get() != pipeline.Get(), "the library gave back the stored pipeline, not one made from its kernel");
  // the 64-bit atomics reach the converter (which refuses them) only where the GPU has them: where they are
  // emulated, their threadgroups work together and the pipeline stays with airconv without asking (Apple7)
  const int want_conversions = warm ? 0 : atomic_pipeline ? 3 : 2;
  expect(conversions == want_conversions || (!warm && atomic_pipeline && conversions == 2),
         "%lld conversions in %s, want %d", conversions, argv[2], want_conversions);
  D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
  device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
  if (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1 && expect(!query_code.empty(), "the ray query does not compile")) {
    ComPtr<ID3D12PipelineState> query;
    auto before = conversions;
    desc = {rs.Get(), bytecode(query_code)};
    CHECK(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&query)));
    expect(conversions == before, "the ray query went to the converter");
  }

  auto input = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, threads * sizeof(UINT), D3D12_RESOURCE_STATE_GENERIC_READ);
  auto scales = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 512, D3D12_RESOURCE_STATE_GENERIC_READ);
  auto output = buffer(
      device.Get(), D3D12_HEAP_TYPE_DEFAULT, regions * threads * sizeof(UINT), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
      D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
  );
  auto readback =
      buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, regions * threads * sizeof(UINT), D3D12_RESOURCE_STATE_COPY_DEST);
  auto arguments = buffer(device.Get(), D3D12_HEAP_TYPE_UPLOAD, 256, D3D12_RESOURCE_STATE_GENERIC_READ);
  if (!expect(input && scales && output && readback && arguments, "the buffers are refused"))
    return verdict();
  CHECK(forget(readback.Get()));
  UINT *mapped;
  CHECK(input->Map(0, nullptr, (void **)&mapped));
  for (UINT i = 0; i < threads; i++)
    mapped[i] = i * 7 + 3;
  input->Unmap(0, nullptr);
  CHECK(scales->Map(0, nullptr, (void **)&mapped));
  mapped[0] = 3, mapped[64] = 5;
  scales->Unmap(0, nullptr);

  // ExecuteIndirect: the region constant, then a dispatch, per command
  struct Command {
    UINT region;
    D3D12_DISPATCH_ARGUMENTS dispatch;
  } commands[] = {{2, {threads / 64, 1, 1}}, {3, {threads / 64, 1, 1}}};
  CHECK(arguments->Map(0, nullptr, (void **)&mapped));
  memcpy(mapped, commands, sizeof(commands));
  arguments->Unmap(0, nullptr);
  D3D12_INDIRECT_ARGUMENT_DESC indirect[2]{};
  indirect[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
  indirect[0].Constant = {0, 2, 1};
  indirect[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
  D3D12_COMMAND_SIGNATURE_DESC signature_desc{sizeof(Command), 2, indirect};
  ComPtr<ID3D12CommandSignature> signature;
  CHECK(device->CreateCommandSignature(&signature_desc, rs.Get(), IID_PPV_ARGS(&signature)));

  // what each region should hold: scale, offset, step, or nothing
  struct Want {
    UINT scale, offset, step;
    bool written;
  } want[regions]{};
  auto constants = [&](UINT offset, UINT step, UINT region) {
    const UINT values[] = {offset, step, region};
    list->SetComputeRoot32BitConstants(0, 3, values, 0);
  };
  list->SetComputeRootSignature(rs.Get());
  list->SetComputeRootConstantBufferView(1, scales->GetGPUVirtualAddress());
  list->SetComputeRootShaderResourceView(2, input->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(3, output->GetGPUVirtualAddress());
  list->SetPipelineState(pipeline.Get());
  constants(11, 1, 0);
  list->Dispatch(threads / 64, 1, 1);
  want[0] = {3, 11, 1, true};
  list->SetPipelineState(other_pipeline.Get());
  list->Dispatch(threads / 64, 1, 1);
  want[6] = {3, 11, 1, true};
  list->SetPipelineState(pipeline.Get());
  // airconv's DXBC kernel, then the converter's again with the root arguments as they were
  constants(13, 2, 1);
  list->SetPipelineState(fxc_pipeline.Get());
  list->Dispatch(threads / 64, 1, 1);
  want[9] = {3, 13, 2, true};
  list->SetPipelineState(pipeline.Get());
  list->Dispatch(threads / 64, 1, 1);
  want[1] = {3, 13, 2, true};
  // regions 2 and 3 from the commands, which keep offset and step
  list->ExecuteIndirect(signature.Get(), 2, arguments.Get(), 0, nullptr, 0);
  want[2] = want[3] = {3, 13, 2, true};
  // the pipeline library's, with another scale
  list->SetPipelineState(loaded.Get());
  list->SetComputeRootConstantBufferView(1, scales->GetGPUVirtualAddress() + 256);
  constants(17, 3, 4);
  list->Dispatch(threads / 64, 1, 1);
  want[4] = {5, 17, 3, true};
  if (atomic_pipeline) {
    list->SetPipelineState(atomic_pipeline.Get());
    constants(23, 5, 0);
    list->Dispatch(threads / 2 / 64, 1, 1);
    want[11] = {5, 23, 5, true};
  }
  // the root signature only airconv lays out: the scale is the constants after the three
  list->SetComputeRootSignature(packed_rs.Get());
  list->SetPipelineState(packed_pipeline.Get());
  constants(19, 4, 0);
  const UINT scale[] = {9, 0, 0, 0};
  list->SetComputeRoot32BitConstants(1, 4, scale, 0);
  list->SetComputeRootShaderResourceView(2, input->GetGPUVirtualAddress());
  list->SetComputeRootUnorderedAccessView(3, output->GetGPUVirtualAddress());
  list->Dispatch(threads / 64, 1, 1);
  want[10] = {9, 19, 4, true};
  transition(list.Get(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->CopyResource(readback.Get(), output.Get());
  CHECK(list->Close());
  CHECK(execute(device.Get(), queue.Get(), list.Get()));

  CHECK(readback->Map(0, nullptr, (void **)&mapped));
  std::vector<UINT> results(mapped, mapped + regions * threads);
  readback->Unmap(0, nullptr);
  UINT wrong = 0;
  for (UINT r = 0; r < regions; r++)
    for (UINT id = 0; id < threads; id++) {
      UINT expect_value = want[r].written ? expected(id, want[r].scale, want[r].offset, want[r].step) : 0;
      if (results[r * threads + id] != expect_value && !wrong++)
        printf("region %u thread %u holds %u, want %u\n", r, id, results[r * threads + id], expect_value);
    }
  expect(!wrong, "%u wrong values", wrong);
  std::ofstream file(argv[3], std::ios::binary);
  file.write(reinterpret_cast<const char *>(results.data()), results.size() * sizeof(results[0]));
  expect(bool(file), "the results were not written");
  return verdict();
}

// a process with an empty store, then one on the store it left, each with the converter on
static int
processes(int argc, char **argv) {
  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  GetModuleFileNameA(nullptr, exe, sizeof(exe));
  if (!expect(temporary_directory(temporary, "msc", directory), "no temporary directory"))
    return verdict();
  auto unix_name = reinterpret_cast<char *(__cdecl *)(const WCHAR *)>(
      GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name")
  );
  auto spawn = reinterpret_cast<LONG(WINAPI *)(char *const *, int)>(
      GetProcAddress(GetModuleHandleA("ntdll.dll"), "__wine_unix_spawnvp")
  );
  std::wstring loader(GetEnvironmentVariableW(L"WINELOADER", nullptr, 0), L'\0');
  if (!expect(
          unix_name && spawn && !loader.empty() &&
              GetEnvironmentVariableW(L"WINELOADER", loader.data(), loader.size()),
          "Wine's loader, path conversion or Unix process launch is unavailable"
      ))
    return verdict();
  if (loader.starts_with(L"\\??\\"))
    loader.replace(0, std::size(L"\\??\\") - 1, L"\\\\?\\");
  std::wstring wide(std::size(directory), L'\0');
  MultiByteToWideChar(CP_ACP, 0, directory, -1, wide.data(), wide.size());
  auto path = unix_name(wide.c_str());
  auto loader_path = unix_name(loader.c_str());
  if (!expect(path && loader_path, "the Unix paths are unavailable"))
    return verdict();
  std::string store = std::string("MULLION_SHADER_CACHE_PATH=") + path;
  std::string reference;
  for (const char *mode : {"cold", "warm"}) {
    step("process %s, store %s", mode, directory);
    std::string output = std::string(directory) + "\\" + mode + ".bin";
    const char *args[] = {
        "env", store.c_str(), "MULLION_SHADER_CACHE=1", "MULLION_MSC=1", loader_path, exe,
        argc > 1 ? argv[1] : "dxil", mode, output.c_str(), nullptr
    };
    auto status = spawn(const_cast<char *const *>(args), TRUE);
    expect(status == 0, "child ended with %ld", status);
    std::ifstream file(output, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(file), {}};
    expect(!bytes.empty(), "no results");
    if (reference.empty())
      reference = bytes;
    else
      expect(bytes == reference, "the warm process wrote other values than the cold one");
  }
  return verdict();
}
