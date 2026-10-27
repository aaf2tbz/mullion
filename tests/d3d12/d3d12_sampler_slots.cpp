// contract: a GPU that runs out of samplers says so once. Metal holds maxArgumentBufferSamplerCount "unique argument
// buffer samplers per app" (Apple, MTLDevice) and the newer ones sample wrong with no error, measured on an M1 Pro
// (1024 there, as the Metal feature set tables give Apple7 and Apple8 for samplers a stage reaches from an argument
// buffer, against 500 K from Apple9). a full sampler heap (D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE) of samplers
// that differ in MinLOD by the GPU's own step of 1/64 makes past 1024 distinct Metal samplers, each with its cube
// twin, and far below 500 K. emulated as Apple7 or Apple8 (MULLION_GPU_FAMILY) the log names 1024 once; any other GPU
// says it at most once, with a limit below what was made.
#include "d3d12_test.hpp"
#include <fstream>
#include <iterator>

int
main() {
  char temporary[MAX_PATH], directory[MAX_PATH], exe[MAX_PATH], family[8] = {};
  auto length = GetTempPathA(sizeof(temporary), temporary);
  CHECK(length && length < sizeof(temporary) ? S_OK : E_FAIL);
  CHECK(temporary_directory(temporary, "smp", directory) ? S_OK : E_FAIL);
  CHECK(SetEnvironmentVariableA("MULLION_LOG_PATH", directory) ? S_OK : E_FAIL);
  CHECK(GetModuleFileNameA(nullptr, exe, sizeof(exe)) ? S_OK : E_FAIL);
  GetEnvironmentVariableA("MULLION_GPU_FAMILY", family, sizeof(family));
  const bool emulated = !strcmp(family, "7") || !strcmp(family, "8");

  step("fill a sampler heap with samplers one LOD step apart");
  ComPtr<ID3D12Device> device;
  CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
  const UINT count = D3D12_MAX_SHADER_VISIBLE_SAMPLER_HEAP_SIZE, steps = 64, lods = 14 * steps + 1;
  D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, count,
                                       D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
  ComPtr<ID3D12DescriptorHeap> heap;
  CHECK(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&heap)));
  auto handle = heap->GetCPUDescriptorHandleForHeapStart();
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
  for (UINT i = 0; i < count; i++, handle.ptr += increment) {
    D3D12_SAMPLER_DESC desc{D3D12_FILTER_MIN_MAG_MIP_POINT, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                            D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP};
    desc.MinLOD = float(i % lods) / steps;
    desc.MaxLOD = D3D12_FLOAT32_MAX;
    device->CreateSampler(&desc, handle);
  }

  step("read the log");
  const std::string name = std::string(exe).substr(std::string(exe).find_last_of("\\/") + 1);
  const std::string path = std::string(directory) + "\\" + name.substr(0, name.find_last_of('.')) + "_d3d12.log";
  std::ifstream file(path);
  const std::string lines{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  const char said[] = "Sampler: past the ";
  size_t first = lines.find(said);
  unsigned limit = first == std::string::npos ? 0 : std::stoul(lines.substr(first + sizeof(said) - 1));
  expect(first == std::string::npos || lines.find(said, first + 1) == std::string::npos, "said more than once");
  expect(emulated ? limit == 1024 : limit < 2 * lods, "family %s: the limit said is %u", family, limit);
  file.close();
  DeleteFileA(path.c_str());
  heap = nullptr;
  device = nullptr;
  RemoveDirectoryA(directory);
  return verdict();
}
