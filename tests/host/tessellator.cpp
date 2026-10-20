// contract: the fixed function tessellator is Direct3D's reference tessellator in what it connects as in where it
// puts its points (D3D11.3 11.7.1, 11.7.6): a patch's triangles, each its three domain locations in clockwise order,
// are the reference's, in whatever order they come (11.7.9), and so are an isoline patch's line segments and every
// patch's points, each once. this holds whatever largest factor the hull shader declares, which caps the factors
// (11.7.10 MaxTessFactor) and sizes the stages (src/airconv/tessellation_limits.hpp): the payload has room for that
// many workloads of a patch, which is all the hull stage is given, and a workload has no more vertices than the mesh.
// this runs the tessellator's own source, src/airconv/shaders/air_tessellation.metal, built for the host
// (metal/metal_stdlib stands in for Metal's library): the hull stage's workloads, what the mesh stage makes of each,
// and what a geometry shader's threadgroup looks up for each primitive (mullion.get_domain_primitive_location). the
// d3d11 and d3d12 tessellator tests check the same source as the GPU runs it, by its points and what it covers.
// listing work is the zip's attempted slots, including ones that make no triangle. D3D11.3 11.7.5 defines an epoch
// as "a power of 2 number of vertices on one half of the domain (excluding corners)"; 11.7.6.2 omits "one segment
// on each end of the inside". for H points on the larger half-edge, the smallest containing epoch E has E < 2H:
// trying both edges at both ends and two middle slots costs at most 4E + 2 <= 8H - 2. the frame has P >= 2H + 1
// edge points (the inner edge omits two end segments, the outer has at least one), so fewer than 4P slots suffice.
// each triangle advances one edge segment: a complete frame has the sum of its edges' segments in triangles.
#include "../tessellator.hpp"
#include "tessellation_limits.hpp"
#include <set>
// last: it names Metal's address spaces away
#include "metal/metal_stdlib"
#include "air_tessellation.metal"

using namespace tessellator;

// a primitive's corners as domain location bits; a triangle is the least of its turns, since it is the same
// triangle whichever corner it starts from
using Corner = std::array<uint32_t, 2>;
using Primitive = std::vector<Corner>;

static Corner
corner(float u, float v) {
  Corner c;
  memcpy(&c[0], &u, 4);
  memcpy(&c[1], &v, 4);
  return c;
}

static Primitive
primitive(Primitive p) {
  return p.size() == 3 ? std::min({p, Primitive{p[1], p[2], p[0]}, Primitive{p[2], p[0], p[1]}}) : p;
}

// the reference's primitives of `vertices` corners each
static std::multiset<Primitive>
expected(Domain domain, Partitioning partitioning, const Factors &patch, int vertices) {
  float factors[6];
  for (uint32_t i = 0; i < edges[domain]; i++)
    factors[i] = patch[i];
  for (uint32_t i = 0; i < insides[domain]; i++)
    factors[edges[domain] + i] = patch[4 + i];
  std::multiset<Primitive> out;
  if (vertices == 1) {
    for (auto &p : reference::points(domain, partitioning, factors))
      out.insert({corner(p.u, p.v)});
    return out;
  }
  auto corners = reference::corners(domain, partitioning, factors, vertices);
  for (size_t i = 0; i + vertices <= corners.size(); i += vertices) {
    Primitive p;
    for (int c = 0; c < vertices; c++)
      p.push_back(corner(corners[i + c].u, corners[i + c].v));
    out.insert(primitive(p));
  }
  return out;
}

