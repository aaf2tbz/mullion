// contract: DiscardResource and DiscardView leave what they discard undefined and nothing else: what the GPU wrote to
// other resources, and what the context writes to the discarded one after it, are as they would be without them
// (ID3D11DeviceContext1::DiscardResource, "the resource will be left in an undefined state"; DiscardView1 with
// rectangles leaves the rest as it was). the cases: two 4x targets drawn by one draw, one resolved and discarded, the
// other resolved after and again later; a target discarded, cleared and half drawn; one whose view is discarded in a
// rectangle, then half drawn; one discarded and drawn whole; two drawn by one draw, the first discarded, the second
// read; one discarded while bound between two draws, the second
// drawing its other half; the same through a deferred context. every texel read is the one the case wrote last; a
// discarded texel nothing wrote after is not read.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11_1/nf-d3d11_1-id3d11devicecontext1-discardresource
#include "d3d11_test.hpp"
#include <d3d11_1.h>

static constexpr UINT size = 64;

static const char hlsl[] = R"hlsl(
cbuffer Color : register(b0) { float4 color; float2 range; };
float4 vs(uint id : SV_VertexID) : SV_Position {
  return float4(id == 1 ? 3 : -1, id == 2 ? -3 : 1, 0, 1);
}
float4 ps(float4 position : SV_Position) : SV_Target0 {
  if (position.x < range.x || position.x >= range.y)
    discard;
  return color;
}
struct Two { float4 a : SV_Target0; float4 b : SV_Target1; };
Two ps_two(float4 position : SV_Position) {
  Two o;
  o.a = color;
  o.b = color.bgra;
  return o;
}
)hlsl";

static uint32_t
packed(const float c[4]) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++)
    v |= uint32_t(c[i] * 255 + 0.5f) << (8 * i);
  return v;
}

