#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <admesh/stl.h>
#include <nlohmann/json.hpp>

#include "libslic3r/Point.hpp"

#include "slic3r/GUI/MCP/McpProtocol.hpp"
#include "slic3r/GUI/MCP/McpSelectors.hpp"

using Slic3r::Transform3d;
using Slic3r::Vec3d;
using Slic3r::GUI::mcp::Selector;
using Slic3r::GUI::mcp::ToolError;
using Slic3r::GUI::mcp::describe;
using Slic3r::GUI::mcp::parse_selector;
using Slic3r::GUI::mcp::select_facets;
using nlohmann::json;

namespace {

// A unit cube from (0,0,0) to (1,1,1), wound so every normal points outward: two triangles per
// face, twelve in all. Every expectation below is stated in terms of that shape rather than a
// triangle index, so the tests say what they mean.
indexed_triangle_set unit_cube()
{
    indexed_triangle_set mesh;
    mesh.vertices = {Slic3r::Vec3f(0.f, 0.f, 0.f), Slic3r::Vec3f(1.f, 0.f, 0.f), Slic3r::Vec3f(1.f, 1.f, 0.f),
                     Slic3r::Vec3f(0.f, 1.f, 0.f), Slic3r::Vec3f(0.f, 0.f, 1.f), Slic3r::Vec3f(1.f, 0.f, 1.f),
                     Slic3r::Vec3f(1.f, 1.f, 1.f), Slic3r::Vec3f(0.f, 1.f, 1.f)};
    mesh.indices  = {Slic3r::Vec3i32(0, 2, 1), Slic3r::Vec3i32(0, 3, 2),  // bottom, -z
                     Slic3r::Vec3i32(4, 5, 6), Slic3r::Vec3i32(4, 6, 7),  // top, +z
                     Slic3r::Vec3i32(0, 1, 5), Slic3r::Vec3i32(0, 5, 4),  // front, -y
                     Slic3r::Vec3i32(1, 2, 6), Slic3r::Vec3i32(1, 6, 5),  // right, +x
                     Slic3r::Vec3i32(2, 3, 7), Slic3r::Vec3i32(2, 7, 6),  // back, +y
                     Slic3r::Vec3i32(3, 0, 4), Slic3r::Vec3i32(3, 4, 7)}; // left, -x
    mesh.properties.resize(mesh.indices.size());
    return mesh;
}

// The two triangles making up the cube's top face.
const std::vector<int> top_face{2, 3};

std::vector<int> pick(const json &selector_json, const std::vector<std::uint8_t> &existing = {},
                      const Transform3d &frame = Transform3d::Identity())
{
    return select_facets(parse_selector(selector_json), unit_cube(), frame, existing);
}

} // namespace

TEST_CASE("whole_volume selects every triangle", "[McpSelectors]")
{
    CHECK(pick(json{{"whole_volume", true}}).size() == 12);
}

TEST_CASE("a plane selects the side its normal points to", "[McpSelectors]")
{
    // Judged by centroid, so each side face contributes its upper triangle and the top face both of
    // its own: six of the twelve.
    const std::vector<int> above =
        pick(json{{"plane", {{"point", {0, 0, 0.5}}, {"normal", {0, 0, 1}}}}, {"side", "above"}});
    CHECK(above.size() == 6);

    const std::vector<int> below =
        pick(json{{"plane", {{"point", {0, 0, 0.5}}, {"normal", {0, 0, 1}}}}, {"side", "below"}});
    CHECK(below.size() == 6);

    // Nothing is both above and below, and together they are the whole cube.
    std::vector<int> both;
    both.insert(both.end(), above.begin(), above.end());
    both.insert(both.end(), below.begin(), below.end());
    std::sort(both.begin(), both.end());
    CHECK(std::unique(both.begin(), both.end()) == both.end());
    CHECK(both.size() == 12);
}

TEST_CASE("a band selects a slab on one axis", "[McpSelectors]")
{
    // Only the top face's centroids sit at z = 1; every side triangle is at 2/3 or below.
    CHECK(pick(json{{"axis", "z"}, {"from", 0.9}, {"to", 1.1}}) == top_face);
    CHECK(pick(json{{"axis", "z"}, {"from", -1.0}, {"to", 2.0}}).size() == 12);
    CHECK(pick(json{{"axis", "z"}, {"from", 5.0}, {"to", 6.0}}).empty());
}

TEST_CASE("a band given backwards means the same slab", "[McpSelectors]")
{
    CHECK(pick(json{{"axis", "z"}, {"from", 1.1}, {"to", 0.9}}) == top_face);
}

TEST_CASE("facing selects triangles pointing a given way", "[McpSelectors]")
{
    CHECK(pick(json{{"facing", {{"direction", {0, 0, 1}}, {"max_angle_deg", 45}}}}) == top_face);

    // Widened past a right angle it also takes the four side faces, whose normals are exactly
    // perpendicular, but never the bottom, which points the other way.
    CHECK(pick(json{{"facing", {{"direction", {0, 0, 1}}, {"max_angle_deg", 90}}}}).size() == 10);
    CHECK(pick(json{{"facing", {{"direction", {0, 0, -1}}, {"max_angle_deg", 45}}}}).size() == 2);
}