int
main() {
  using namespace mullion::dxbc;
  unsigned failures = 0, patches_checked = 0;
  size_t primitives = 0, frames_checked = 0, zip_slots = 0;
  // the largest factors a hull shader may declare: the tessellator's, and lower ones whole and not, odd and even
  for (float declared : {64.0f, 17.5f, 8.0f, 3.0f, 1.0f})
  for (Domain domain : {reference::isoline, reference::triangle, reference::quad})
    for (Partitioning partitioning :
         {reference::integer, reference::pow2, reference::fractional_odd, reference::fractional_even}) {
      // the tokenized program's names for them follow its undefined ones
      auto sb_domain = microsoft::D3D11_SB_TESSELLATOR_DOMAIN(domain + 1);
      auto sb_partitioning = microsoft::D3D11_SB_TESSELLATOR_PARTITIONING(partitioning + 1);
      uint32_t most_segments = get_integer_factor(declared, sb_partitioning);
      // the room a patch's workloads have, and no more
      std::vector<tess_workload> workloads(get_max_potential_workload_count(most_segments, sb_domain, sb_partitioning));
      auto data = (int *)workloads.data();
      auto fail = [&](const char *what, const Factors &f, size_t got, size_t want) {
        if (failures++ < 8)
          printf(
              "%s, %s, at most %g, factors %g %g %g %g, inside %g %g: %s: %zu, want %zu\n", domain_names[domain],
              partitioning_names[partitioning], declared, f[0], f[1], f[2], f[3], f[4], f[5], what, got, want
          );
      };
      // the patches the GPU tests draw alone, then draws of mixed ones
      std::vector<Factors> all(std::begin(alone), std::end(alone));
      all.insert(all.end(), std::begin(lines_alone), std::end(lines_alone));
      for (uint32_t seed = 1; seed <= 48; seed++)
        for (auto &patch : draw_patches(domain, seed))
          all.push_back(patch);
      if (domain != reference::isoline)
        for (uint32_t factor = 1; factor <= most_segments; factor++) {
          Factors uniform{};
          uniform.fill(factor);
          all.push_back(uniform);
          // both sides of every epoch boundary, with unequal edges and inside axes and both orders of parity
          for (float inside : {1.0f, float(factor) + 0.5f, float(most_segments)}) {
            Factors mixed = {float(factor), 1, float(most_segments), float(factor) + 0.5f, inside, float(factor)};
            all.push_back(mixed);
          }
        }
      for (auto f : all) {
        // the hull stage's cap, which a NaN passes
        for (auto &factor : f)
          factor = factor > declared ? declared : factor;
        int count = 0;
        if (domain == reference::quad)
          generate_workload_quad(0, &count, data, workloads.size(), partitioning, f[4], f[5], f[0], f[1], f[2], f[3]);
        else if (domain == reference::triangle)
          generate_workload_triangle(0, &count, data, workloads.size(), partitioning, f[4], f[0], f[1], f[2]);
        else
          generate_workload_isoline(0, &count, data, workloads.size(), partitioning, f[0], f[1]);
        // one more than there is room for was not written
        if ((size_t)count > workloads.size())
          fail("workloads", f, count, workloads.size());
        count = std::min<size_t>(count, workloads.size());
        if (domain != reference::isoline)
          for (int w = 0; w < count; w++)
            for (int frame = 0; frame < 4 && workloads[w].edge[frame].segments >= 0; frame += 2) {
              tess_zip zip = {workloads[w].edge[frame], workloads[w].edge[frame + 1], 0, 0, 0};
              size_t triangles = 0, segments = zip.inner.segments + zip.outer.segments;
              short3 indices(0, 0, 0);
              while (zip.next(indices, 0, zip.inner.segments + 1))
                triangles++;
              if (triangles != segments)
                fail("triangles in a frame", f, triangles, segments);
              size_t bound = 4 * (segments + 2);
              if ((size_t)zip.slot >= bound)
                fail("zip slots in a frame", f, zip.slot, bound - 1);
              frames_checked++;
              zip_slots += zip.slot;
            }
        // by corners: the points, an isoline patch's lines, the other patches' triangles
        for (int vertices : {1, domain == reference::isoline ? 2 : 3}) {
          std::multiset<Primitive> made, looked;
          for (int w = 0; w < count; w++) {
            auto at = [&](int vertex) {
              auto location = workload_vertex(workloads[w], vertex);
              return corner(location.uv.x, location.uv.y);
            };
            int workload_vertices = 0;
            for (auto &edge : workloads[w].edge)
              workload_vertices += edge.segments + 1;
            if ((uint32_t)workload_vertices > get_max_workload_vertices(most_segments, sb_domain))
              fail("vertices in a workload", f, workload_vertices, get_max_workload_vertices(most_segments, sb_domain));
            metal::recorder recorded;
            Mesh mesh{&recorded};
            if (vertices == 3)
              generatePrimitiveTriangle(w, data, mesh, nullptr);
            else if (vertices == 2)
              generatePrimitiveLine(w, data, mesh, nullptr);
            else
              generatePrimitivePoint(w, data, mesh, nullptr);
            for (uint32_t n = 0; n < recorded.primitives; n++) {
              Primitive from_mesh, from_lookup;
              for (int c = 0; c < vertices; c++) {
                from_mesh.push_back(at(recorded.indices[vertices * n + c]));
                auto location = get_domain_primitive_location(w, n, c, vertices, 0, data);
                from_lookup.push_back(corner(location.uv.x, location.uv.y));
                if (from_lookup.back() != from_mesh.back())
                  fail("primitive numbering", f, primitive_vertex(workloads[w], n, c, vertices),
                       recorded.indices[vertices * n + c]);
              }
              made.insert(primitive(from_mesh));
              looked.insert(primitive(from_lookup));
            }
            // the lookup ends where the mesh's primitives do
            if (get_domain_primitive_location(w, recorded.primitives, 0, vertices, 0, data).active)
              fail("primitives looked up past a workload's", f, recorded.primitives + 1, recorded.primitives);
          }
          auto want = expected(domain, partitioning, f, vertices);
          if (made != want)
            fail(vertices == 1 ? "points" : vertices == 2 ? "lines" : "triangles", f, made.size(), want.size());
          if (looked != made)
            fail("primitives looked up", f, looked.size(), made.size());
          primitives += want.size();
        }
        patches_checked++;
      }
    }
  if (failures) {
    printf("failed: %u wrong\n", failures);
    return 1;
  }
  printf("passed: %zu primitives of %u patches as the reference's; %zu frames in %zu zip slots\n",
         primitives, patches_checked, frames_checked, zip_slots);
  return 0;
}
