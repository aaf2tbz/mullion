// contract: uint UAV clears copy channel bits; float and ClearView clears apply the view format's conversions.
// "copying the lower ni bits from each array element i to the corresponding channel" (D3D11.3 5.2.3.2).
// FLOAT to UNORM/SNORM maps NaN to zero and clamps infinities, with "0.6f ULP" tolerance on the integer side
// (3.2.3.4/3.2.3.6). sRGB conversion uses 3.2.3.8's transfer function, except alpha remains linear (19.1.4).
// ClearView integer inputs have "fractions rounded to zero, out of range/INF values clamped to target range,
// NaN to 0" (5.2.3.3). viewport/scissor do not restrict full-view clears (5.2.3.1).
// 3.2.2: "Round-to-zero must be used during conversion to another float format"; finite overflow gives
// "the maximum representable (appropriately signed) value results, NOT including signed infinity".
// 3.1.5/6 define five exponent bits and preserved denorms for 16/11/10-bit formats; unsigned negatives clamp to 0.
// ClearView with null rectangles covers the full view: "NULL means clear the entire view." (5.2.3.3); two
// rectangles that tile the view clear every texel of it as well.
// SDK format names supply channel order and widths; every physical channel is checked over every texel.
#include "d3d11_test.hpp"
#include "../copy_formats.hpp"
#include <d3d11_1.h>
#include <algorithm>
#include <array>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <limits>

