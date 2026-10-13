// contract: valid views select the requested mip and slices, and all accesses
// and clears use that selection. D3D11.3 5.2.1: "Out of range behaviors are
// also always with respect to the view properties where views are used."
// 5.2.3.1: "The full extent of the resource view is always cleared.  Viewport
// and scissor are not applied."
// 22.4.7: "ld2dms operates identically to ld except on 2D multsample resources
// with one or more samples". 22.4.21: "Returns the number of samples for the
// given resource or the rasterizer."
// 22.4.8: "Out of bounds addressing is the same as the ld instruction."
// 22.4.6: "ld returns 0 in all non-missing components of the format of the SrcResource".
// 22.4.9: "Out of bounds addressing means nothing gets written to memory."
// https://microsoft.github.io/DirectX-Specs/d3d/archive/D3D11_3_FunctionalSpec.htm
// D3D11_TEX3D_RTV, WSize: "A value of -1 indicates all of the slices along the
// w axis, starting from FirstWSlice."
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/ns-d3d11-d3d11_tex3d_rtv
// Wine's test_create_shader_resource_view (16), test_create_depthstencil_view
// (20), and test_create_unordered_access_view (24) record Windows accepting
// these sample-count-one and partial-W views. Every valid W interval at every
// mip is generated; words outside it retain their initial value. The first
// out-of-view shader coordinate reads zero and cannot write, even when it is
// inside the underlying texture.
// the read precedes the store because 22.19.2 permits "the compiler or drivers
// can optimize away the read from memory" after a store, even outside the view.
// Direct3D 11 Return Codes: E_INVALIDARG means "An invalid parameter was passed
// to the returning function."
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/d3d11-graphics-reference-returnvalues
#include "d3d11_test.hpp"
#include <d3d10_1.h>
#include <algorithm>
#include <bit>
#include <iterator>
#include <map>

int
main() {
  std::map<std::string, std::pair<unsigned, std::string>> failures;
  auto expect = [&](bool ok, const char *format, auto... args) {
    if (ok)
      return true;
    trace::wrong++;
    auto &[count, first] = failures[format];
    if (!count++) {
      char message[sizeof(trace::doing)];
      if constexpr (sizeof...(args))
        snprintf(message, sizeof(message), format, args...);
      else
        snprintf(message, sizeof(message), "%s", format);
      first = std::string(message) + " (while: " + trace::doing + ")";
    }
    return false;
  };
  const UINT width = 9, height = 5, depth = 9, word_bytes = sizeof(UINT);
  const UINT unwritten[] = {~0u, ~0u, ~0u, ~0u};
  const UINT mips = std::bit_width(std::max({width, height, depth}));
  const UINT mips_2d = std::bit_width(std::max(width, height));
  const char hlsl[] = R"(
RWTexture3D<uint> volume : register(u0);
RWBuffer<uint> result : register(u1);
[numthreads(1, 1, 1)] void cs() {
  uint width, height, depth;
  volume.GetDimensions(width, height, depth);
  result[0] = width; result[1] = height; result[2] = depth;
  result[3] = volume[uint3(0, 0, depth)];
#if !READONLY
  volume[uint3(0, 0, depth)] = 99;
  for (uint z = 0; z < depth; ++z) volume[uint3(0, 0, z)] = z + 1;
#endif
}
[numthreads(1, 1, 1)] void write_only(uint3 id : SV_DispatchThreadID) {
  volume[uint3(0, 0, id.x)] = id.x + 1;
}
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4((id & 1) * 2.0 - 1.0, (id >> 1) * 2.0 - 1.0, 0, 1);
}
float4 ps() : SV_Target { return 7; }
)";
  auto cs_code = compile(hlsl, "cs", "cs"), vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps");
  auto read_code = compile(hlsl, "cs", "cs", {"READONLY=1"});
  auto write_code = compile(hlsl, "write_only", "cs");
  const char pixel_hlsl[] = R"(
RWTexture3D<uint> volume : register(u1);
RWBuffer<uint> result : register(u2);
float4 ps() : SV_Target {
  uint width, height, depth;
  volume.GetDimensions(width, height, depth);
  result[0] = width; result[1] = height; result[2] = depth;
  result[3] = volume[uint3(0, 0, depth)];
#if !READONLY
  volume[uint3(0, 0, depth)] = 99;
  for (uint z = 0; z < depth; ++z) volume[uint3(0, 0, z)] = z + 1;
#endif
  return 7;
}
float4 write_only() : SV_Target {
  for (uint z = 0; z < DEPTH; ++z) volume[uint3(0, 0, z)] = z + 1;
  return 7;
}
)";
  const std::string depth_define = "DEPTH=" + std::to_string(depth + 1);
  auto pixel_code = compile(pixel_hlsl, "ps", "ps", {depth_define});
  auto pixel_read_code = compile(pixel_hlsl, "ps", "ps", {depth_define, "READONLY=1"});
  auto pixel_write_code = compile(pixel_hlsl, "write_only", "ps", {depth_define});
  const char ms_hlsl[] = R"(
