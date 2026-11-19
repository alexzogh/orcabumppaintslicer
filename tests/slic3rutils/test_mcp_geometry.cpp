#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <admesh/stl.h>
#include <nlohmann/json.hpp>

#include <boost/beast/core/detail/base64.hpp>

#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "slic3r/GUI/MCP/McpGeometry.hpp"

using Slic3r::Transform3d;
using Slic3r::Vec2d;
using Slic3r::Vec3f;
using Slic3r::Vec3i32;
using Slic3r::GUI::mcp::base64_encode;
using Slic3r::GUI::mcp::bed_json;
using Slic3r::GUI::mcp::encode_mesh;
using Slic3r::GUI::mcp::mesh_fingerprint;
using Slic3r::GUI::mcp::mesh_key;
using Slic3r::GUI::mcp::simplify_to;
using Slic3r::GUI::mcp::transform_json;
using Catch::Matchers::WithinAbs;
using nlohmann::json;

namespace {

// A unit cube from (0,0,0) to (1,1,1): eight vertices, twelve triangles. Small enough that every
// expectation below can be written out by hand.
indexed_triangle_set unit_cube()
{
    indexed_triangle_set mesh;
    mesh.vertices = {Vec3f(0.f, 0.f, 0.f), Vec3f(1.f, 0.f, 0.f), Vec3f(1.f, 1.f, 0.f), Vec3f(0.f, 1.f, 0.f),
                     Vec3f(0.f, 0.f, 1.f), Vec3f(1.f, 0.f, 1.f), Vec3f(1.f, 1.f, 1.f), Vec3f(0.f, 1.f, 1.f)};
    mesh.indices  = {Vec3i32(0, 2, 1), Vec3i32(0, 3, 2), Vec3i32(4, 5, 6), Vec3i32(4, 6, 7),
                     Vec3i32(0, 1, 5), Vec3i32(0, 5, 4), Vec3i32(2, 3, 7), Vec3i32(2, 7, 6),
                     Vec3i32(1, 2, 6), Vec3i32(1, 6, 5), Vec3i32(3, 0, 4), Vec3i32(3, 4, 7)};
    return mesh;
}

std::vector<unsigned char> from_base64(const std::string &text)
{
    std::vector<unsigned char> out(boost::beast::detail::base64::decoded_size(text.size()));
    const auto                 written = boost::beast::detail::base64::decode(out.data(), text.data(), text.size());
    out.resize(written.first);
    return out;
}

std::vector<float> floats_in(const json &buffer)
{
    const std::vector<unsigned char> bytes = from_base64(buffer.at("data").get<std::string>());
    std::vector<float>               values(bytes.size() / sizeof(float));
    if (!values.empty())
        std::memcpy(values.data(), bytes.data(), values.size() * sizeof(float));
    return values;
}

std::vector<std::int32_t> ints_in(const json &buffer)
{
    const std::vector<unsigned char> bytes = from_base64(buffer.at("data").get<std::string>());
    std::vector<std::int32_t>        values(bytes.size() / sizeof(std::int32_t));
    if (!values.empty())
        std::memcpy(values.data(), bytes.data(), values.size() * sizeof(std::int32_t));
    return values;
}

} // namespace

TEST_CASE("base64_encode matches the published encoding of a known string", "[McpGeometry]")
{
    // The worked examples from RFC 4648, which cover all three padding cases. Checked against the
    // standard rather than against a round-trip, so a decoder that shares the encoder's mistake
    // cannot make this pass.
    REQUIRE(base64_encode("Man", 3) == "TWFu");
    REQUIRE(base64_encode("Ma", 2) == "TWE=");
    REQUIRE(base64_encode("M", 1) == "TQ==");
}

TEST_CASE("base64_encode returns nothing for an empty buffer", "[McpGeometry]")
{
    REQUIRE(base64_encode(nullptr, 0).empty());
    REQUIRE(base64_encode("Man", 0).empty());
}

