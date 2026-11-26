#ifndef slic3r_McpGeometry_hpp_
#define slic3r_McpGeometry_hpp_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <admesh/stl.h>

#include "libslic3r/Point.hpp"

namespace Slic3r { namespace GUI { namespace mcp {

// Turning slicer geometry into something a renderer on the other end of a socket can draw.
//
// Kept free of Model, Plater and wxWidgets, like McpSelectors and for the same reason: all of the
// fiddly parts - how a buffer is laid out, what makes two meshes the same mesh, what decimation
// does to the counts - are properties of the data, and testing them should not need a GUI.
//
// Everything here is bytes and arithmetic, so it is safe to call off the GUI thread. That matters:
// a tool must hold the GUI thread only long enough to take a reference to the mesh, and do the
// hashing, decimating and encoding afterwards, where a slow mesh cannot freeze the application.

// Base64 of a raw byte range. The encoding every buffer below travels in, because a tool result is
// JSON text and a few hundred thousand numbers spelled out as decimal is not a payload.
std::string base64_encode(const void *data, std::size_t size);

// A 64-bit fingerprint of a mesh's geometry as 16 lowercase hex characters.
//
// Covers the vertex and index data and the counts of both, so a mesh that keeps its triangle count
// through an edit still fingerprints differently. Not a cryptographic hash and not meant to resist
// one: it answers "is this the same geometry I already have", nothing more.
std::string mesh_fingerprint(const indexed_triangle_set &mesh);

// The opaque key a client caches a mesh under.
//
// Carries the volume id as well as the fingerprint so two volumes that happen to hold identical
// geometry - the common case for a duplicated part - stay distinguishable in a client's cache,
// which is keyed by what it asked for rather than by what came back.
std::string mesh_key(std::uint64_t volume_id, const indexed_triangle_set &mesh);

// Reduces `mesh` to at most `max_triangles`, by quadric edge collapse.
//
// Returns it untouched when max_triangles is 0, or when the mesh already fits. The result is an
// approximation: it is for looking at, never for measuring.
indexed_triangle_set simplify_to(indexed_triangle_set mesh, std::size_t max_triangles);

// The buffers and counts a renderer needs, as a self-describing object: each buffer states its
// element type, how many components make an element, how many elements there are, and the base64
// of the raw little-endian bytes.
//
// Positions are float32 triples in the mesh's own coordinates, millimetres. Indices are int32
// triples into the position buffer. No normals: a renderer has to choose between flat and smooth
// shading anyway, and that choice belongs at the end that is drawing.
nlohmann::json encode_mesh(const indexed_triangle_set &mesh);

// A 4x4 transform as the 16 numbers of its column-major storage, which is the order OpenGL, Metal
// and SceneKit all take a matrix in. Writing it row-major instead produces a scene that renders
// without complaint and is wrong, so the order is stated here and in the tool description.
nlohmann::json transform_json(const Transform3d &transform);

// The plate a client draws the objects on: the printable area as a closed polygon in millimetres,
// how high the printer can print, and the shape's name.
nlohmann::json bed_json(const std::vector<Vec2d> &printable_area, double printable_height, const std::string &type);

}}} // namespace Slic3r::GUI::mcp

#endif // slic3r_McpGeometry_hpp_
