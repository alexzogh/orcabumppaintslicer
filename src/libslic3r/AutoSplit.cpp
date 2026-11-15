#include "AutoSplit.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <admesh/stl.h>
#include <boost/log/trivial.hpp>

#include "BoundingBox.hpp"
#include "BuildVolume.hpp"
#include "ClipperUtils.hpp"
#include "CutUtils.hpp"
#include "ExPolygon.hpp"
#include "Geometry.hpp"
#include "Geometry/Circle.hpp"
#include "Model.hpp"
#include "Point.hpp"
#include "Polygon.hpp"
#include "TriangleMesh.hpp"
#include "TriangleMeshSlicer.hpp"
#include "libslic3r.h"

namespace Slic3r {
namespace AutoSplit {

namespace {

constexpr double INF = std::numeric_limits<double>::infinity();

// A region of the model still to be considered, as the planner subdivides. `faces` are the
// faces of the whole mesh that reach into it, and `lo`/`hi` are the half-space bounds the
// cuts made so far have imposed on it. Both start unbounded.
struct Region
{
    std::vector<uint32_t> faces;
    Vec3d                 lo{-INF, -INF, -INF};
    Vec3d                 hi{INF, INF, INF};
};

// The extent of the geometry inside `region`.
//
// Vertices are clamped into the region before being merged, which makes the extent exact
// along every axis the region is bounded on — the axes a cut has already been made on, and
// so the ones the fit test turns on. On an unbounded axis a face that only partly reaches
// into the region still contributes its whole span, so the box can come out slightly large.
// That errs towards cutting once more than needed, never towards leaving a piece too big.
BoundingBoxf3 region_extent(const indexed_triangle_set &its, const Region &region)
{
    BoundingBoxf3 bb;
    for (uint32_t face : region.faces)
        for (int corner = 0; corner < 3; ++corner) {
            const Vec3d v = its.vertices[its.indices[face](corner)].cast<double>();
            bb.merge(v.cwiseMax(region.lo).cwiseMin(region.hi));
        }
    return bb;
}

// Split `region` at `position` along `axis`. A face that straddles the plane lands in both
// halves, which is what the cut itself does to it.
std::pair<Region, Region> split_region(const indexed_triangle_set &its, const Region &region, int axis, double position)
{
    std::pair<Region, Region> halves; // first = above the plane, second = below
    halves.first  = Region{{}, region.lo, region.hi};
    halves.second = Region{{}, region.lo, region.hi};
    halves.first.lo[axis]  = position;
    halves.second.hi[axis] = position;

    for (uint32_t face : region.faces) {
        double lo = INF;
        double hi = -INF;
        for (int corner = 0; corner < 3; ++corner) {
            const double c = double(its.vertices[its.indices[face](corner)](axis));
            lo             = std::min(lo, c);
            hi             = std::max(hi, c);
        }
        if (hi >= position)
            halves.first.faces.push_back(face);
        if (lo <= position)
            halves.second.faces.push_back(face);
    }
    return halves;
}

// The rotation taking `normal` onto +Z, so a cut plane becomes the plane z = 0.
Transform3d plane_to_xy(const CutPlane &plane)
{
    const Vec3d n = plane.normal.normalized();
    const Eigen::Quaterniond q = Eigen::Quaterniond::FromTwoVectors(n, Vec3d::UnitZ());
    return Transform3d(q) * Geometry::translation_transform(-plane.origin);
}

// Points spread over `region` at roughly `spacing` apart, at most `max_count` of them.
std::vector<Vec2d> sample_positions(const ExPolygons &region, double spacing, int max_count)
{
    std::vector<Vec2d> out;
    if (region.empty() || max_count <= 0)
        return out;

    const BoundingBox bb     = get_extents(region);
    const Point       centre = bb.center();

    auto scan = [&region, &bb, &centre](coord_t step, bool first_hit_only) {
        // A fine step over a large face would be an unbounded amount of work, so hold the
        // grid to a size that is still far denser than any sensible connector spacing.
        constexpr int max_points_per_axis = 128;
        step = std::max(step, coord_t(1 + std::max(bb.size().x(), bb.size().y()) / max_points_per_axis));

        std::vector<Vec2d> hits;
        const int nx = int(bb.size().x() / (2 * step)) + 1;
        const int ny = int(bb.size().y() / (2 * step)) + 1;
        for (int j = -ny; j <= ny; ++j)
            for (int i = -nx; i <= nx; ++i) {
                const Point p{centre.x() + coord_t(i) * step, centre.y() + coord_t(j) * step};
                if (!bb.contains(p))
                    continue;
                for (const ExPolygon &expoly : region)
                    if (expoly.contains(p, false)) {
                        hits.emplace_back(unscaled(p.x()), unscaled(p.y()));
                        break;
                    }
                if (first_hit_only && !hits.empty())
                    return hits;
            }
        return hits;
    };

    // Widen the grid until it yields few enough connectors, rather than dropping some and
    // leaving them bunched at one end of the face.
    double used = std::max(spacing, 0.1);
    for (int attempt = 0; attempt < 8; ++attempt) {
        out = scan(std::max<coord_t>(scaled<coord_t>(used), 1), false);
        if (int(out.size()) <= max_count)
            break;
        used *= std::sqrt(double(out.size()) / double(max_count)) * 1.05;
    }
    if (int(out.size()) > max_count)
        out.resize(max_count);

    // A long thin face can fall between grid lines even though it has room for a connector.
    if (out.empty())
        out = scan(std::max<coord_t>(scaled<coord_t>(used / 8.), 1), true);

    return out;
}

// Where connectors fit on `plane` within `region`: the cross-section of the solid there,
// clipped to the region and eroded so no connector breaks out through a wall.
std::vector<Connector> plan_connectors(const indexed_triangle_set &its,
                                       const CutPlane             &plane,
                                       const Region               &region,
                                       const ConnectorParams      &params)
{
    std::vector<Connector> connectors;
    if (!params.enabled || params.radius <= 0. || params.height <= 0.)
        return connectors;

    const Transform3d to_xy = plane_to_xy(plane);

    MeshSlicingParamsEx slicing;
    slicing.mode  = MeshSlicingParams::SlicingMode::Positive;
    slicing.trafo = to_xy;
    const std::vector<ExPolygons> sections = slice_mesh_ex(its, {0.f}, slicing);
    if (sections.empty() || sections.front().empty())
        return connectors;

    // The region is a box in cut space and the plane frame is a rigid transform of it, so
    // the eight transformed corners bound the part of the plane this piece owns.
    BoundingBoxf corner_box;
    for (int corner = 0; corner < 8; ++corner) {
        Vec3d p;
        for (int axis = 0; axis < 3; ++axis) {
            const double bound = (corner & (1 << axis)) ? region.hi[axis] : region.lo[axis];
            // An unbounded side cannot clip anything; push it far enough out to be inert.
            p[axis] = std::isinf(bound) ? std::copysign(1e7, bound) : bound;
        }
        const Vec3d q = to_xy * p;
        corner_box.merge(Vec2d(q.x(), q.y()));
    }
    const Polygon clip_rect = Polygon(Points{
        Point(scaled<coord_t>(corner_box.min.x()), scaled<coord_t>(corner_box.min.y())),
        Point(scaled<coord_t>(corner_box.max.x()), scaled<coord_t>(corner_box.min.y())),
        Point(scaled<coord_t>(corner_box.max.x()), scaled<coord_t>(corner_box.max.y())),
        Point(scaled<coord_t>(corner_box.min.x()), scaled<coord_t>(corner_box.max.y()))});

    const ExPolygons owned = intersection_ex(sections.front(), ExPolygon(clip_rect));
    if (owned.empty())
        return connectors;

    const ExPolygons seatable = offset_ex(owned, -scaled<float>(params.radius + params.wall_margin));
    if (seatable.empty())
        return connectors;

    const Transform3d from_xy = to_xy.inverse();
    for (const Vec2d &pos : sample_positions(seatable, params.spacing, params.max_per_plane)) {
        Connector connector;
        connector.pos    = from_xy * Vec3d(pos.x(), pos.y(), 0.);
        connector.radius = params.radius;
        connector.height = params.height;
        connectors.push_back(connector);
    }
    return connectors;
}

} // namespace

Transform3d cut_space_matrix(const ModelObject &object, int instance_idx, const ModelVolume &volume)
{
    return object.instances[instance_idx]->get_transformation().get_matrix_no_offset() * volume.get_matrix();
}

indexed_triangle_set cut_space_mesh(const ModelObject &object, int instance_idx)
{
    indexed_triangle_set merged;
    if (instance_idx < 0 || size_t(instance_idx) >= object.instances.size())
        return merged;

    for (const ModelVolume *volume : object.volumes) {
        if (!volume->is_model_part() || volume->is_cut_connector() || volume->mesh().empty())
            continue;
        indexed_triangle_set part = volume->mesh().its;
        its_transform(part, cut_space_matrix(object, instance_idx, *volume), true);
        its_merge(merged, part);
    }
    return merged;
}

Vec3d usable_size(const BuildVolume &build_volume, double clearance)
{
    const BoundingBoxf3 &box = build_volume.bounding_volume();
    Vec2d                plan(box.size().x(), box.size().y());

    // On a round bed the bounding box is mostly unusable, so take the inscribed square.
    if (build_volume.type() == BuildVolume_Type::Circle) {
        const double side = unscaled(double(build_volume.circle().radius)) * std::sqrt(2.);
        plan = Vec2d(side, side);
    }

    return Vec3d(std::max(0., plan.x() - 2. * clearance),
                 std::max(0., plan.y() - 2. * clearance),
                 std::max(0., build_volume.printable_height()));
}

const char *error_name(Error error)
{
    switch (error) {
    case Error::None:             return "none";
    case Error::EmptyBuildVolume: return "empty build volume";
    case Error::NoSuchInstance:   return "no such instance";
    case Error::PiecesTooThin:    return "pieces too thin";
    case Error::TooManyPieces:    return "too many pieces";
    case Error::CutFailed:        return "cut failed";
    case Error::MalformedPlan:    return "malformed plan";
    }
    return "unknown";
}

Plan plan_fit_build_plate(const indexed_triangle_set &its, const FitPlateParams &params, Error *error)
{
    Plan plan;
    auto fail = [&plan, error](Error reason) {
        plan.nodes.clear();
        if (error != nullptr)
            *error = reason;
        return plan;
    };

    if (error != nullptr)
        *error = Error::None;
    if (its.indices.empty())
        return plan;

    const Vec3d &limit = params.usable_size;
    if (limit.x() <= 0. || limit.y() <= 0. || limit.z() <= 0.)
        return fail(Error::EmptyBuildVolume);
    if (params.max_pieces < 1)
        return fail(Error::TooManyPieces);

    Region whole;
    whole.faces.resize(its.indices.size());
    for (size_t i = 0; i < its.indices.size(); ++i)
        whole.faces[i] = uint32_t(i);

    // Depth-first, so a region is split as soon as it is found to be too big. `parent` and
    // `parent_upper` say which child slot of which node this region came from.
    struct Pending
    {
        Region region;
        int    parent{-1};
        bool   parent_upper{false};
    };
    std::vector<Pending> pending;
    pending.push_back(Pending{std::move(whole), -1, false});

    while (!pending.empty()) {
        Pending current = std::move(pending.back());
        pending.pop_back();

        const BoundingBoxf3 extent = region_extent(its, current.region);
        if (!extent.defined)
            continue; // Nothing of the model reaches into this region.

        const Vec3d size = extent.size();

        // The axis that overshoots the plate by the largest factor is the one to cut. The
        // margin keeps a model that measures a hair over the plate from being cut in two.
        int    axis      = -1;
        double overshoot = 1. + 1e-9;
        for (int i = 0; i < 3; ++i) {
            const double ratio = size[i] / limit[i];
            if (ratio > overshoot) {
                overshoot = ratio;
                axis      = i;
            }
        }
        if (axis < 0)
            continue; // This piece already fits; it is a leaf.

        // Cutting this axis into `slabs` equal slices makes every slice fit. Cut it in the
        // middle of that run rather than slicing one plate-sized slab off the end, so the
        // pieces come out a similar size and the recursion stays balanced.
        const int slabs = int(std::ceil(size[axis] / limit[axis] - 1e-9));
        if (size[axis] / double(slabs) < params.min_piece_size)
            return fail(Error::PiecesTooThin);
        if (int(plan.nodes.size()) + 2 > params.max_pieces)
            return fail(Error::TooManyPieces);

        const double position = extent.min[axis] + size[axis] * double(slabs / 2) / double(slabs);

        PlanNode node;
        node.plane.origin        = extent.center();
        node.plane.origin[axis]  = position;
        node.plane.normal        = Vec3d::Unit(axis);
        node.connectors          = plan_connectors(its, node.plane, current.region, params.connectors);

        const int index = int(plan.nodes.size());
        plan.nodes.push_back(std::move(node));
        if (current.parent >= 0)
            (current.parent_upper ? plan.nodes[current.parent].upper : plan.nodes[current.parent].lower) = index;

        std::pair<Region, Region> halves = split_region(its, current.region, axis, position);
        pending.push_back(Pending{std::move(halves.second), index, false});
        pending.push_back(Pending{std::move(halves.first), index, true});
    }

    return plan;
}

namespace {

// Hang the plan's connectors on `object` the way the cut gizmo does, so Cut carves the
// matching hole and stud out of the two halves.
void attach_connectors(ModelObject &object, const PlanNode &node, const ConnectorParams &params)
{
    object.cut_connectors.clear();
    if (!params.enabled || node.connectors.empty())
        return;

    const Vec3d       normal   = node.plane.normal.normalized();
    const Transform3d rotation = Transform3d(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), normal));