int
main() {
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps"), two_code = compile(hlsl, "ps_two", "ps");
  if (!expect(vs_code && ps_code && two_code, "HLSL did not compile"))
    return verdict();
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate0;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &immediate0));
  ComPtr<ID3D11DeviceContext1> immediate;
  CHECK(immediate0.As(&immediate));
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps, ps_two;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  CHECK(device->CreatePixelShader(two_code->GetBufferPointer(), two_code->GetBufferSize(), nullptr, &ps_two));
  auto constants = buffer(device.Get(), 32, D3D11_BIND_CONSTANT_BUFFER);

  auto target = [&](UINT samples, ComPtr<ID3D11RenderTargetView> *rtv) {
    D3D11_TEXTURE2D_DESC desc{size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {samples, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_RENDER_TARGET};
    ComPtr<ID3D11Texture2D> t;
    device->CreateTexture2D(&desc, nullptr, &t);
    if (rtv && t)
      device->CreateRenderTargetView(t.Get(), nullptr, &*rtv);
    return t;
  };
  D3D11_TEXTURE2D_DESC staging_desc{size, size, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0,
                                    D3D11_CPU_ACCESS_READ};
  ComPtr<ID3D11Texture2D> staging;
  CHECK(device->CreateTexture2D(&staging_desc, nullptr, &staging));

  const float red[4] = {1, 0, 0, 1}, green[4] = {0, 1, 0, 1}, blue[4] = {0, 0, 1, 1}, white[4] = {1, 1, 1, 1};
  const float bgra_red[4] = {0, 0, 1, 1};
  // a draw of `color` over the columns [from, to) to what is bound
  auto draw = [&](ID3D11DeviceContext *c, ID3D11PixelShader *pixel, const float color[4], float from = 0, float to = size) {
    float values[8] = {color[0], color[1], color[2], color[3], from, to};
    c->UpdateSubresource(constants.Get(), 0, nullptr, values, 0, 0);
    D3D11_VIEWPORT viewport{0, 0, float(size), float(size), 0, 1};
    c->RSSetViewports(1, &viewport);
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->VSSetShader(vs.Get(), nullptr, 0);
    c->PSSetShader(pixel, nullptr, 0);
    c->PSSetConstantBuffers(0, 1, constants.GetAddressOf());
    c->Draw(3, 0);
  };
  std::vector<uint32_t> texels;
  auto read = [&](ID3D11Texture2D *t) {
    immediate->CopyResource(staging.Get(), t);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
      return;
    texels.resize(size * size);
    for (UINT y = 0; y < size; y++)
      memcpy(&texels[y * size], (const char *)mapped.pData + y * mapped.RowPitch, size * 4);
    immediate->Unmap(staging.Get(), 0);
  };
  auto check = [&](uint32_t want, UINT from = 0, UINT to = size) {
    for (UINT y = 0; y < size; y++)
      for (UINT x = from; x < to; x++)
        if (texels[y * size + x] != want) {
          expect(false, "texel %u,%u is %08x, want %08x", x, y, texels[y * size + x], want);
          return;
        }
  };

  for (bool deferred : {false, true}) {
    ComPtr<ID3D11DeviceContext1> context = immediate;
    if (deferred) {
      ComPtr<ID3D11DeviceContext> made;
      CHECK(device->CreateDeferredContext(0, &made));
      CHECK(made.As(&context));
    }
    // runs what the case recorded: a deferred context's list on the immediate one
    auto run = [&] {
      if (!deferred)
        return;
      ComPtr<ID3D11CommandList> list;
      if (SUCCEEDED(context->FinishCommandList(FALSE, &list)))
        immediate->ExecuteCommandList(list.Get(), FALSE);
    };
    {
      step("two 4x targets of one draw, the first resolved and discarded, the second resolved after and later%s",
           deferred ? ", deferred" : "");
      ComPtr<ID3D11RenderTargetView> h[2];
      auto a = target(4, &h[0]), b = target(4, &h[1]);
      auto ra = target(1, nullptr), rb = target(1, nullptr), rb2 = target(1, nullptr);
      ID3D11RenderTargetView *both[2] = {h[0].Get(), h[1].Get()};
      context->OMSetRenderTargets(2, both, nullptr);
      draw(context.Get(), ps_two.Get(), red);
      context->ResolveSubresource(ra.Get(), 0, a.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
      context->DiscardResource(a.Get());
      context->ResolveSubresource(rb.Get(), 0, b.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
      run();
      immediate->Flush();
      immediate->ResolveSubresource(rb2.Get(), 0, b.Get(), 0, DXGI_FORMAT_R8G8B8A8_UNORM);
      read(ra.Get());
      check(packed(red));
      read(rb.Get());
      check(packed(bgra_red));
      read(rb2.Get());
      check(packed(bgra_red));
    }
    {
      step("two targets of one draw, the first discarded%s", deferred ? ", deferred" : "");
      ComPtr<ID3D11RenderTargetView> h[2];
      auto a = target(1, &h[0]), b = target(1, &h[1]);
      ID3D11RenderTargetView *both[2] = {h[0].Get(), h[1].Get()};
      context->OMSetRenderTargets(2, both, nullptr);
      draw(context.Get(), ps_two.Get(), red);
      context->OMSetRenderTargets(0, nullptr, nullptr);
      context->DiscardResource(a.Get());
      run();
      read(b.Get());
      check(packed(bgra_red));
    }
    // a target filled red, on the immediate context
    auto filled = [&](ComPtr<ID3D11RenderTargetView> &rtv) {
      auto t = target(1, &rtv);
      immediate->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      draw(immediate.Get(), ps.Get(), red);
      immediate->OMSetRenderTargets(0, nullptr, nullptr);
      return t;
    };
    {
      step("discarded, cleared, half drawn%s", deferred ? ", deferred" : "");
      ComPtr<ID3D11RenderTargetView> rtv;
      auto t = filled(rtv);
      context->DiscardResource(t.Get());
      context->ClearRenderTargetView(rtv.Get(), blue);
      context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      draw(context.Get(), ps.Get(), green, 0, size / 2);
      run();
      read(t.Get());
      check(packed(green), 0, size / 2);
      check(packed(blue), size / 2, size);
    }
    {
      step("a view discarded in a rectangle, half drawn%s", deferred ? ", deferred" : "");
      ComPtr<ID3D11RenderTargetView> rtv;
      auto t = filled(rtv);
      D3D11_RECT left{0, 0, LONG(size / 2), LONG(size)};
      context->DiscardView1(rtv.Get(), &left, 1);
      context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      draw(context.Get(), ps.Get(), green, 0, size / 2);
      run();
      read(t.Get());
      check(packed(green), 0, size / 2);
      check(packed(red), size / 2, size);
    }
    {
      step("a view discarded, drawn whole%s", deferred ? ", deferred" : "");
      ComPtr<ID3D11RenderTargetView> rtv;
      auto t = filled(rtv);
      context->DiscardView(rtv.Get());
      context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      draw(context.Get(), ps.Get(), green);
      run();
      read(t.Get());
      check(packed(green));
    }
    {
      step("discarded while bound between two draws%s", deferred ? ", deferred" : "");
      ComPtr<ID3D11RenderTargetView> rtv;
      auto t = filled(rtv);
      context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      draw(context.Get(), ps.Get(), white, 0, size / 2);
      context->DiscardResource(t.Get());
      draw(context.Get(), ps.Get(), green, size / 2, size);
      run();
      read(t.Get());
      check(packed(green), size / 2, size);
    }
  }
  return verdict();
}