#if ARRAY
Texture2DMSArray<float> source : register(t0);
#else
Texture2DMS<float> source : register(t0);
#endif
RWBuffer<uint> result : register(u0);
[numthreads(1, 1, 1)] void cs() {
  uint width, height, samples, arrays = 1;
#if ARRAY
  source.GetDimensions(width, height, arrays, samples);
  float value = source.Load(int3(width - 1, height - 1, arrays - 1), samples - 1);
#else
  source.GetDimensions(width, height, samples);
  float value = source.Load(int2(width - 1, height - 1), samples - 1);
#endif
  result[0] = width; result[1] = height; result[2] = arrays;
  result[3] = samples; result[4] = asuint(value);
}
)";
  auto ms_code = compile(ms_hlsl, "cs", "cs", {"ARRAY=0"});
  auto ms_array_code = compile(ms_hlsl, "cs", "cs", {"ARRAY=1"});
  const char plane_hlsl[] = R"(
#if ARRAY
RWTexture2DArray<uint> plane : register(u0);
#else
RWTexture2D<uint> plane : register(u0);
#endif
RWBuffer<uint> result : register(u1);
[numthreads(1, 1, 1)] void cs() {
  uint width, height, arrays = 1;
#if ARRAY
  plane.GetDimensions(width, height, arrays);
  for (uint z = 0; z < arrays; ++z) plane[uint3(0, 0, z)] = z + 1;
#else
  plane.GetDimensions(width, height);
  plane[uint2(0, 0)] = 1;
#endif
  result[0] = width; result[1] = height; result[2] = arrays; result[3] = 0;
}
)";
  auto plane_code = compile(plane_hlsl, "cs", "cs", {"ARRAY=0"});
  auto plane_array_code = compile(plane_hlsl, "cs", "cs", {"ARRAY=1"});
  if (!cs_code || !read_code || !write_code || !vs_code || !ps_code || !pixel_code || !pixel_read_code ||
      !pixel_write_code || !ms_code || !ms_array_code || !plane_code || !plane_array_code)
    return 1;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(
      nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr, &context
  ));
  auto create10 = reinterpret_cast<decltype(&D3D10CreateDevice1)>(
      (void *)GetProcAddress(LoadLibraryA("d3d10_1.dll"), "D3D10CreateDevice1")
  );
  if (!create10) {
    printf("failed: no D3D10CreateDevice1 in d3d10_1.dll\n");
    return 1;
  }
  ComPtr<ID3D10Device1> device10;
  CHECK(create10(
      nullptr, D3D10_DRIVER_TYPE_HARDWARE, nullptr, 0, D3D10_FEATURE_LEVEL_10_1, D3D10_1_SDK_VERSION, &device10
  ));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11ComputeShader> cs, read_only, write_only;
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps, pixel_uav, pixel_read_only, pixel_write_only;
  CHECK(device->CreateComputeShader(cs_code->GetBufferPointer(), cs_code->GetBufferSize(), nullptr, &cs));
  CHECK(device->CreateComputeShader(read_code->GetBufferPointer(), read_code->GetBufferSize(), nullptr, &read_only));
  CHECK(device->CreateComputeShader(write_code->GetBufferPointer(), write_code->GetBufferSize(), nullptr, &write_only));
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  CHECK(device->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr, &pixel_uav));
  CHECK(device->CreatePixelShader(
      pixel_read_code->GetBufferPointer(), pixel_read_code->GetBufferSize(), nullptr, &pixel_read_only
  ));
  CHECK(device->CreatePixelShader(
      pixel_write_code->GetBufferPointer(), pixel_write_code->GetBufferSize(), nullptr, &pixel_write_only
  ));
  ComPtr<ID3D11RasterizerState> rasterizer;
  D3D11_RASTERIZER_DESC rasterizer_desc{};
  rasterizer_desc.FillMode = D3D11_FILL_SOLID;
  rasterizer_desc.CullMode = D3D11_CULL_NONE;
  rasterizer_desc.DepthClipEnable = TRUE;
  CHECK(device->CreateRasterizerState(&rasterizer_desc, &rasterizer));
  ComPtr<ID3D11ComputeShader> ms, ms_array;
  CHECK(device->CreateComputeShader(ms_code->GetBufferPointer(), ms_code->GetBufferSize(), nullptr, &ms));
  CHECK(
      device->CreateComputeShader(ms_array_code->GetBufferPointer(), ms_array_code->GetBufferSize(), nullptr, &ms_array)
  );
  D3D11_BUFFER_DESC ms_result_desc{5 * word_bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS};
  ComPtr<ID3D11Buffer> ms_result;
  CHECK(device->CreateBuffer(&ms_result_desc, nullptr, &ms_result));
  ComPtr<ID3D11UnorderedAccessView> ms_result_view;
  D3D11_UNORDERED_ACCESS_VIEW_DESC ms_result_view_desc{};
  ms_result_view_desc.Format = DXGI_FORMAT_R32_UINT;
  ms_result_view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  ms_result_view_desc.Buffer.NumElements = ms_result_desc.ByteWidth / word_bytes;
  CHECK(device->CreateUnorderedAccessView(ms_result.Get(), &ms_result_view_desc, &ms_result_view));

  for (bool depth_view : {false, true}) {
    for (UINT arrays : {1u, 3u}) {
      D3D11_TEXTURE1D_DESC desc{
          width,
          1,
          arrays,
          depth_view ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32_FLOAT,
          D3D11_USAGE_DEFAULT,
          UINT(depth_view ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET) | D3D11_BIND_SHADER_RESOURCE
      };
      ComPtr<ID3D11Texture1D> texture;
      CHECK(device->CreateTexture1D(&desc, nullptr, &texture));
      step("unknown 1D view arrays=%u depth=%u", arrays, depth_view);
      if (depth_view) {
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_D32_FLOAT;
        ComPtr<ID3D11DepthStencilView> dsv;
        HRESULT hr = device->CreateDepthStencilView(texture.Get(), &view, &dsv);
        expect(hr == E_INVALIDARG && !dsv, "unknown DSV dimension returned %#lx", hr);
      } else {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = desc.Format;
        ComPtr<ID3D11ShaderResourceView> srv;
        HRESULT hr = device->CreateShaderResourceView(texture.Get(), &view, &srv);
        expect(hr == E_INVALIDARG && !srv, "unknown SRV dimension returned %#lx", hr);
        D3D11_RENDER_TARGET_VIEW_DESC target_view{};
        target_view.Format = desc.Format;
        ComPtr<ID3D11RenderTargetView> rtv;
        hr = device->CreateRenderTargetView(texture.Get(), &target_view, &rtv);
        expect(hr == E_INVALIDARG && !rtv, "unknown RTV dimension returned %#lx", hr);
      }
      for (bool array_view : {false, true}) {
        step("invalid 1D MS view arrays=%u depth=%u array-view=%u", arrays, depth_view, array_view);
        if (depth_view) {
          D3D11_DEPTH_STENCIL_VIEW_DESC view{};
          view.Format = DXGI_FORMAT_D32_FLOAT;
          view.ViewDimension = array_view ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D11_DSV_DIMENSION_TEXTURE2DMS;
          if (array_view)
            view.Texture2DMSArray = {0, arrays};
          ComPtr<ID3D11DepthStencilView> dsv;
          HRESULT hr = device->CreateDepthStencilView(texture.Get(), &view, &dsv);
          expect(hr == E_INVALIDARG && !dsv, "1D MS DSV returned %#lx", hr);
        } else {
          D3D11_SHADER_RESOURCE_VIEW_DESC view{};
          view.Format = desc.Format;
          view.ViewDimension = array_view ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
          if (array_view)
            view.Texture2DMSArray = {0, arrays};
          ComPtr<ID3D11ShaderResourceView> srv;
          HRESULT hr = device->CreateShaderResourceView(texture.Get(), &view, &srv);
          expect(hr == E_INVALIDARG && !srv, "1D MS SRV returned %#lx", hr);
          D3D11_RENDER_TARGET_VIEW_DESC target_view{};
          target_view.Format = desc.Format;
          target_view.ViewDimension =
              array_view ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DMS;
          if (array_view)
            target_view.Texture2DMSArray = {0, arrays};
          ComPtr<ID3D11RenderTargetView> rtv;
          hr = device->CreateRenderTargetView(texture.Get(), &target_view, &rtv);
          expect(hr == E_INVALIDARG && !rtv, "1D MS RTV returned %#lx", hr);
        }
      }
    }
  }

  for (UINT arrays : {1u, 3u}) {
    step("unknown dynamic SRV dimension arrays=%u", arrays);
    D3D11_TEXTURE1D_DESC desc{width,
                              1,
                              arrays,
                              DXGI_FORMAT_R32_FLOAT,
                              D3D11_USAGE_DYNAMIC,
                              D3D11_BIND_SHADER_RESOURCE,
                              D3D11_CPU_ACCESS_WRITE};
    ComPtr<ID3D11Texture1D> texture;
    CHECK(device->CreateTexture1D(&desc, nullptr, &texture));
    D3D11_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = desc.Format;
    ComPtr<ID3D11ShaderResourceView> srv;
    HRESULT hr = device->CreateShaderResourceView(texture.Get(), &view, &srv);
    expect(hr == E_INVALIDARG && !srv, "unknown dynamic SRV dimension returned %#lx", hr);
    for (bool array_view : {false, true}) {
      step("invalid dynamic 1D MS view arrays=%u array-view=%u", arrays, array_view);
      view.ViewDimension = array_view ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
      if (array_view)
        view.Texture2DMSArray = {0, arrays};
      hr = device->CreateShaderResourceView(texture.Get(), &view, &srv);
      expect(hr == E_INVALIDARG && !srv, "dynamic 1D MS SRV returned %#lx", hr);
    }
  }

  for (UINT samples = 1; samples <= D3D11_MAX_MULTISAMPLE_SAMPLE_COUNT; samples *= 2) {
    for (bool depth : {false, true}) {
      const DXGI_FORMAT format = depth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_R32_FLOAT;
      UINT quality = 0;
      CHECK(device->CheckMultisampleQualityLevels(format, samples, &quality));
      if (!quality) {
        expect(samples > 1, "format %u has no sample-count-one support", format);
        step("format %u samples %u unsupported", format, samples);
        continue;
      }
      for (UINT arrays : {1u, 3u}) {
        for (UINT mip_count : {1u, samples == 1 ? mips_2d : 1u}) {
          D3D11_TEXTURE2D_DESC desc{
              width,
              height,
              mip_count,
              arrays,
              depth ? DXGI_FORMAT_R32_TYPELESS : format,
              {samples, 0},
              D3D11_USAGE_DEFAULT,
              UINT(depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET) | D3D11_BIND_SHADER_RESOURCE
          };
          ComPtr<ID3D11Texture2D> texture;
          CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
          D3D10_TEXTURE2D_DESC desc10{
              width,
              height,
              mip_count,
              arrays,
              desc.Format,
              desc.SampleDesc,
              D3D10_USAGE_DEFAULT,
              UINT(depth ? D3D10_BIND_DEPTH_STENCIL : D3D10_BIND_RENDER_TARGET) | D3D10_BIND_SHADER_RESOURCE
          };
          ComPtr<ID3D10Texture2D> texture10;
          CHECK(device10->CreateTexture2D(&desc10, nullptr, &texture10));
          for (UINT first : {0u, arrays, ~0u}) {
            for (UINT count : {0u, arrays + 1, ~0u}) {
              if (first == 0 && count == ~0u)
                continue;
              step(
                  "invalid MS array samples=%u arrays=%u depth=%u first=%u "
                  "count=%u",
                  samples, arrays, depth, first, count
              );
              if (depth) {
                D3D11_DEPTH_STENCIL_VIEW_DESC view{};
                view.Format = format;
                view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY;
                view.Texture2DMSArray = {first, count};
                ComPtr<ID3D11DepthStencilView> dsv;
                HRESULT hr = device->CreateDepthStencilView(texture.Get(), &view, &dsv);
                expect(hr == E_INVALIDARG && !dsv, "invalid MS-array DSV returned %#lx", hr);
              } else {
                D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
                srv_desc.Format = format;
                srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY;
                srv_desc.Texture2DMSArray = {first, count};
                ComPtr<ID3D11ShaderResourceView> srv;
                HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, &srv);
                expect(hr == E_INVALIDARG && !srv, "invalid MS-array SRV returned %#lx", hr);
                D3D11_RENDER_TARGET_VIEW_DESC view{};
                view.Format = format;
                view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
                view.Texture2DMSArray = {first, count};
                ComPtr<ID3D11RenderTargetView> rtv;
                hr = device->CreateRenderTargetView(texture.Get(), &view, &rtv);
                expect(hr == E_INVALIDARG && !rtv, "invalid MS-array RTV returned %#lx", hr);
              }
            }
          }
          for (bool array_view : {false, true}) {
            if (!array_view && arrays > 1 && samples > 1)
              continue;
            for (UINT first = 0; first < (array_view ? arrays : 1); first++) {
              for (UINT requested : {1u, arrays - first, ~0u}) {
                if (!array_view && requested != 1)
                  continue;
                const UINT count = requested == ~0u ? arrays - first : requested;
                step(
                    "2D samples=%u mips=%u arrays=%u depth=%u array-view=%u "
                    "first=%u count=%u",
                    samples, mip_count, arrays, depth, array_view, first, requested
                );
                std::vector<UINT> zero(width * height);
                if (samples == 1)
                  for (UINT slice = 0; slice < arrays; slice++)
                    for (UINT mip = 0; mip < mip_count; mip++)
                      context->UpdateSubresource(
                          texture.Get(), D3D11CalcSubresource(mip, slice, mip_count), nullptr, zero.data(),
                          std::max(1u, width >> mip) * word_bytes, 0
                      );
                if (!depth) {
                  D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
                  srv_desc.Format = format;
                  srv_desc.ViewDimension =
                      array_view ? D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D11_SRV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    srv_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D11ShaderResourceView> srv;
                  HRESULT hr = device->CreateShaderResourceView(texture.Get(), &srv_desc, &srv);
                  expect(hr == S_OK && srv, "SRV returned %#lx", hr);
                  if (srv) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC actual{};
                    srv->GetDesc(&actual);
                    expect(
                        actual.Format == format && actual.ViewDimension == srv_desc.ViewDimension &&
                            (!array_view || (actual.Texture2DMSArray.FirstArraySlice == first &&
                                             actual.Texture2DMSArray.ArraySize == count)),
                        "SRV description differs"
                    );
                  }
                  D3D10_SHADER_RESOURCE_VIEW_DESC srv10_desc{};
                  srv10_desc.Format = format;
                  srv10_desc.ViewDimension =
                      array_view ? D3D10_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D10_SRV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    srv10_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D10ShaderResourceView> srv10;
                  hr = device10->CreateShaderResourceView(texture10.Get(), &srv10_desc, &srv10);
                  expect(hr == S_OK && srv10, "D3D10 SRV returned %#lx", hr);
                  D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
                  rtv_desc.Format = format;
                  rtv_desc.ViewDimension =
                      array_view ? D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D11_RTV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    rtv_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D11RenderTargetView> rtv;
                  hr = device->CreateRenderTargetView(texture.Get(), &rtv_desc, &rtv);
                  expect(hr == S_OK && rtv, "RTV returned %#lx", hr);
                  if (rtv) {
                    D3D11_RENDER_TARGET_VIEW_DESC actual{};
                    rtv->GetDesc(&actual);
                    expect(
                        actual.Format == format && actual.ViewDimension == rtv_desc.ViewDimension &&
                            (!array_view || (actual.Texture2DMSArray.FirstArraySlice == first &&
                                             actual.Texture2DMSArray.ArraySize == count)),
                        "RTV description differs"
                    );
                    const float clear[] = {3, 3, 3, 3};
                    context->ClearRenderTargetView(rtv.Get(), clear);
                  }
                  D3D10_RENDER_TARGET_VIEW_DESC rtv10_desc{};
                  rtv10_desc.Format = format;
                  rtv10_desc.ViewDimension =
                      array_view ? D3D10_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D10_RTV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    rtv10_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D10RenderTargetView> rtv10;
                  hr = device10->CreateRenderTargetView(texture10.Get(), &rtv10_desc, &rtv10);
                  expect(hr == S_OK && rtv10, "D3D10 RTV returned %#lx", hr);
                  if (srv && rtv) {
                    context->ClearUnorderedAccessViewUint(ms_result_view.Get(), unwritten);
                    context->CSSetShader(array_view ? ms_array.Get() : ms.Get(), nullptr, 0);
                    ID3D11ShaderResourceView *source = srv.Get();
                    context->CSSetShaderResources(0, 1, &source);
                    ID3D11UnorderedAccessView *target = ms_result_view.Get();
                    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
                    context->Dispatch(1, 1, 1);
                    auto words = read(device.Get(), context.Get(), ms_result.Get());
                    const float clear = 3;
                    UINT bits;
                    memcpy(&bits, &clear, sizeof(bits));
                    const UINT expected[] = {width, height, count, samples, bits};
                    if (expect(words.size() == std::size(expected), "MS readback has %zu words", words.size()))
                      for (UINT i = 0; i < std::size(expected); i++)
                        expect(words[i] == expected[i], "MS word=%u got=%#x expected=%#x", i, words[i], expected[i]);
                    context->ClearState();
                  }
                } else {
                  D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
                  dsv_desc.Format = format;
                  dsv_desc.ViewDimension =
                      array_view ? D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D11_DSV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    dsv_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D11DepthStencilView> dsv;
                  HRESULT hr = device->CreateDepthStencilView(texture.Get(), &dsv_desc, &dsv);
                  expect(hr == S_OK && dsv, "DSV returned %#lx", hr);
                  if (dsv) {
                    D3D11_DEPTH_STENCIL_VIEW_DESC actual{};
                    dsv->GetDesc(&actual);
                    expect(
                        actual.Format == format && actual.Flags == 0 &&
                            actual.ViewDimension == dsv_desc.ViewDimension &&
                            (!array_view || (actual.Texture2DMSArray.FirstArraySlice == first &&
                                             actual.Texture2DMSArray.ArraySize == count)),
                        "DSV description differs"
                    );
                    context->ClearDepthStencilView(dsv.Get(), D3D11_CLEAR_DEPTH, 0.5f, 0);
                  }
                  D3D10_DEPTH_STENCIL_VIEW_DESC dsv10_desc{};
                  dsv10_desc.Format = format;
                  dsv10_desc.ViewDimension =
                      array_view ? D3D10_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D10_DSV_DIMENSION_TEXTURE2DMS;
                  if (array_view)
                    dsv10_desc.Texture2DMSArray = {first, requested};
                  ComPtr<ID3D10DepthStencilView> dsv10;
                  hr = device10->CreateDepthStencilView(texture10.Get(), &dsv10_desc, &dsv10);
                  expect(hr == S_OK && dsv10, "D3D10 DSV returned %#lx", hr);
                }
                if (samples == 1) {
                  auto staging_desc = desc;
                  staging_desc.Usage = D3D11_USAGE_STAGING;
                  staging_desc.BindFlags = 0;
                  staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                  ComPtr<ID3D11Texture2D> staging;
                  CHECK(device->CreateTexture2D(&staging_desc, nullptr, &staging));
                  context->CopyResource(staging.Get(), texture.Get());
                  for (UINT slice = 0; slice < arrays; slice++) {
                    for (UINT mip = 0; mip < mip_count; mip++) {
                      const UINT subresource = D3D11CalcSubresource(mip, slice, mip_count);
                      D3D11_MAPPED_SUBRESOURCE mapped{};
                      CHECK(context->Map(staging.Get(), subresource, D3D11_MAP_READ, 0, &mapped));
                      const float wanted =
                          mip == 0 && slice >= first && slice - first < count ? (depth ? 0.5f : 3.0f) : 0;
                      for (UINT y = 0; y < std::max(1u, height >> mip); y++) {
                        for (UINT x = 0; x < std::max(1u, width >> mip); x++) {
                          float actual;
                          memcpy(
                              &actual, (char *)mapped.pData + y * mapped.RowPitch + x * sizeof(float), sizeof(actual)
                          );
                          expect(
                              actual == wanted, "2D mip=%u slice=%u xy=%u,%u got=%g expected=%g", mip, slice, x, y,
                              actual, wanted
                          );
                        }
                      }
                      context->Unmap(staging.Get(), subresource);
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  const UINT initial_value = 17;
  ComPtr<ID3D11Buffer> result;
  D3D11_BUFFER_DESC result_desc{4 * word_bytes, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS};
  CHECK(device->CreateBuffer(&result_desc, nullptr, &result));
  ComPtr<ID3D11UnorderedAccessView> result_view;
  D3D11_UNORDERED_ACCESS_VIEW_DESC result_view_desc{};
  result_view_desc.Format = DXGI_FORMAT_R32_UINT;
  result_view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  result_view_desc.Buffer.NumElements = result_desc.ByteWidth / word_bytes;
  CHECK(device->CreateUnorderedAccessView(result.Get(), &result_view_desc, &result_view));
  ComPtr<ID3D11ComputeShader> plane, plane_array;
  CHECK(device->CreateComputeShader(plane_code->GetBufferPointer(), plane_code->GetBufferSize(), nullptr, &plane));
  CHECK(device->CreateComputeShader(
      plane_array_code->GetBufferPointer(), plane_array_code->GetBufferSize(), nullptr, &plane_array
  ));
  for (UINT arrays : {1u, 3u}) {
    step("2D UAV metadata control arrays=%u", arrays);
    D3D11_TEXTURE2D_DESC desc{
        width, height, 1, arrays, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS
    };
    std::vector<UINT> zero(width * height);
    std::vector<D3D11_SUBRESOURCE_DATA> initial(arrays, {zero.data(), width * word_bytes, 0});
    ComPtr<ID3D11Texture2D> texture, staging;
    CHECK(device->CreateTexture2D(&desc, initial.data(), &texture));
    D3D11_UNORDERED_ACCESS_VIEW_DESC unknown{};
    unknown.Format = desc.Format;
    ComPtr<ID3D11UnorderedAccessView> invalid;
    HRESULT hr = device->CreateUnorderedAccessView(texture.Get(), &unknown, &invalid);
    expect(hr == E_INVALIDARG && !invalid, "unknown UAV dimension returned %#lx", hr);
    ComPtr<ID3D11UnorderedAccessView> uav;
    CHECK(device->CreateUnorderedAccessView(texture.Get(), nullptr, &uav));
    context->ClearState();
    context->ClearUnorderedAccessViewUint(result_view.Get(), unwritten);
    context->CSSetShader(arrays > 1 ? plane_array.Get() : plane.Get(), nullptr, 0);
    ID3D11UnorderedAccessView *views[] = {uav.Get(), result_view.Get()};
    context->CSSetUnorderedAccessViews(0, 2, views, nullptr);
    context->Dispatch(1, 1, 1);
    auto words = read(device.Get(), context.Get(), result.Get());
    expect(
        words.size() == 4 && words[0] == width && words[1] == height && words[2] == arrays, "2D UAV dimensions differ"
    );
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
    context->CopyResource(staging.Get(), texture.Get());
    for (UINT slice = 0; slice < arrays; slice++) {
      D3D11_MAPPED_SUBRESOURCE mapped{};
      CHECK(context->Map(staging.Get(), slice, D3D11_MAP_READ, 0, &mapped));
      for (UINT y = 0; y < height; y++) {
        for (UINT x = 0; x < width; x++) {
          UINT actual;
          memcpy(&actual, (char *)mapped.pData + y * mapped.RowPitch + x * word_bytes, sizeof(actual));
          expect(
              actual == (x == 0 && y == 0 ? slice + 1 : 0), "2D UAV slice=%u xy=%u,%u value=%u", slice, x, y, actual
          );
        }
      }
      context->Unmap(staging.Get(), slice);
    }
  }
  ComPtr<ID3D11Texture2D> pixel_target;
  D3D11_TEXTURE2D_DESC pixel_desc{
      1, 1, 1, 1, DXGI_FORMAT_R32_FLOAT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET
  };
  CHECK(device->CreateTexture2D(&pixel_desc, nullptr, &pixel_target));
  ComPtr<ID3D11RenderTargetView> pixel_target_view;
  CHECK(device->CreateRenderTargetView(pixel_target.Get(), nullptr, &pixel_target_view));
  for (bool floating : {false, true}) {
    const DXGI_FORMAT format = floating ? DXGI_FORMAT_R32_FLOAT : DXGI_FORMAT_R32_UINT;
    std::vector<std::vector<UINT>> initial(mips);
    std::vector<D3D11_SUBRESOURCE_DATA> data(mips);
    const float initial_float = float(initial_value);
    UINT initial_bits = initial_value;
    if (floating)
      memcpy(&initial_bits, &initial_float, sizeof(initial_bits));
    for (UINT mip = 0; mip < mips; mip++) {
      const UINT w = std::max(1u, width >> mip), h = std::max(1u, height >> mip), d = std::max(1u, depth >> mip);
      initial[mip].assign(w * h * d, initial_bits);
      data[mip] = {initial[mip].data(), w * word_bytes, w * h * word_bytes};
    }
    D3D11_TEXTURE3D_DESC desc{width,
                              height,
                              depth,
                              mips,
                              format,
                              D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS};
    ComPtr<ID3D11Texture3D> texture, staging;
    CHECK(device->CreateTexture3D(&desc, data.data(), &texture));
    ComPtr<ID3D10Texture3D> texture10;
    if (floating) {
      D3D10_TEXTURE3D_DESC desc10{width, height, depth, mips, format, D3D10_USAGE_DEFAULT, D3D10_BIND_RENDER_TARGET};
      CHECK(device10->CreateTexture3D(&desc10, nullptr, &texture10));
    }
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(device->CreateTexture3D(&desc, nullptr, &staging));
    auto reset = [&] {
      context->ClearState();
      for (UINT mip = 0; mip < mips; mip++)
        context->UpdateSubresource(
            texture.Get(), mip, nullptr, data[mip].pSysMem, data[mip].SysMemPitch, data[mip].SysMemSlicePitch
        );
    };
    auto verify = [&](UINT mip, UINT first, UINT count, UINT value, bool shader, UINT draw_plane = ~0u) {
      context->CopyResource(staging.Get(), texture.Get());
      for (UINT level_index = 0; level_index < mips; level_index++) {
        const UINT w = std::max(1u, width >> level_index), h = std::max(1u, height >> level_index);
        const UINT d = std::max(1u, depth >> level_index);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (!expect(SUCCEEDED(context->Map(staging.Get(), level_index, D3D11_MAP_READ, 0, &mapped)), "readback failed"))
          continue;
        for (UINT z = 0; z < d; z++) {
          for (UINT y = 0; y < h; y++) {
            for (UINT x = 0; x < w; x++) {
              UINT wanted = initial_bits;
              const bool inside = level_index == mip && z >= first && z - first < count;
              if (inside && (!shader || (x == 0 && y == 0)))
                wanted = shader ? z - first + 1 : value;
              if (level_index == mip && z == draw_plane) {
                const float drawn = 7;
                memcpy(&wanted, &drawn, sizeof(wanted));
              }
              UINT actual;
              memcpy(
                  &actual, (char *)mapped.pData + z * mapped.DepthPitch + y * mapped.RowPitch + x * sizeof(UINT),
                  sizeof(actual)
              );
              if (!expect(
                      actual == wanted, "mip=%u xyz=%u,%u,%u got=%#x expected=%#x", level_index, x, y, z, actual, wanted
                  )) {
                context->Unmap(staging.Get(), level_index);
                return;
              }
            }
          }
        }
        context->Unmap(staging.Get(), level_index);
      }
    };
    for (UINT mip = 0; mip < mips; mip++) {
      const UINT w = std::max(1u, width >> mip), h = std::max(1u, height >> mip);
      const UINT size = std::max(1u, depth >> mip);
      for (UINT bad_mip : {mip, mips, ~0u}) {
        for (UINT bad_first : {0u, size, ~0u}) {
          for (UINT bad_size : {0u, size + 1, ~0u}) {
            if (bad_mip == mip && bad_first == 0 && bad_size == ~0u)
              continue;
            step("invalid 3D float=%u mip=%u first=%u WSize=%u", floating, bad_mip, bad_first, bad_size);
            D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
            uav_desc.Format = format;
            uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
            uav_desc.Texture3D = {bad_mip, bad_first, bad_size};
            ComPtr<ID3D11UnorderedAccessView> uav;
            HRESULT hr = device->CreateUnorderedAccessView(texture.Get(), &uav_desc, &uav);
            expect(hr == E_INVALIDARG && !uav, "invalid UAV returned %#lx", hr);
            if (floating) {
              D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
              rtv_desc.Format = format;
              rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
              rtv_desc.Texture3D = {bad_mip, bad_first, bad_size};
              ComPtr<ID3D11RenderTargetView> rtv;
              hr = device->CreateRenderTargetView(texture.Get(), &rtv_desc, &rtv);
              expect(hr == E_INVALIDARG && !rtv, "invalid RTV returned %#lx", hr);
            }
          }
        }
      }
      for (UINT first = 0; first < size; first++) {
        for (UINT requested = 1; requested <= size - first + 1; requested++) {
          const UINT wsize = requested == size - first + 1 ? ~0u : requested;
          const UINT count = wsize == ~0u ? size - first : wsize;
          step("3D float=%u mip=%u first=%u WSize=%u", floating, mip, first, wsize);
          D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};
          uav_desc.Format = format;
          uav_desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
          uav_desc.Texture3D = {mip, first, wsize};
          ComPtr<ID3D11UnorderedAccessView> uav;
          HRESULT hr = device->CreateUnorderedAccessView(texture.Get(), &uav_desc, &uav);
          expect(hr == S_OK && uav, "3D UAV returned %#lx", hr);
          if (uav) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC actual{};
            uav->GetDesc(&actual);
            expect(
                actual.Texture3D.MipSlice == mip && actual.Texture3D.FirstWSlice == first &&
                    actual.Texture3D.WSize == count,
                "UAV W range differs"
            );
            for (bool raw : {false, true}) {
              if (!floating && !raw)
                continue;
              reset();
              UINT value = 5;
              if (raw) {
                const UINT clear[] = {5, 5, 5, 5};
                context->ClearUnorderedAccessViewUint(uav.Get(), clear);
              } else {
                const float clear[] = {5, 5, 5, 5};
                memcpy(&value, clear, sizeof(value));
                context->ClearUnorderedAccessViewFloat(uav.Get(), clear);
              }
              step("3D clear float=%u raw=%u mip=%u first=%u count=%u", floating, raw, mip, first, count);
              verify(mip, first, count, value, false);
            }
            if (!floating) {
              for (bool pixel : {false, true}) {
                for (UINT variant = 0; variant < 5; variant++) {
                  step("3D shader mip=%u first=%u count=%u pixel=%u variant=%u", mip, first, count, pixel, variant);
                  reset();
                  context->ClearUnorderedAccessViewUint(result_view.Get(), unwritten);
                  auto run = variant == 1 ? deferred.Get() : context.Get();
                  run->ClearState();
                  ID3D11UnorderedAccessView *views[] = {uav.Get(), result_view.Get()};
                  if (pixel) {
                    run->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
                    run->VSSetShader(vs.Get(), nullptr, 0);
                    run->PSSetShader(
                        variant == 3   ? pixel_read_only.Get()
                        : variant == 4 ? pixel_write_only.Get()
                                       : pixel_uav.Get(),
                        nullptr, 0
                    );
                    run->RSSetState(rasterizer.Get());
                    D3D11_VIEWPORT viewport{0, 0, 1, 1, 0, 1};
                    run->RSSetViewports(1, &viewport);
                    ID3D11RenderTargetView *target = pixel_target_view.Get();
                    run->OMSetRenderTargetsAndUnorderedAccessViews(1, &target, nullptr, 1, 2, views, nullptr);
                  } else {
                    run->CSSetShader(
                        variant == 3   ? read_only.Get()
                        : variant == 4 ? write_only.Get()
                                       : cs.Get(),
                        nullptr, 0
                    );
                    run->CSSetUnorderedAccessViews(0, 2, views, nullptr);
                  }
                  if (variant == 2) {
                    deferred->ClearState();
                    ComPtr<ID3D11CommandList> empty;
                    CHECK(deferred->FinishCommandList(FALSE, &empty));
                    context->ExecuteCommandList(empty.Get(), TRUE);
                  }
                  if (pixel)
                    run->Draw(4, 0);
                  else
                    run->Dispatch(variant == 4 ? count + 1 : 1, 1, 1);
                  if (variant == 1) {
                    ComPtr<ID3D11CommandList> list;
                    CHECK(deferred->FinishCommandList(FALSE, &list));
                    context->ExecuteCommandList(list.Get(), TRUE);
                  }
                  if (variant != 4) {
                    auto words = read(device.Get(), context.Get(), result.Get());
                    const UINT expected[] = {w, h, count, 0};
                    if (expect(words.size() == std::size(expected), "3D readback has %zu words", words.size()))
                      for (UINT i = 0; i < std::size(expected); i++)
                        expect(words[i] == expected[i], "3D word=%u got=%#x expected=%#x", i, words[i], expected[i]);
                  }
                  verify(mip, first, count, initial_bits, variant != 3);
                }
              }
            }
          }
          if (floating) {
            D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
            rtv_desc.Format = format;
            rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE3D;
            rtv_desc.Texture3D = {mip, first, wsize};
            ComPtr<ID3D11RenderTargetView> rtv;
            hr = device->CreateRenderTargetView(texture.Get(), &rtv_desc, &rtv);
            expect(hr == S_OK && rtv, "3D RTV returned %#lx", hr);
            if (!rtv)
              continue;
            D3D11_RENDER_TARGET_VIEW_DESC actual{};
            rtv->GetDesc(&actual);
            expect(
                actual.Texture3D.MipSlice == mip && actual.Texture3D.FirstWSlice == first &&
                    actual.Texture3D.WSize == count,
                "RTV W range differs"
            );
            D3D10_RENDER_TARGET_VIEW_DESC rtv10_desc{};
            rtv10_desc.Format = format;
            rtv10_desc.ViewDimension = D3D10_RTV_DIMENSION_TEXTURE3D;
            rtv10_desc.Texture3D = {mip, first, wsize};
            ComPtr<ID3D10RenderTargetView> rtv10;
            hr = device10->CreateRenderTargetView(texture10.Get(), &rtv10_desc, &rtv10);
            expect(hr == S_OK && rtv10, "D3D10 3D RTV returned %#lx", hr);
            if (rtv10) {
              D3D10_RENDER_TARGET_VIEW_DESC actual10{};
              rtv10->GetDesc(&actual10);
              expect(
                  actual10.Texture3D.MipSlice == mip && actual10.Texture3D.FirstWSlice == first &&
                      actual10.Texture3D.WSize == count,
                  "D3D10 RTV W range differs"
              );
            }
            reset();
            const float clear[] = {5, 5, 5, 5};
            UINT clear_bits;
            memcpy(&clear_bits, clear, sizeof(clear_bits));
            context->ClearRenderTargetView(rtv.Get(), clear);
            verify(mip, first, count, clear_bits, false);
            context->ClearRenderTargetView(rtv.Get(), clear);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            context->VSSetShader(vs.Get(), nullptr, 0);
            context->PSSetShader(ps.Get(), nullptr, 0);
            context->RSSetState(rasterizer.Get());
            D3D11_VIEWPORT viewport{0, 0, float(w), float(h), 0, 1};
            context->RSSetViewports(1, &viewport);
            const UINT draw_plane = count == 1 && size > 1 ? (first + 1) % size : first;
            rtv_desc.Texture3D = {mip, draw_plane, count};
            ComPtr<ID3D11RenderTargetView> draw_view;
            CHECK(device->CreateRenderTargetView(texture.Get(), &rtv_desc, &draw_view));
            ID3D11RenderTargetView *target = draw_view.Get();
            context->OMSetRenderTargets(1, &target, nullptr);
            context->Draw(4, 0);
            verify(mip, first, count, clear_bits, false, draw_plane);
          }
        }
      }
    }
  }
  for (const auto &[kind, failure] : failures)
    printf("wrong: %u x %s\n", failure.first, failure.second.c_str());
  return verdict();
}