    for (const Connector &planned : node.connectors) {
        CutConnector connector;
        connector.attribs    = params.attributes;
        connector.rotation_m = rotation;
        connector.radius     = float(planned.radius);
        connector.height     = float(planned.height);
        connector.radius_tolerance = 0.f;
        connector.height_tolerance = 0.f;
        connector.z_angle          = 0.f;
        connector.pos              = planned.pos;

        if (connector.attribs.type == CutConnectorType::Dowel) {
            if (connector.attribs.style == CutConnectorStyle::Prism)
                connector.height *= 2.f;
        } else {
            // A plug or snap sits on one side of the plane, so its centre is half a height up.
            connector.pos += normal * 0.5 * double(connector.height);
        }
        object.cut_connectors.push_back(connector);
    }

    size_t id = 0;
    for (const CutConnector &connector : object.cut_connectors) {
        ModelVolume *volume = object.add_volume(TriangleMesh(make_cut_connector_mesh(connector.attribs)),
                                                ModelVolumeType::NEGATIVE_VOLUME);
        volume->set_transformation(Geometry::translation_transform(connector.pos) * connector.rotation_m *
                                   Geometry::rotation_transform(-connector.z_angle * Vec3d::UnitZ()) *
                                   Geometry::scale_transform(
                                       Vec3f(connector.radius, connector.radius, connector.height).cast<double>()));
        volume->cut_info = {connector.attribs.type, connector.radius_tolerance, connector.height_tolerance};
        volume->name     = "Connector-" + std::to_string(++id);
    }
}

} // namespace