TEST_CASE("existing selects what is already painted", "[McpSelectors]")
{
    std::vector<std::uint8_t> painted(12, 0);
    painted[2] = 1;
    painted[3] = 1;
    painted[7] = 4;

    // No state, or state 0, means anything painted at all.
    CHECK(pick(json{{"existing", json::object()}}, painted).size() == 3);
    CHECK(pick(json{{"existing", {{"state", 0}}}}, painted).size() == 3);

    // A state picks out just that one, which is what makes "recolour what I painted green" sayable.
    CHECK(pick(json{{"existing", {{"state", 1}}}}, painted) == top_face);
    CHECK(pick(json{{"existing", {{"state", 4}}}}, painted) == std::vector<int>{7});
    CHECK(pick(json{{"existing", {{"state", 9}}}}, painted).empty());
}

TEST_CASE("existing selects nothing when the channel is empty", "[McpSelectors]")
{
    CHECK(pick(json{{"existing", json::object()}}, {}).empty());
}

TEST_CASE("the frame moves what a selector picks", "[McpSelectors]")
{
    // The same band, against a mesh placed ten millimetres up, selects the same face - which is the
    // whole point of saying which frame the coordinates are in.
    const Transform3d lifted = Transform3d(Eigen::Translation3d(0., 0., 10.));
    CHECK(pick(json{{"axis", "z"}, {"from", 10.9}, {"to", 11.1}}, {}, lifted) == top_face);
    CHECK(pick(json{{"axis", "z"}, {"from", 0.9}, {"to", 1.1}}, {}, lifted).empty());
}

TEST_CASE("a scaled frame does not break which way a face points", "[McpSelectors]")
{
    // Normals are taken from the transformed corners rather than transformed as directions, so a
    // non-uniform scale - where a normal does not transform like a direction - still comes out right.
    Transform3d squashed = Transform3d::Identity();
    squashed.scale(Vec3d(1., 1., 0.1));
    CHECK(pick(json{{"facing", {{"direction", {0, 0, 1}}, {"max_angle_deg", 45}}}}, {}, squashed) == top_face);
}

TEST_CASE("the frame defaults to the object", "[McpSelectors]")
{
    CHECK(parse_selector(json{{"whole_volume", true}}).frame == Selector::Frame::Object);
    CHECK(parse_selector(json{{"whole_volume", true}, {"frame", "world"}}).frame == Selector::Frame::World);
    CHECK(parse_selector(json{{"whole_volume", true}, {"frame", "volume"}}).frame == Selector::Frame::Volume);
}

TEST_CASE("a selector that names no shape is refused", "[McpSelectors]")
{
    CHECK_THROWS_AS(parse_selector(json::object()), ToolError);
    CHECK_THROWS_AS(parse_selector(json::array()), ToolError);
    CHECK_THROWS_AS(parse_selector(json{{"nonsense", true}}), ToolError);
}

TEST_CASE("a malformed selector is refused rather than guessed at", "[McpSelectors]")
{
    const auto bad = GENERATE(as<std::string>{},
                              R"({"plane": {"point": [0,0,0]}})",
                              R"({"plane": {"point": [0,0,0], "normal": [0,0,0]}})",
                              R"({"plane": {"point": [0,0], "normal": [0,0,1]}})",
                              R"({"plane": {"point": [0,0,0], "normal": [0,0,1]}, "side": "sideways"})",
                              R"({"axis": "w", "from": 0, "to": 1})",
                              R"({"axis": "z", "from": 0})",
                              R"({"axis": "z", "from": "low", "to": 1})",
                              R"({"facing": {"max_angle_deg": 45}})",
                              R"({"facing": {"direction": [0,0,1], "max_angle_deg": 0}})",
                              R"({"facing": {"direction": [0,0,1], "max_angle_deg": 200}})",
                              R"({"existing": {"state": -1}})",
                              R"({"whole_volume": true, "frame": "galactic"})");
    CAPTURE(bad);

    CHECK_THROWS_AS(parse_selector(json::parse(bad)), ToolError);
}

TEST_CASE("a refusal says what the accepted shapes are", "[McpSelectors]")
{
    // The error text is the only documentation a client sees at the point of failure, so it has to
    // carry the vocabulary rather than just saying no.
    try {
        parse_selector(json{{"nonsense", true}});
        FAIL("parse_selector accepted a selector that names no shape");
    } catch (const ToolError &e) {
        const std::string message = e.what();
        CHECK(message.find("whole_volume") != std::string::npos);
        CHECK(message.find("plane") != std::string::npos);
        CHECK(message.find("facing") != std::string::npos);
        CHECK(message.find("existing") != std::string::npos);
    }
}

TEST_CASE("a selector describes itself for the tool result", "[McpSelectors]")
{
    CHECK(describe(parse_selector(json{{"whole_volume", true}})) == "the whole volume");
    CHECK(describe(parse_selector(json{{"axis", "x"}, {"from", 0}, {"to", 1}})).find("x band") !=
          std::string::npos);
    CHECK(describe(parse_selector(json{{"existing", json::object()}})).find("already painted") !=
          std::string::npos);
}
