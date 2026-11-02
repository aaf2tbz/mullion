// contract: a pipeline made in one process is not compiled again in the next. Mullion keeps the GPU binaries of the
// pipelines it compiles in binary archives beside its shader cache (pipelines_<version>/chunk_<n>.metallib, saved
// whole when a device is released), and a later process making the same pipelines loads them from there: it adds
// nothing, so no chunk is written again and none begun. Either way the pipelines draw and dispatch what they write.
// with the archive turned off (MULLION_D3D12_PIPELINE_ARCHIVE=0) nothing is kept.
// the target is one row with a column per graphics pipeline: pipeline n writes its own value into column n; compute
// pipeline n writes its own value into element n of a buffer.
#include "d3d12_test.hpp"
#include <algorithm>
#include <chrono>

static const char hlsl[] = R"hlsl(
cbuffer C : register(b0) { uint column; };
float4 cover(uint id : SV_VertexID) : SV_Position {
  float2 uv = float2((id << 1) & 2, id & 2);
  return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint ps() : SV_Target { return VALUE; }
RWByteAddressBuffer written : register(u0);
[numthreads(1, 1, 1)] void cs() { written.Store(INDEX * 4, VALUE); }
)hlsl";

static const UINT graphics = 12, computes = 4;

// one process's pipelines, drawn and dispatched; the device's release saves what the archive holds
static int
child(int argc, char **argv, Compiler &compiler) {
  std::vector<std::string> pixel_shaders(graphics), compute_shaders(computes);
  auto cover = compiler.compile(hlsl, "cover", "vs", {"VALUE=0", "INDEX=0"});
  for (UINT i = 0; i < graphics; i++)
    pixel_shaders[i] = compiler.compile(hlsl, "ps", "ps", {"VALUE=" + std::to_string(2000 + i), "INDEX=0"});
  for (UINT i = 0; i < computes; i++)
    compute_shaders[i] =
        compiler.compile(hlsl, "cs", "cs", {"VALUE=" + std::to_string(3000 + i), "INDEX=" + std::to_string(i)});
  auto empty = [](auto &s) { return s.empty(); };
  if (!expect(
          !cover.empty() && std::none_of(pixel_shaders.begin(), pixel_shaders.end(), empty) &&
              std::none_of(compute_shaders.begin(), compute_shaders.end(), empty),
          "HLSL did not compile"
      ))
    return verdict();
  {
    ComPtr<ID3D12Device> device;
    CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
    D3D12_ROOT_PARAMETER params[2] = {{D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS}, {D3D12_ROOT_PARAMETER_TYPE_UAV}};
    params[0].Constants.Num32BitValues = 1;
    auto rs = root_signature(device.Get(), {2, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE});

    auto start = std::chrono::steady_clock::now();
    step("graphics pipelines");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = rs.Get();
    desc.VS = bytecode(cover);
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = ~0u;
    desc.RasterizerState = {D3D12_FILL_MODE_SOLID, D3D12_CULL_MODE_NONE};
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1, desc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    desc.SampleDesc = {1, 0};
    std::vector<ComPtr<ID3D12PipelineState>> pipelines(graphics + computes);
    for (UINT i = 0; i < graphics; i++) {
      desc.PS = bytecode(pixel_shaders[i]);
      CHECK(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipelines[i])));
    }
    step("compute pipelines");
    for (UINT i = 0; i < computes; i++) {
      D3D12_COMPUTE_PIPELINE_STATE_DESC compute{rs.Get(), bytecode(compute_shaders[i])};
      CHECK(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&pipelines[graphics + i])));
    }

    step("draws and dispatches");
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC target_desc{D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, graphics, 1, 1, 1, DXGI_FORMAT_R32_UINT,
                                    {1, 0}, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET};
    ComPtr<ID3D12Resource> target;
    CHECK(device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&target)
    ));
    auto written = buffer(device.Get(), D3D12_HEAP_TYPE_DEFAULT, 256, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    ComPtr<ID3D12DescriptorHeap> rtvs;
    D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
    CHECK(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtvs)));
    auto rtv = rtvs->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT row;
    UINT64 size;
    device->GetCopyableFootprints(&target_desc, 0, 1, 256, &row, nullptr, nullptr, &size);
    auto readback = buffer(device.Get(), D3D12_HEAP_TYPE_READBACK, 256 + size, D3D12_RESOURCE_STATE_COPY_DEST);

    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    CHECK(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)));
    CHECK(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    CHECK(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
    const float zero[4] = {};
    list->ClearRenderTargetView(rtv, zero, 0, nullptr);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->SetGraphicsRootSignature(rs.Get());
    list->SetGraphicsRootUnorderedAccessView(1, written->GetGPUVirtualAddress());
    D3D12_VIEWPORT viewport{0, 0, (float)graphics, 1, 0, 1};
    list->RSSetViewports(1, &viewport);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    for (UINT i = 0; i < graphics; i++) {
      // only its own column
      D3D12_RECT column{(LONG)i, 0, LONG(i + 1), 1};
      list->RSSetScissorRects(1, &column);
      list->SetPipelineState(pipelines[i].Get());
      list->SetGraphicsRoot32BitConstant(0, i, 0);
      list->DrawInstanced(3, 1, 0, 0);
    }
    list->SetComputeRootSignature(rs.Get());
    list->SetComputeRootUnorderedAccessView(1, written->GetGPUVirtualAddress());
    for (UINT i = 0; i < computes; i++) {
      list->SetPipelineState(pipelines[graphics + i].Get());
      list->Dispatch(1, 1, 1);
    }
    transition(list.Get(), target.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list.Get(), written.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, written.Get(), 0, computes * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    D3D12_TEXTURE_COPY_LOCATION src{target.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dst.PlacedFootprint = row;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    CHECK(submit(device.Get(), queue.Get(), list.Get()));
    printf(
        "%s: %u pipelines made and drawn in %lld ms\n", argv[2], graphics + computes,
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start)
            .count()
    );

    UINT *got;
    CHECK(readback->Map(0, nullptr, (void **)&got));
    for (UINT i = 0; i < computes; i++)
      expect(got[i] == 3000 + i, "compute pipeline %u wrote %u, want %u", i, got[i], 3000 + i);
    for (UINT i = 0; i < graphics; i++)
      expect(got[64 + i] == 2000 + i, "graphics pipeline %u wrote %u, want %u", i, got[64 + i], 2000 + i);
    readback->Unmap(0, nullptr);
  }
  return verdict();
}

// the archive's folder in `store`, if one was made
static std::string
archive_folder(const std::string &store) {
  WIN32_FIND_DATAA found;
  auto find = FindFirstFileA((store + "\\pipelines_*").c_str(), &found);
  if (find == INVALID_HANDLE_VALUE)
    return {};
  FindClose(find);
  return store + "\\" + found.cFileName;
}

struct Chunk {
  bool exists;
  ULONGLONG size;
  FILETIME written;
};

static Chunk
chunk(const std::string &folder, int n) {
  WIN32_FILE_ATTRIBUTE_DATA data;
  if (folder.empty() || !GetFileAttributesExA((folder + "\\chunk_" + std::to_string(n) + ".metallib").c_str(),
                                              GetFileExInfoStandard, &data))
    return {false};
  return {true, (ULONGLONG(data.nFileSizeHigh) << 32) | data.nFileSizeLow, data.ftLastWriteTime};
}

int
main(int argc, char **argv) {
  Compiler compiler;
  if (!front_end(argc, argv, compiler)) {
    printf("skipped: dxcompiler.dll not found\n");
    return 77;
  }
  if (argc > 2)
    return child(argc, argv, compiler);

  char temporary[MAX_PATH], exe[MAX_PATH];
  GetTempPathA(sizeof(temporary), temporary);
  GetModuleFileNameA(nullptr, exe, sizeof(exe));
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
          "Wine's path conversion, loader or Unix process launch is unavailable"
      ))
    return verdict();
  if (loader.starts_with(L"\\??\\"))
    loader.replace(0, std::size(L"\\??\\") - 1, L"\\\\?\\");
  auto loader_path = unix_name(loader.c_str());
  auto unix_path = [&](const char *path) {
    std::wstring wide(MAX_PATH, L'\0');
    MultiByteToWideChar(CP_ACP, 0, path, -1, wide.data(), wide.size());
    auto name = unix_name(wide.c_str());
    std::string result = name ? name : "";
    HeapFree(GetProcessHeap(), 0, name);
    return result;
  };
  // a process of this test making the pipelines with the store `directory`
  auto run = [&](const char *mode, const char *directory, bool archive) {
    step("process %s, store %s", mode, directory);
    std::string store = "MULLION_SHADER_CACHE_PATH=" + unix_path(directory);
    const char *args[] = {
        "env",
        "-u",
        "MULLION_SHADER_CACHE",
        "-u",
        "MULLION_D3D12_DEFER_PIPELINES",
        store.c_str(),
        archive ? "MULLION_D3D12_PIPELINE_ARCHIVE=1" : "MULLION_D3D12_PIPELINE_ARCHIVE=0",
        loader_path,
        exe,
        argc > 1 ? argv[1] : "dxbc",
        mode,
        nullptr
    };
    auto status = spawn(const_cast<char *const *>(args), TRUE);
    expect(status == 0, "%s process ended with %ld", mode, status);
  };

  char kept[MAX_PATH], off[MAX_PATH];
  if (!expect(temporary_directory(temporary, "pa", kept) && temporary_directory(temporary, "pa", off),
              "no temporary directory"))
    return verdict();

  run("first", kept, true);
  auto folder = archive_folder(kept);
  auto first = chunk(folder, 0);
  expect(first.exists && first.size > 0, "the first process saved no archive in %s", kept);

  // the same pipelines again: all of them in the saved chunk, so it is not written again and no chunk is begun
  run("second", kept, true);
  auto again = chunk(folder, 0);
  expect(again.exists && again.size == first.size && !CompareFileTime(&again.written, &first.written),
         "the second process wrote chunk 0 again (size %llu, was %llu)", again.size, first.size);
  expect(!chunk(folder, 1).exists, "the second process compiled pipelines the archive holds, and saved chunk 1");

  run("off", off, false);
  expect(archive_folder(off).empty(), "an archive was saved with MULLION_D3D12_PIPELINE_ARCHIVE=0");

  printf("archive: %s\n", folder.c_str());
  return verdict();
}
