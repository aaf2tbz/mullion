// contract: UpdateSubresource between two draws of one render pass is seen by the draws after it and by none before
// it (D3D11's immediate and deferred contexts run commands in order), whether the resource is a texture a pixel or a
// vertex shader reads or a vertex buffer, and whether a draw of the pass used it before the update or not (an upload
// into a resource no draw of the pass has used yet goes before the pass, MULLION_D3D11_HOIST_UPDATES), and so is a
// copy from a staging texture the CPU wrote, CopySubresourceRegion. each draw
// writes what it read into its own columns of a 16x1 target: the draws before an update read the value before it.
#include "d3d11_test.hpp"

static const char hlsl[] = R"hlsl(
Texture2D<uint> value : register(t0);
cbuffer Columns : register(b0) { uint first; uint last; uint from_vertex; uint pad; };
struct V { float4 position : SV_Position; nointerpolation uint read : READ; };
V vs(uint id : SV_VertexID, uint vertex_value : VALUE) {
  V v;
  float x0 = first / 8.0 - 1, x1 = last / 8.0 - 1;
  v.position = float4((id & 1) ? x1 : x0, (id & 2) ? -1 : 1, 0, 1);
  v.read = from_vertex == 1 ? value.Load(int3(0, 0, 0)) : from_vertex == 2 ? vertex_value : 0;
  return v;
}
uint ps(V v) : SV_Target {
  return v.read ? v.read : value.Load(int3(0, 0, 0));
}
)hlsl";

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate;
  D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device, nullptr,
                          &immediate));
  auto vs_code = compile(hlsl, "vs", "vs"), ps_code = compile(hlsl, "ps", "ps");
  if (!vs_code || !ps_code) {
    printf("failed: HLSL did not compile\n");
    return 1;
  }
  ComPtr<ID3D11VertexShader> vs;
  ComPtr<ID3D11PixelShader> ps;
  CHECK(device->CreateVertexShader(vs_code->GetBufferPointer(), vs_code->GetBufferSize(), nullptr, &vs));
  CHECK(device->CreatePixelShader(ps_code->GetBufferPointer(), ps_code->GetBufferSize(), nullptr, &ps));
  D3D11_INPUT_ELEMENT_DESC element{"VALUE", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0};
  ComPtr<ID3D11InputLayout> layout;
  CHECK(device->CreateInputLayout(&element, 1, vs_code->GetBufferPointer(), vs_code->GetBufferSize(), &layout));

  D3D11_TEXTURE2D_DESC target_desc{16, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                                   D3D11_BIND_RENDER_TARGET};
  ComPtr<ID3D11Texture2D> target;
  ComPtr<ID3D11RenderTargetView> rtv;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &target));
  CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
  target_desc.Usage = D3D11_USAGE_STAGING;
  target_desc.BindFlags = 0;
  target_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> readback;
  CHECK(device->CreateTexture2D(&target_desc, nullptr, &readback));

  // two textures of one texel, and a vertex buffer of one value a vertex
  auto texture = [&](uint32_t initial) {
    D3D11_TEXTURE2D_DESC desc{1, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_DEFAULT,
                              D3D11_BIND_SHADER_RESOURCE};
    D3D11_SUBRESOURCE_DATA data{&initial, 4};
    ComPtr<ID3D11Texture2D> out;
    device->CreateTexture2D(&desc, &data, &out);
    return out;
  };
  struct Case {
    const char *name;
    // 0: the pixel shader reads the texture, 1: the vertex shader does, 2: the vertex buffer is read
    uint32_t from;
    // whether a draw reads the resource before the first update
    bool used_first;
    // updates, each followed by a draw
    uint32_t updates;
    // a texture's updates are CopySubresourceRegion from a staging texture the CPU wrote, not UpdateSubresource
    bool staging = false;
  };
  const Case cases[] = {
      {"pixel shader, read before the update", 0, true, 1},
      {"vertex shader, read before the update", 1, true, 1},
      {"vertex buffer, read before the update", 2, true, 1},
      {"pixel shader, not read before the update", 0, false, 1},
      {"vertex shader, not read before the update", 1, false, 1},
      {"vertex buffer, not read before the update", 2, false, 1},
      {"pixel shader, not read before the first of three updates", 0, false, 3},
      {"vertex buffer, not read before the first of three updates", 2, false, 3},
      {"pixel shader, read before a copy from a staging texture", 0, true, 1, true},
      {"pixel shader, not read before copies from a staging texture", 0, false, 2, true},
  };
  unsigned failures = 0;
  for (int deferred = 0; deferred < 2; deferred++)
    for (auto &c : cases) {
      step("%s%s", c.name, deferred ? ", deferred" : "");
      ComPtr<ID3D11DeviceContext> context = immediate;
      if (deferred)
        CHECK(device->CreateDeferredContext(0, &context));
      auto tex = texture(1);
      ComPtr<ID3D11ShaderResourceView> srv;
      CHECK(device->CreateShaderResourceView(tex.Get(), nullptr, &srv));
      uint32_t vertices[4] = {1, 1, 1, 1};
      auto vb = buffer(device.Get(), sizeof(vertices), D3D11_BIND_VERTEX_BUFFER, 1);
      auto constants = buffer(device.Get(), 16, D3D11_BIND_CONSTANT_BUFFER);
      float clear[4] = {};
      context->ClearRenderTargetView(rtv.Get(), clear);
      context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
      D3D11_VIEWPORT viewport{0, 0, 16, 1, 0, 1};
      context->RSSetViewports(1, &viewport);
      context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
      context->IASetInputLayout(layout.Get());
      UINT stride = 4, offset = 0;
      context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
      context->VSSetShader(vs.Get(), nullptr, 0);
      context->PSSetShader(ps.Get(), nullptr, 0);
      context->VSSetShaderResources(0, 1, srv.GetAddressOf());
      context->PSSetShaderResources(0, 1, srv.GetAddressOf());
      context->VSSetConstantBuffers(0, 1, constants.GetAddressOf());
      // a draw of columns [first, last) reading the case's resource
      auto draw = [&](uint32_t first, uint32_t last) {
        uint32_t columns[4] = {first, last, c.from, 0};
        context->UpdateSubresource(constants.Get(), 0, nullptr, columns, 0, 0);
        context->Draw(4, 0);
      };
      // an unrelated draw first, which opens the pass: its texture and vertex buffer are its own
      auto other = texture(99);
      ComPtr<ID3D11ShaderResourceView> other_srv;
      CHECK(device->CreateShaderResourceView(other.Get(), nullptr, &other_srv));
      auto other_vb = buffer(device.Get(), sizeof(vertices), D3D11_BIND_VERTEX_BUFFER, 0);
      uint32_t from_pixel[4] = {0, 1, 0, 0};
      context->UpdateSubresource(constants.Get(), 0, nullptr, from_pixel, 0, 0);
      context->VSSetShaderResources(0, 1, other_srv.GetAddressOf());
      context->PSSetShaderResources(0, 1, other_srv.GetAddressOf());
      context->IASetVertexBuffers(0, 1, other_vb.GetAddressOf(), &stride, &offset);
      context->Draw(4, 0);
      context->VSSetShaderResources(0, 1, srv.GetAddressOf());
      context->PSSetShaderResources(0, 1, srv.GetAddressOf());
      context->IASetVertexBuffers(0, 1, vb.GetAddressOf(), &stride, &offset);
      uint32_t column = 2;
      std::vector<uint32_t> want(16, 0);
      want[0] = 99;
      if (c.used_first) {
        draw(column, column + 2);
        want[column] = want[column + 1] = 1;
        column += 2;
      }
      for (uint32_t u = 0; u < c.updates; u++) {
        uint32_t value = 10 + u;
        if (c.from == 2) {
          uint32_t values[4] = {value, value, value, value};
          context->UpdateSubresource(vb.Get(), 0, nullptr, values, 0, 0);
        } else if (c.staging) {
          D3D11_TEXTURE2D_DESC desc{1, 1, 1, 1, DXGI_FORMAT_R32_UINT, {1, 0}, D3D11_USAGE_STAGING, 0,
                                    D3D11_CPU_ACCESS_WRITE};
          D3D11_SUBRESOURCE_DATA data{&value, 4};
          ComPtr<ID3D11Texture2D> staging;
          CHECK(device->CreateTexture2D(&desc, &data, &staging));
          context->CopySubresourceRegion(tex.Get(), 0, 0, 0, 0, staging.Get(), 0, nullptr);
        } else {
          D3D11_BOX box{0, 0, 0, 1, 1, 1};
          context->UpdateSubresource(tex.Get(), 0, &box, &value, 4, 4);
        }
        draw(column, column + 2);
        want[column] = want[column + 1] = value;
        column += 2;
      }
      if (deferred) {
        ComPtr<ID3D11CommandList> list;
        CHECK(context->FinishCommandList(FALSE, &list));
        immediate->ExecuteCommandList(list.Get(), FALSE);
      }
      immediate->CopyResource(readback.Get(), target.Get());
      D3D11_MAPPED_SUBRESOURCE mapped;
      CHECK(immediate->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
      auto got = static_cast<const uint32_t *>(mapped.pData);
      for (uint32_t x = 0; x < 16; x++)
        if (got[x] != want[x]) {
          if (failures++ < 12)
            printf("wrong: %s%s: column %u is %u, want %u\n", c.name, deferred ? ", deferred" : "", x, got[x], want[x]);
          break;
        }
      immediate->Unmap(readback.Get(), 0);
    }
  if (failures) {
    printf("failed: %u wrong results\n", failures);
    return 1;
  }
  printf("passed\n");
  return 0;
}
