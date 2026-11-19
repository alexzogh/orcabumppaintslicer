#include "McpGeometry.hpp"

#include <algorithm>
#include <limits>
#include <utility>

#include <boost/beast/core/detail/base64.hpp>

#include "libslic3r/QuadricEdgeCollapse.hpp"

using nlohmann::json;

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

// FNV-1a, 64-bit. Chosen because it needs no dependency, no state and no buffering, and because a
// fingerprint here only has to notice that geometry changed - a client that mistakes one mesh for
// another sees a wrong picture, not a wrong slice.
constexpr std::uint64_t fnv_offset_basis = 14695981039346656037ull;
constexpr std::uint64_t fnv_prime        = 1099511628211ull;

void hash_bytes(std::uint64_t &hash, const void *data, std::size_t size)
{
    const auto *bytes = static_cast<const unsigned char *>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= std::uint64_t(bytes[i]);
        hash *= fnv_prime;
    }
}

void hash_number(std::uint64_t &hash, std::uint64_t value)
{
    hash_bytes(hash, &value, sizeof(value));
}

std::string to_hex16(std::uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    std::string       out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[std::size_t(i)] = digits[value & 0x0f];
        value >>= 4;
    }
    return out;
}

// One buffer, described rather than merely dumped: a client that has never seen this server should
// be able to read the element type and the counts off the payload instead of out of a document.
json buffer_json(const char *type, int components, std::size_t count, const void *data, std::size_t size)
{
    return json{{"type", type},
                {"components", components},
                {"count", count},
                {"encoding", "base64"},
                {"byte_length", size},
                {"data", base64_encode(data, size)}};
}

} // namespace

std::string base64_encode(const void *data, std::size_t size)
{
    if (data == nullptr || size == 0)
        return {};
    std::string out;
    out.resize(boost::beast::detail::base64::encoded_size(size));
    out.resize(boost::beast::detail::base64::encode(&out[0], data, size));
    return out;
}

std::string mesh_fingerprint(const indexed_triangle_set &mesh)
{
    std::uint64_t hash = fnv_offset_basis;
    // The counts go in as well as the data. Without them a mesh and a differently split mesh with
    // the same bytes laid end to end would fingerprint alike.
    hash_number(hash, std::uint64_t(mesh.vertices.size()));
    hash_number(hash, std::uint64_t(mesh.indices.size()));
    if (!mesh.vertices.empty())
        hash_bytes(hash, mesh.vertices.data(), mesh.vertices.size() * sizeof(stl_vertex));
    if (!mesh.indices.empty())
        hash_bytes(hash, mesh.indices.data(), mesh.indices.size() * sizeof(stl_triangle_vertex_indices));
    return to_hex16(hash);
}

std::string mesh_key(std::uint64_t volume_id, const indexed_triangle_set &mesh)
{
    return std::to_string(volume_id) + "-" + mesh_fingerprint(mesh);
}

indexed_triangle_set simplify_to(indexed_triangle_set mesh, std::size_t max_triangles)
{
    if (max_triangles == 0 || mesh.indices.size() <= max_triangles)
        return mesh;
    // Below a handful of triangles there is nothing left to look at, and the collapse has no room
    // to keep the shape closed.
    const std::size_t target = std::max<std::size_t>(max_triangles, 4);
    its_quadric_edge_collapse(mesh, std::uint32_t(target));
    return mesh;
}

json encode_mesh(const indexed_triangle_set &mesh)
{
    Vec3f min = Vec3f::Constant(std::numeric_limits<float>::max());
    Vec3f max = Vec3f::Constant(std::numeric_limits<float>::lowest());
    for (const stl_vertex &vertex : mesh.vertices) {
        min = min.cwiseMin(vertex);
        max = max.cwiseMax(vertex);
    }
    if (mesh.vertices.empty()) {
        min = Vec3f::Zero();
        max = Vec3f::Zero();
    }

    return json{
        {"triangles", mesh.indices.size()},
        {"vertices", mesh.vertices.size()},
        {"bbox_min", {min.x(), min.y(), min.z()}},
        {"bbox_max", {max.x(), max.y(), max.z()}},
        {"positions", buffer_json("float32", 3, mesh.vertices.size(), mesh.vertices.data(),
                                  mesh.vertices.size() * sizeof(stl_vertex))},
        {"indices", buffer_json("int32", 3, mesh.indices.size(), mesh.indices.data(),
                                mesh.indices.size() * sizeof(stl_triangle_vertex_indices))}};
}

json transform_json(const Transform3d &transform)
{
    // Read out column by column explicitly rather than handing over the storage, so the order is a
    // property of this function and not of however Eigen happens to be configured.
    const Matrix4d matrix = transform.matrix();
    json           out    = json::array();
    for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row)
            out.push_back(matrix(row, column));
    return out;
}

json bed_json(const std::vector<Vec2d> &printable_area, double printable_height, const std::string &type)
{
    json polygon = json::array();
    for (const Vec2d &point : printable_area)
        polygon.push_back(json{point.x(), point.y()});
    return json{{"type", type}, {"printable_height_mm", printable_height}, {"polygon", std::move(polygon)}};
}

}}} // namespace Slic3r::GUI::mcp
