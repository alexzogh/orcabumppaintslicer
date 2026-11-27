#ifndef slic3r_McpSelectors_hpp_
#define slic3r_McpSelectors_hpp_

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <admesh/stl.h>

#include "libslic3r/Point.hpp"

namespace Slic3r { namespace GUI { namespace mcp {

// Which triangles of a mesh a tool should act on.
//
// The GUI paints with a brush, and a brush stroke has no API analogue: a client cannot drag a
// cursor across a model. Rather than simulate the gesture, this describes the *result* a stroke
// was meant to produce, declaratively, so the same vocabulary serves every painting tool and both
// paint channels.
struct Selector
{
    enum class Kind {
        WholeVolume, // every triangle
        Plane,       // one side of an arbitrary plane
        Band,        // a slab between two coordinates on one axis
        Facing,      // triangles whose normal points a given way, for "the front"
        Existing,    // whatever is already painted, which is what makes "recolour what I painted" sayable
    };

    // The space the selector's coordinates and directions are written in. A client saying "above
    // 10mm" almost always means the object as it sits on the plate, not the raw mesh, so Object is
    // the default; Volume is the mesh's own frame, and World includes the instance placement.
    enum class Frame { Volume, Object, World };

    Kind  kind  = Kind::WholeVolume;
    Frame frame = Frame::Object;

    // Plane: a point on it, its normal, and which side to take.
    Vec3d plane_point{0., 0., 0.};
    Vec3d plane_normal{0., 0., 1.};
    bool  take_above = true;

    // Band: a slab on one axis, 0 = x, 1 = y, 2 = z.
    int    axis = 2;
    double from = 0.;
    double to   = 0.;

    // Facing: triangles whose normal is within max_angle_deg of direction.
    Vec3d  direction     = Vec3d(0., 0., 1.);
    double max_angle_deg = 45.;

    // Existing: which painted state to match. 0 means "anything painted at all".
    int existing_state = 0;
};

// Reads the selector vocabulary out of a tool's arguments.
//
// Throws ToolError with a message naming what was wrong and listing the shapes that are accepted,
// because this is the argument a client is most likely to get wrong and the error is the only
// documentation it sees at the point of failure.
Selector parse_selector(const nlohmann::json &args);

// The triangles `selector` picks out of `mesh`.
//
// `to_frame` maps a mesh vertex into the frame the selector is written in; the caller composes it
// from the volume and instance transforms. `existing` is the per-facet paint state of the channel
// an Existing selector refers to, indexed by facet, and may be empty when nothing is painted.
//
// Kept free of Model, ModelVolume and anything else from the GUI so the geometry can be tested on
// a bare mesh.
std::vector<int> select_facets(const Selector             &selector,
                               const indexed_triangle_set &mesh,
                               const Transform3d          &to_frame,
                               const std::vector<uint8_t> &existing);

// How a selector reads back to a human, for a tool result that says what it actually did.
std::string describe(const Selector &selector);

}}} // namespace Slic3r::GUI::mcp

#endif // slic3r_McpSelectors_hpp_