TEST_CASE("encode_mesh carries the vertices back unchanged", "[McpGeometry]")
{
    const indexed_triangle_set mesh   = unit_cube();
    const json                 result = encode_mesh(mesh);

    REQUIRE(result.at("vertices").get<std::size_t>() == mesh.vertices.size());
    REQUIRE(result.at("positions").at("type") == "float32");
    REQUIRE(result.at("positions").at("components").get<int>() == 3);
    REQUIRE(result.at("positions").at("count").get<std::size_t>() == mesh.vertices.size());

    const std::vector<float> values = floats_in(result.at("positions"));
    REQUIRE(values.size() == mesh.vertices.size() * 3);
    for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
        CHECK_THAT(values[i * 3 + 0], WithinAbs(mesh.vertices[i].x(), 1e-6));
        CHECK_THAT(values[i * 3 + 1], WithinAbs(mesh.vertices[i].y(), 1e-6));
        CHECK_THAT(values[i * 3 + 2], WithinAbs(mesh.vertices[i].z(), 1e-6));
    }
}

TEST_CASE("encode_mesh carries the triangle indices back unchanged", "[McpGeometry]")
{
    const indexed_triangle_set mesh   = unit_cube();
    const json                 result = encode_mesh(mesh);

    REQUIRE(result.at("triangles").get<std::size_t>() == mesh.indices.size());
    REQUIRE(result.at("indices").at("type") == "int32");
    REQUIRE(result.at("indices").at("components").get<int>() == 3);

    const std::vector<std::int32_t> values = ints_in(result.at("indices"));
    REQUIRE(values.size() == mesh.indices.size() * 3);
    for (std::size_t i = 0; i < mesh.indices.size(); ++i)
        for (int corner = 0; corner < 3; ++corner)
            CHECK(values[i * 3 + std::size_t(corner)] == mesh.indices[i][corner]);
}

TEST_CASE("encode_mesh reports the bounding box of the vertices", "[McpGeometry]")
{
    indexed_triangle_set mesh = unit_cube();
    // Push one corner out so the box is not simply the unit cube the vertices were written as.
    mesh.vertices[6] = Vec3f(4.f, 1.f, 1.f);

    const json result = encode_mesh(mesh);
    CHECK_THAT(result.at("bbox_min")[0].get<double>(), WithinAbs(0., 1e-6));
    CHECK_THAT(result.at("bbox_max")[0].get<double>(), WithinAbs(4., 1e-6));
    CHECK_THAT(result.at("bbox_max")[2].get<double>(), WithinAbs(1., 1e-6));
}

TEST_CASE("encode_mesh describes an empty mesh without inventing a box", "[McpGeometry]")
{
    const json result = encode_mesh(indexed_triangle_set());
    CHECK(result.at("triangles").get<std::size_t>() == 0);
    CHECK(result.at("vertices").get<std::size_t>() == 0);
    CHECK(result.at("positions").at("data").get<std::string>().empty());
    CHECK(result.at("indices").at("data").get<std::string>().empty());
    CHECK_THAT(result.at("bbox_min")[0].get<double>(), WithinAbs(0., 1e-6));
    CHECK_THAT(result.at("bbox_max")[0].get<double>(), WithinAbs(0., 1e-6));
}

