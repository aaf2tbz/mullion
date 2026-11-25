// contract: a render target made after another of the same description was released is a texture of its own: its
// views name its own subresources, and what a clear writes there is what a copy reads back, whatever views the
// released one had and whatever was drawn to it. "If you don't pass anything to pInitialData, the initial content of
// the memory for the resource is undefined" (ID3D11Device::CreateTexture2D), so only what a round writes is read.
// each round makes the texture again, makes its views in another order than the round before (a view of the last
// round's subresource order would clear the wrong one), clears every subresource to the round's own value and checks
// them all; RGBA8 targets and typeless ones with sRGB views, with mips and array layers, of more than a MiB each,
// 1024 and 512 texels wide by turns.
// https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11device-createtexture2d
#include "d3d11_test.hpp"
#include <vector>

static constexpr UINT height = 256, mips = 3, layers = 2, subs = mips * layers, rounds = 6;

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &context));
  auto value = [](UINT round, UINT sub) { return 0xff000000u | (round * 37 + 11) << 8 | (sub * 29 + 5); };
  for (UINT kind = 0; kind < 2; kind++)
    for (UINT round = 0; round < rounds; round++) {
      step("kind %u round %u", kind, round);
      const bool typeless = kind == 1;
      // two sizes, by turns: a round takes the texture of the round two before
      const UINT width = round % 2 ? 512 : 1024;
      D3D11_TEXTURE2D_DESC desc{width, height, mips, layers,
                                typeless ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0},
                                D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE};
      ComPtr<ID3D11Texture2D> texture, staging;
      CHECK(device->CreateTexture2D(&desc, nullptr, &texture));
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
      // the views, in an order that turns with the round
      std::vector<ComPtr<ID3D11RenderTargetView>> views(subs);
      for (UINT k = 0; k < subs; k++) {
        const UINT sub = (k + round) % subs, mip = sub % mips, layer = sub / mips;
        D3D11_RENDER_TARGET_VIEW_DESC view{typeless ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM,
                                           D3D11_RTV_DIMENSION_TEXTURE2DARRAY};
        view.Texture2DArray = {mip, layer, 1};
        CHECK(device->CreateRenderTargetView(texture.Get(), &view, &views[k]));
        // an sRGB view's clear is encoded: 0 and 1 encode exactly, so a typeless round clears its subresources to
        // black or white channels by the bits of the value
        const uint32_t v = value(round, sub);
        float color[4];
        for (int c = 0; c < 4; c++)
          color[c] = typeless ? float((v >> (8 * c) & 1) | (c == 3)) : float(v >> (8 * c) & 0xff) / 255.0f;
        context->ClearRenderTargetView(views[k].Get(), color);
      }
      context->CopyResource(staging.Get(), texture.Get());
      bool ok = true;
      for (UINT sub = 0; sub < subs && ok; sub++) {
        const UINT mip = sub % mips, layer = sub / mips;
        D3D11_MAPPED_SUBRESOURCE mapped;
        CHECK(context->Map(staging.Get(), D3D11CalcSubresource(mip, layer, mips), D3D11_MAP_READ, 0, &mapped));
        uint32_t want = value(round, sub);
        if (typeless) {
          uint32_t bits = want;
          want = 0;
          for (int c = 0; c < 4; c++)
            want |= uint32_t((bits >> (8 * c) & 1) | (c == 3) ? 0xff : 0) << (8 * c);
        }
        for (UINT y = 0; y < std::max(1u, height >> mip) && ok; y++)
          for (UINT x = 0; x < std::max(1u, width >> mip) && ok; x++) {
            uint32_t got;
            memcpy(&got, static_cast<const uint8_t *>(mapped.pData) + y * mapped.RowPitch + x * 4, 4);
            ok = expect(got == want, "subresource %u texel (%u,%u) is %08x, want %08x", sub, x, y, got, want);
          }
        context->Unmap(staging.Get(), D3D11CalcSubresource(mip, layer, mips));
      }
      if (!ok)
        return verdict();
    }
  return verdict();
}
