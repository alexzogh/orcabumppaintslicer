#include "McpSelectors.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>

#include "McpProtocol.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

Vec3d read_vec3(const json &value, const char *what)
{
    if (!value.is_array() || value.size() != 3)
        throw ToolError(std::string(what) + " must be an array of three numbers, like [0, 0, 1].");
    for (const json &component : value)
        if (!component.is_number())
            throw ToolError(std::string(what) + " must be an array of three numbers, like [0, 0, 1].");
    return Vec3d(value[0].get<double>(), value[1].get<double>(), value[2].get<double>());
}

int read_axis(const json &value)
{
    const std::string axis = value.is_string() ? value.get<std::string>() : std::string();
    if (axis == "x" || axis == "X")
        return 0;
    if (axis == "y" || axis == "Y")
        return 1;
    if (axis == "z" || axis == "Z")
        return 2;
    throw ToolError("axis must be \"x\", \"y\" or \"z\".");
}

Selector::Frame read_frame(const json &args)
{
    const std::string frame = args.value("frame", std::string("object"));
    if (frame == "object")
        return Selector::Frame::Object;
    if (frame == "volume")
        return Selector::Frame::Volume;
    if (frame == "world")
        return Selector::Frame::World;
    throw ToolError("frame must be \"volume\", \"object\" or \"world\"; the default is \"object\", "
                    "which is the model as it sits on the plate.");
}

const char *axis_name(int axis) { return axis == 0 ? "x" : (axis == 1 ? "y" : "z"); }

// Spelled out rather than taken from M_PI, which is not standard and needs _USE_MATH_DEFINES to
// exist at all under MSVC-style headers.
constexpr double pi = 3.14159265358979323846;

// The point a triangle is judged by. Using the centroid means a triangle straddling a plane or a
// band edge falls on one side rather than being split: set_facet works at whole-triangle
// granularity anyway, so there is nothing finer to express.
Vec3d facet_centroid(const indexed_triangle_set &mesh, const Transform3d &to_frame, int facet)
{
    const Vec3i32 &index = mesh.indices[size_t(facet)];
    Vec3d          sum   = Vec3d::Zero();
    for (int corner = 0; corner < 3; ++corner)
        sum += to_frame * mesh.vertices[size_t(index[corner])].cast<double>();
    return sum / 3.;
}

// Taken from the transformed corners rather than transformed from the mesh normal, so that any
// affine placement - including a non-uniform scale, where the normal does not transform like a
// direction - comes out right.
Vec3d facet_normal(const indexed_triangle_set &mesh, const Transform3d &to_frame, int facet)
{
    const Vec3i32 &index = mesh.indices[size_t(facet)];
    const Vec3d    a     = to_frame * mesh.vertices[size_t(index[0])].cast<double>();
    const Vec3d    b     = to_frame * mesh.vertices[size_t(index[1])].cast<double>();
    const Vec3d    c     = to_frame * mesh.vertices[size_t(index[2])].cast<double>();
    return (b - a).cross(c - a);
}

} // namespace

Selector parse_selector(const json &args)
{
    if (!args.is_object())
        throw ToolError("selector must be an object. Accepted shapes: {\"whole_volume\": true}, "
                        "{\"plane\": {\"point\": [x,y,z], \"normal\": [x,y,z]}, \"side\": \"above\"}, "
                        "{\"axis\": \"z\", \"from\": 0, \"to\": 10}, "
                        "{\"facing\": {\"direction\": [x,y,z], \"max_angle_deg\": 45}}, "
                        "{\"existing\": {\"state\": 0}}.");

    Selector selector;
    selector.frame = read_frame(args);

    if (args.value("whole_volume", false)) {
        selector.kind = Selector::Kind::WholeVolume;
        return selector;
    }

    if (args.contains("plane")) {
        const json &plane = args.at("plane");
        if (!plane.is_object() || !plane.contains("point") || !plane.contains("normal"))
            throw ToolError("plane needs both a point and a normal, as "
                            "{\"plane\": {\"point\": [0,0,10], \"normal\": [0,0,1]}}.");
        selector.kind         = Selector::Kind::Plane;
        selector.plane_point  = read_vec3(plane.at("point"), "plane.point");
        selector.plane_normal = read_vec3(plane.at("normal"), "plane.normal");
        if (selector.plane_normal.norm() < 1e-9)
            throw ToolError("plane.normal must not be zero length.");
        const std::string side = args.value("side", std::string("above"));
        if (side != "above" && side != "below")
            throw ToolError("side must be \"above\" or \"below\". Above means the side the normal points to.");
        selector.take_above = side == "above";
        return selector;
    }

    if (args.contains("axis")) {
        if (!args.contains("from") || !args.contains("to"))
            throw ToolError("a band needs from and to in millimetres, as {\"axis\": \"z\", \"from\": 0, \"to\": 10}.");
        if (!args.at("from").is_number() || !args.at("to").is_number())
            throw ToolError("from and to must be numbers, in millimetres.");
        selector.kind = Selector::Kind::Band;
        selector.axis = read_axis(args.at("axis"));
        selector.from = args.at("from").get<double>();
        selector.to   = args.at("to").get<double>();
        if (selector.from > selector.to)
            std::swap(selector.from, selector.to);
        return selector;
    }

    if (args.contains("facing")) {
        const json &facing = args.at("facing");
        if (!facing.is_object() || !facing.contains("direction"))
            throw ToolError("facing needs a direction, as "
                            "{\"facing\": {\"direction\": [0,-1,0], \"max_angle_deg\": 45}}.");
        selector.kind      = Selector::Kind::Facing;
        selector.direction = read_vec3(facing.at("direction"), "facing.direction");
        if (selector.direction.norm() < 1e-9)
            throw ToolError("facing.direction must not be zero length.");
        selector.max_angle_deg = facing.value("max_angle_deg", 45.);
        if (selector.max_angle_deg <= 0. || selector.max_angle_deg > 180.)
            throw ToolError("facing.max_angle_deg must be above 0 and at most 180.");
        return selector;
    }

    if (args.contains("existing")) {
        const json &existing = args.at("existing");
        if (!existing.is_object())
            throw ToolError("existing must be an object, as {\"existing\": {\"state\": 2}}. "
                            "Leave state out, or set it to 0, to mean anything painted at all.");
        selector.kind           = Selector::Kind::Existing;
        selector.existing_state = existing.value("state", 0);
        if (selector.existing_state < 0)
            throw ToolError("existing.state must not be negative.");
        return selector;
    }

    throw ToolError("selector did not name a shape. Use one of whole_volume, plane, axis (a band), "
                    "facing, or existing.");
}