TEST_CASE("mesh_fingerprint is the same for the same geometry", "[McpGeometry]")
{
    const std::string first  = mesh_fingerprint(unit_cube());
    const std::string second = mesh_fingerprint(unit_cube());
    REQUIRE(first == second);
    REQUIRE(first.size() == 16);
    REQUIRE(first.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("mesh_fingerprint changes when a vertex moves", "[McpGeometry]")
{
    indexed_triangle_set moved = unit_cube();
    moved.vertices[3].z() += 0.001f;
    REQUIRE(mesh_fingerprint(moved) != mesh_fingerprint(unit_cube()));
}

TEST_CASE("mesh_fingerprint changes when the triangles change", "[McpGeometry]")
{
    // Same vertices, same triangle count, one face wound the other way. A fingerprint over the
    // vertex buffer alone would miss this.
    indexed_triangle_set rewound = unit_cube();
    rewound.indices[0]           = Vec3i32(0, 1, 2);
    REQUIRE(mesh_fingerprint(rewound) != mesh_fingerprint(unit_cube()));
}

TEST_CASE("mesh_key separates volumes that hold identical geometry", "[McpGeometry]")
{
    const indexed_triangle_set mesh = unit_cube();
    REQUIRE(mesh_key(11, mesh) != mesh_key(12, mesh));
    REQUIRE(mesh_key(11, mesh) == mesh_key(11, unit_cube()));
}

TEST_CASE("simplify_to leaves a mesh that already fits alone", "[McpGeometry]")
{
    const indexed_triangle_set mesh = unit_cube();
    CHECK(simplify_to(mesh, 0).indices.size() == mesh.indices.size());
    CHECK(simplify_to(mesh, mesh.indices.size()).indices.size() == mesh.indices.size());
    CHECK(simplify_to(mesh, mesh.indices.size() + 100).indices.size() == mesh.indices.size());
    // Untouched, not merely the same size.
    CHECK(mesh_fingerprint(simplify_to(mesh, 0)) == mesh_fingerprint(mesh));
}

TEST_CASE("simplify_to reduces a mesh to its budget", "[McpGeometry]")
{
    const indexed_triangle_set sphere = Slic3r::its_make_sphere(10., 2. * PI / 90.);
    REQUIRE(sphere.indices.size() > 200);

    const indexed_triangle_set reduced = simplify_to(sphere, 100);
    CHECK(reduced.indices.size() <= 100);
    CHECK(reduced.indices.size() > 0);
    CHECK(reduced.vertices.size() > 0);
}

TEST_CASE("transform_json writes the matrix column by column", "[McpGeometry]")
{
    // A quarter turn about Z, then a translation. Both halves are asymmetric, so a transposed
    // matrix - which renders perfectly happily, just wrong - fails here rather than on someone's
    // screen.
    Transform3d transform = Transform3d::Identity();
    transform.translate(Slic3r::Vec3d(5., 6., 7.));
    transform.rotate(Eigen::AngleAxisd(0.5 * PI, Slic3r::Vec3d::UnitZ()));

    const json out = transform_json(transform);
    REQUIRE(out.size() == 16);

    // First column is the image of the x axis: (0, 1, 0).
    CHECK_THAT(out[0].get<double>(), WithinAbs(0., 1e-9));
    CHECK_THAT(out[1].get<double>(), WithinAbs(1., 1e-9));
    CHECK_THAT(out[2].get<double>(), WithinAbs(0., 1e-9));
    // Second column is the image of the y axis: (-1, 0, 0).
    CHECK_THAT(out[4].get<double>(), WithinAbs(-1., 1e-9));
    CHECK_THAT(out[5].get<double>(), WithinAbs(0., 1e-9));
    // Last column is the translation, with the homogeneous 1 beneath it.
    CHECK_THAT(out[12].get<double>(), WithinAbs(5., 1e-9));
    CHECK_THAT(out[13].get<double>(), WithinAbs(6., 1e-9));
    CHECK_THAT(out[14].get<double>(), WithinAbs(7., 1e-9));
    CHECK_THAT(out[15].get<double>(), WithinAbs(1., 1e-9));
}

TEST_CASE("bed_json reports the printable area as it was given", "[McpGeometry]")
{
    const std::vector<Vec2d> area{Vec2d(0., 0.), Vec2d(256., 0.), Vec2d(256., 256.), Vec2d(0., 256.)};
    const json               bed = bed_json(area, 250., "rectangular");

    CHECK(bed.at("type") == "rectangular");
    CHECK_THAT(bed.at("printable_height_mm").get<double>(), WithinAbs(250., 1e-9));
    REQUIRE(bed.at("polygon").size() == area.size());
    for (std::size_t i = 0; i < area.size(); ++i) {
        CHECK_THAT(bed.at("polygon")[i][0].get<double>(), WithinAbs(area[i].x(), 1e-9));
        CHECK_THAT(bed.at("polygon")[i][1].get<double>(), WithinAbs(area[i].y(), 1e-9));
    }
}

TEST_CASE("bed_json copes with a printer whose area is unknown", "[McpGeometry]")
{
    const json bed = bed_json({}, 0., "invalid");
    CHECK(bed.at("polygon").empty());
    CHECK(bed.at("type") == "invalid");
}