int
main() {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> immediate, deferred;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  CHECK(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                         &device, nullptr, &immediate));
  CHECK(device->CreateDeferredContext(0, &deferred));
  ComPtr<ID3D11DeviceContext1> contexts[2];
  CHECK(immediate.As(&contexts[0]));
  CHECK(deferred.As(&contexts[1]));
  D3D11_RASTERIZER_DESC raster{D3D11_FILL_SOLID, D3D11_CULL_NONE};
  raster.ScissorEnable = TRUE;
  ComPtr<ID3D11RasterizerState> scissored;
  CHECK(device->CreateRasterizerState(&raster, &scissored));
  constexpr UINT width = 3, height = 2;
  constexpr double norm_tolerance = 0.6; // D3D11.3 3.2.3.4/6/8, integer ULP
  const float infinity = std::numeric_limits<float>::infinity();
  const float base_values[] = {-infinity, std::numeric_limits<float>::lowest(), -1.5f, -1, -0.5f,
                          -std::numeric_limits<float>::denorm_min(), -0.0f, 0.0f,
                          std::numeric_limits<float>::denorm_min(), 0.25f, 0.5f, 0.75f, 1, 1.5f,
                          std::numeric_limits<float>::max(), infinity, std::numeric_limits<float>::quiet_NaN()};
  const UINT bit_values[] = {0, 1, UINT_MAX, UINT(1) << (sizeof(UINT) * CHAR_BIT - 1), 0xa53c19e7};
  UINT exercised = 0;
  for (const auto &format : copy_formats) {
    const bool snorm = strstr(format.name, "_SNORM"), unorm = strstr(format.name, "_UNORM");
    const bool sint = strstr(format.name, "_SINT"), uint = strstr(format.name, "_UINT");
    const bool floating = strstr(format.name, "_FLOAT"), srgb = strstr(format.name, "_SRGB");
    if (format.depth || format.unit != 1 || strstr(format.name, "TYPELESS") || strchr(format.family, 'X') ||
        !(snorm || unorm || sint || uint || floating))
      continue;
    UINT support = 0;
    const HRESULT support_hr = device->CheckFormatSupport(format.id, &support);
    if (!expect(support_hr == S_OK || (support_hr == E_FAIL && !support),
                "format %s support: HRESULT %#lx, bits %#x", format.name, support_hr, support))
      return verdict();
    if (support_hr == E_FAIL)
      continue;
    struct Channel { UINT component, bits; };
    constexpr char components[] = "RGBA";
    std::vector<Channel> channels;
    for (const char *at = format.family; *at;) {
      const char *component = strchr(components, *at++);
      char *end;
      const UINT bits = UINT(strtoul(at, &end, 10));
      if (!expect(component && at != end && bits <= sizeof(UINT) * CHAR_BIT,
                  "unexpected SDK layout %s", format.family))
        return verdict();
      channels.push_back({UINT(component - components), bits});
      at = end;
    }
    std::vector<float> values(std::begin(base_values), std::end(base_values));
    if (floating)
      for (auto channel : channels)
        if (channel.bits < sizeof(float) * CHAR_BIT) {
          constexpr UINT exponent_bits = 5; // D3D11.3 3.1.5/6
          constexpr int bias = (1 << (exponent_bits - 1)) - 1;
          const UINT fraction_bits = channel.bits - exponent_bits - (channel.bits == 16);
          const float minimum = std::ldexp(1.0f, 1 - bias - int(fraction_bits));
          const float maximum = std::ldexp(2.0f - std::ldexp(1.0f, -int(fraction_bits)), bias);
          for (float edge : {minimum, std::ldexp(1.0f, 1 - bias), maximum,
                             1.0f + std::ldexp(3.0f, -int(fraction_bits) - 2)})
            for (float sign : {-1.0f, 1.0f}) {
              values.push_back(sign * std::nextafter(edge, 0.0f));
              values.push_back(sign * edge);
              values.push_back(sign * std::nextafter(edge, infinity));
            }
        }
    for (UINT method = 0; method < 6; method++) {
      const bool unordered = method == 0 || method == 1 || method == 2;
      const bool clear_view = method == 2 || method >= 4;
      if (!(support & (unordered ? D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW :
                                   D3D11_FORMAT_SUPPORT_RENDER_TARGET)))
        continue;
      if ((sint || uint) && !clear_view && method != 0)
        continue;
      D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, format.id, {1, 0}, D3D11_USAGE_DEFAULT,
                                 unordered ? D3D11_BIND_UNORDERED_ACCESS : D3D11_BIND_RENDER_TARGET};
      ComPtr<ID3D11Texture2D> target, staging;
      CHECK(device->CreateTexture2D(&desc, nullptr, &target));
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      CHECK(device->CreateTexture2D(&desc, nullptr, &staging));
      ComPtr<ID3D11UnorderedAccessView> uav;
      ComPtr<ID3D11RenderTargetView> rtv;
      ComPtr<ID3D11View> view;
      if (unordered) {
        CHECK(device->CreateUnorderedAccessView(target.Get(), nullptr, &uav));
        view = uav;
      } else {
        CHECK(device->CreateRenderTargetView(target.Get(), nullptr, &rtv));
        view = rtv;
      }
      for (UINT route = 0; route < std::size(contexts); route++)
        for (UINT index = 0; index < (method ? std::size(values) : std::size(bit_values)); index++) {
          std::array<float, 4> color;
          std::array<UINT, 4> bits;
          for (UINT c = 0; c < color.size(); c++) {
            color[c] = values[(index + c) % std::size(values)];
            bits[c] = bit_values[(index + c) % std::size(bit_values)];
          }
          step("clear %s method %u route %u values %u", format.name, method, route, index);
          auto context = contexts[route].Get();
          const D3D11_VIEWPORT viewport{0, 0, 0, 0, 0, 1};
          const D3D11_RECT empty{};
          context->RSSetViewports(1, &viewport);
          context->RSSetScissorRects(1, &empty);
          context->RSSetState(scissored.Get());
          if (method == 0)
            context->ClearUnorderedAccessViewUint(uav.Get(), bits.data());
          else if (method == 1)
            context->ClearUnorderedAccessViewFloat(uav.Get(), color.data());
          else if (method == 5) {
            const D3D11_RECT halves[] = {{0, 0, LONG(width / 2), LONG(height)}, {LONG(width / 2), 0, LONG(width), LONG(height)}};
            context->ClearView(view.Get(), color.data(), halves, UINT(std::size(halves)));
          } else if (clear_view)
            context->ClearView(view.Get(), color.data(), nullptr, 0);
          else
            context->ClearRenderTargetView(rtv.Get(), color.data());
          context->CopyResource(staging.Get(), target.Get());
          if (route) {
            ComPtr<ID3D11CommandList> commands;
            CHECK(deferred->FinishCommandList(FALSE, &commands));
            immediate->ExecuteCommandList(commands.Get(), FALSE);
          }
          D3D11_MAPPED_SUBRESOURCE mapped;
          CHECK(immediate->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
          bool ok = true;
          for (UINT y = 0; y < height && ok; y++)
            for (UINT x = 0; x < width && ok; x++) {
              auto pixel = static_cast<unsigned char *>(mapped.pData) + y * mapped.RowPitch + x * format.bytes;
              UINT at = 0;
              for (auto channel : channels) {
                UINT64 encoded = 0;
                for (UINT bit = 0; bit < channel.bits; bit++, at++)
                  encoded |= UINT64((pixel[at / CHAR_BIT] >> (at % CHAR_BIT)) & 1) << bit;
                const UINT64 mask = (UINT64(1) << channel.bits) - 1;
                if (!method) {
                  ok &= expect(encoded == (bits[channel.component] & mask), "pixel (%u,%u) channel %u bits %#llx",
                               x, y, channel.component, encoded);
                  continue;
                }
                const double value = color[channel.component];
                if (floating) {
                  if (channel.bits == sizeof(float) * CHAR_BIT) {
                    const float got = std::bit_cast<float>(UINT(encoded));
                    const float input = color[channel.component];
                    ok &= expect(std::isnan(value) ? std::isnan(got) :
                        UINT(encoded) == std::bit_cast<UINT>(input) ||
                            (std::fpclassify(input) == FP_SUBNORMAL &&
                             UINT(encoded) == std::bit_cast<UINT>(std::copysign(0.0f, input))),
                        "pixel (%u,%u) float channel %u: %#llx, input %g", x, y, channel.component, encoded, value);
                  } else {
                    constexpr UINT exponent_bits = 5; // D3D11.3 3.1.5/6
                    constexpr int bias = (1 << (exponent_bits - 1)) - 1;
                    const bool signed_float = channel.bits == 16;
                    const UINT fraction_bits = channel.bits - exponent_bits - signed_float;
                    const UINT fraction_mask = (1u << fraction_bits) - 1;
                    const UINT exponent_mask = (1u << exponent_bits) - 1;
                    const UINT sign = signed_float && std::signbit(value) ? 1u << (channel.bits - 1) : 0;
                    UINT want = sign;
                    if (std::isnan(value)) {
                      ok &= expect(((encoded >> fraction_bits) & exponent_mask) == exponent_mask &&
                                       (encoded & fraction_mask), "pixel (%u,%u) channel %u: NaN became %#llx",
                                   x, y, channel.component, encoded);
                      continue;
                    }
                    const double magnitude = !signed_float && value < 0 ? 0 : std::abs(value);
                    if (std::isinf(magnitude))
                      want |= exponent_mask << fraction_bits;
                    else if (magnitude) {
                      int exponent;
                      const double fraction = std::frexp(magnitude, &exponent);
                      const int biased = exponent - 1 + bias;
                      if (biased >= int(exponent_mask))
                        want |= ((exponent_mask - 1) << fraction_bits) | fraction_mask;
                      else if (biased <= 0)
                        want |= UINT(std::floor(std::ldexp(magnitude, bias - 1 + fraction_bits)));
                      else
                        want |= (UINT(biased) << fraction_bits) |
                                UINT(std::floor(std::ldexp(2 * fraction - 1, fraction_bits)));
                    }
                    ok &= expect(encoded == want, "pixel (%u,%u) channel %u: %#llx, want %#x for %g",
                                 x, y, channel.component, encoded, want, value);
                  }
                } else {
                  const bool signed_value = snorm || sint;
                  const UINT64 sign = UINT64(1) << (channel.bits - 1);
                  const double got = signed_value ? double(INT64(encoded ^ sign) - INT64(sign)) : double(encoded);
                  const double maximum = signed_value ? double(sign - 1) : double(mask);
                  const double minimum = snorm ? -maximum : sint ? -double(sign) : 0;
                  double ideal = std::isnan(value) ? 0 : std::clamp(value, snorm ? -1.0 : 0.0, 1.0);
                  if (unorm && srgb && channel.component != 3)
                    ideal = ideal <= 0.0031308 ? 12.92 * ideal : 1.055 * std::pow(ideal, 1.0 / 2.4) - 0.055;
                  ideal *= maximum;
                  if (sint || uint)
                    ideal = std::isnan(value) ? 0 : std::trunc(std::clamp(value, minimum, maximum));
                  ok &= expect((sint || uint) ? got == ideal : std::abs(got - ideal) <= norm_tolerance,
                               "pixel (%u,%u) channel %u: %g, ideal %g", x, y, channel.component, got, ideal);
                }
              }
            }
          immediate->Unmap(staging.Get(), 0);
          exercised++;
        }
    }
  }
  expect(exercised != 0, "no clear conversion was exercised");
  return verdict();
}