bool execute(const Plan          &plan,
             const ModelObject   &object,
             int                  instance_idx,
             const ExecuteParams &params,
             Model               &out_model,
             Error               *error)
{
    if (error != nullptr)
        *error = Error::None;
    auto fail = [error](Error reason) {
        if (error != nullptr)
            *error = reason;
        return false;
    };

    if (instance_idx < 0 || size_t(instance_idx) >= object.instances.size())
        return fail(Error::NoSuchInstance);

    if (plan.empty()) {
        out_model.add_object(object);
        return true;
    }

    // Cut owns the objects it hands back and frees them when it goes out of scope, so each
    // result is copied into `work` before the next cut runs.
    Model work;
    struct Pending
    {
        ModelObject *object;
        int          node;
        // Cut space is world space less the instance offset, so a piece whose instance
        // offset differs from the original's sees the remaining planes shifted by this.
        Vec3d shift;
    };
    auto instance_offset = [instance_idx](const ModelObject &mo) {
        return mo.instances[instance_idx]->get_offset();
    };

    const Vec3d                origin_offset = instance_offset(object);
    std::vector<Pending>       pending{{work.add_object(object), 0, Vec3d::Zero()}};
    std::vector<ModelObject *> pieces;

    while (!pending.empty()) {
        const Pending current = pending.back();
        pending.pop_back();

        if (current.node < 0) {
            pieces.push_back(current.object);
            continue;
        }
        if (size_t(current.node) >= plan.nodes.size())
            return fail(Error::MalformedPlan);

        const PlanNode &node = plan.nodes[current.node];
        PlanNode        shifted = node;
        shifted.plane.origin += current.shift;
        for (Connector &connector : shifted.connectors)
            connector.pos += current.shift;
        attach_connectors(*current.object, shifted, params.connectors);

        const Transform3d cut_matrix =
            Geometry::translation_transform(shifted.plane.origin) *
            Transform3d(Eigen::Quaterniond::FromTwoVectors(Vec3d::UnitZ(), shifted.plane.normal.normalized()));

        ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper |
                                              ModelObjectCutAttribute::KeepLower |
                                              ModelObjectCutAttribute::KeepPaint;

        ModelObject *upper = nullptr;
        ModelObject *lower = nullptr;
        {
            Cut                    cut(current.object, instance_idx, cut_matrix, attributes);
            const ModelObjectPtrs &halves = cut.perform_with_plane();
            if (halves.size() != 2)
                return fail(Error::CutFailed);
            upper = work.add_object(*halves[0]);
            lower = work.add_object(*halves[1]);
        }
        work.delete_object(current.object);

        pending.push_back(Pending{lower, node.lower, origin_offset - instance_offset(*lower)});
        pending.push_back(Pending{upper, node.upper, origin_offset - instance_offset(*upper)});
    }

    size_t number = 0;
    for (ModelObject *piece : pieces) {
        ModelObject *added = out_model.add_object(*piece);
        // The pieces are separate objects, not a cut set the cut gizmo can keep in sync.
        added->cut_id.invalidate();
        if (params.number_pieces)
            added->name = object.name + " - " + std::to_string(++number);
    }

    BOOST_LOG_TRIVIAL(info) << "AutoSplit: " << object.name << " split into " << pieces.size() << " pieces";
    return true;
}

} // namespace AutoSplit
} // namespace Slic3r