std::vector<int> select_facets(const Selector             &selector,
                               const indexed_triangle_set &mesh,
                               const Transform3d          &to_frame,
                               const std::vector<uint8_t> &existing)
{
    const int        facets = int(mesh.indices.size());
    std::vector<int> picked;

    switch (selector.kind) {
    case Selector::Kind::WholeVolume:
        picked.reserve(size_t(facets));
        for (int facet = 0; facet < facets; ++facet)
            picked.push_back(facet);
        break;

    case Selector::Kind::Plane: {
        const Vec3d normal = selector.plane_normal.normalized();
        for (int facet = 0; facet < facets; ++facet) {
            const double side = (facet_centroid(mesh, to_frame, facet) - selector.plane_point).dot(normal);
            if (selector.take_above ? side >= 0. : side <= 0.)
                picked.push_back(facet);
        }
        break;
    }

    case Selector::Kind::Band:
        for (int facet = 0; facet < facets; ++facet) {
            const double coordinate = facet_centroid(mesh, to_frame, facet)[selector.axis];
            if (coordinate >= selector.from && coordinate <= selector.to)
                picked.push_back(facet);
        }
        break;

    case Selector::Kind::Facing: {
        const Vec3d  direction = selector.direction.normalized();
        // Compared with a tolerance because the caller is thinking in degrees, not cosines: at
        // max_angle_deg 90 the cosine is 6e-17 rather than 0, so an exactly perpendicular face -
        // every side of a box, when asked for everything not pointing away - would fall just
        // outside the comparison.
        const double smallest = std::cos(selector.max_angle_deg * pi / 180.) - 1e-9;
        for (int facet = 0; facet < facets; ++facet) {
            const Vec3d normal = facet_normal(mesh, to_frame, facet);
            const double length = normal.norm();
            // A degenerate triangle has no direction to compare, so it is never "facing" anywhere.
            if (length < 1e-12)
                continue;
            if (normal.dot(direction) / length >= smallest)
                picked.push_back(facet);
        }
        break;
    }

    case Selector::Kind::Existing:
        for (int facet = 0; facet < facets && size_t(facet) < existing.size(); ++facet) {
            const uint8_t state = existing[size_t(facet)];
            if (selector.existing_state == 0 ? state != 0 : int(state) == selector.existing_state)
                picked.push_back(facet);
        }
        break;
    }

    return picked;
}

std::string describe(const Selector &selector)
{
    const std::string frame = selector.frame == Selector::Frame::Volume ?
                                  " in the volume's own frame" :
                                  (selector.frame == Selector::Frame::World ? " in world coordinates" : "");

    switch (selector.kind) {
    case Selector::Kind::WholeVolume:
        return "the whole volume";
    case Selector::Kind::Plane:
        return std::string(selector.take_above ? "above" : "below") + " a plane" + frame;
    case Selector::Kind::Band:
        return std::string("the ") + axis_name(selector.axis) + " band from " + std::to_string(selector.from) +
               " to " + std::to_string(selector.to) + "mm" + frame;
    case Selector::Kind::Facing:
        return "faces pointing within " + std::to_string(int(selector.max_angle_deg)) + " degrees of a direction" +
               frame;
    case Selector::Kind::Existing:
        return selector.existing_state == 0 ?
                   "whatever is already painted" :
                   "what is already painted with state " + std::to_string(selector.existing_state);
    }
    return "an unknown selection";
}

}}} // namespace Slic3r::GUI::mcp
