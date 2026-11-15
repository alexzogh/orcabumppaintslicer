#include "AutoSplit.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <Eigen/Geometry>
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

// A piece of the model, as the set of points satisfying every `normal · p >= offset`.
// Both strategies produce regions of this shape: a box is six of them, and a colour split
// is one per cut made so far.
struct Halfspace
{
    Vec3d  normal;
    double offset;
};

std::vector<Halfspace> halfspaces_of(const Region &region)
{
    std::vector<Halfspace> bounds;
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isinf(region.lo[axis]))
            bounds.push_back(Halfspace{Vec3d::Unit(axis), region.lo[axis]});
        if (!std::isinf(region.hi[axis]))
            bounds.push_back(Halfspace{-Vec3d::Unit(axis), -region.hi[axis]});
    }
    return bounds;
}

// The half-plane `normal · p >= offset` of the cut plane's own 2D frame, as a polygon big
// enough to cover any cross-section. Wound counter-clockwise, which is what Clipper wants.
Polygon halfplane_polygon(const Vec2d &normal, double offset)
{
    constexpr double far_away = 1.e6; // mm; a model is far smaller, and this still scales safely
    const Vec2d      unit     = normal.normalized();
    const Vec2d      on_line  = unit * (offset / normal.norm());
    const Vec2d      along(unit.y(), -unit.x());

    Points points;
    for (const Vec2d &p : {Vec2d(on_line - along * far_away), Vec2d(on_line + along * far_away),
                           Vec2d(on_line + along * far_away + unit * far_away),
                           Vec2d(on_line - along * far_away + unit * far_away)})
        points.emplace_back(scaled<coord_t>(p.x()), scaled<coord_t>(p.y()));
    return Polygon(points);
}

// Where connectors fit on `plane` within `region`: the cross-section of the solid there,
// clipped to the region and eroded so no connector breaks out through a wall.
std::vector<Connector> plan_connectors(const indexed_triangle_set   &its,
                                       const CutPlane               &plane,
                                       const std::vector<Halfspace> &region,
                                       const ConnectorParams        &params)
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

    // Cut the cross-section down to the part of the plane this piece owns. A half-space of
    // cut space meets the plane in a half-plane of the plane's own frame: a point (x, y) of
    // that frame is R⁻¹(x, y, 0) + origin, so `normal · p >= offset` becomes
    // `(R normal) · (x, y) >= offset - normal · origin`.
    ExPolygons owned = sections.front();
    for (const Halfspace &bound : region) {
        const Vec3d  mapped = to_xy.linear() * bound.normal;
        const double rhs    = bound.offset - bound.normal.dot(plane.origin);
        const Vec2d  planar(mapped.x(), mapped.y());

        if (planar.norm() < 1e-12) {
            // Parallel to the plane: the whole cross-section is either inside or outside.
            if (rhs > 1e-9)
                return connectors;
            continue;
        }
        owned = intersection_ex(owned, ExPolygon(halfplane_polygon(planar, rhs)));
        if (owned.empty())
            return connectors;
    }
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
    case Error::NothingToSeparate: return "nothing to separate";
    }
    return "unknown";
}

Plan plan_fit_build_plate(const indexed_triangle_set &its, const FitPlateParams &params, Error *error)
{
    Plan plan;
    auto fail = [&plan, error](Error reason) {
        plan.nodes.clear();
        plan.pieces.clear();
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

    // Hand the region back as a finished piece, and tell its parent which piece it is.
    auto finish_piece = [&plan](const Pending &region) {
        const int branch = piece_to_branch(int(plan.pieces.size()));
        plan.pieces.push_back(Piece{});
        if (region.parent >= 0)
            (region.parent_upper ? plan.nodes[region.parent].upper : plan.nodes[region.parent].lower) = branch;
    };

    while (!pending.empty()) {
        Pending current = std::move(pending.back());
        pending.pop_back();

        const BoundingBoxf3 extent = region_extent(its, current.region);
        if (!extent.defined) {
            // Nothing of the model reaches into this region, but the cut still made two
            // objects, so the empty side is still a piece as far as the tree is concerned.
            finish_piece(current);
            continue;
        }

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
        if (axis < 0) {
            finish_piece(current); // This region already fits; it is a piece.
            continue;
        }

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
        node.connectors          = plan_connectors(its, node.plane, halfspaces_of(current.region), params.connectors);

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

// Splitting by painted colour
// ---------------------------

std::vector<PaintedFacet> painted_surface(const ModelObject &object, int instance_idx)
{
    std::vector<PaintedFacet> facets;
    if (instance_idx < 0 || size_t(instance_idx) >= object.instances.size())
        return facets;

    for (const ModelVolume *volume : object.volumes) {
        if (!volume->is_model_part() || volume->is_cut_connector() || volume->mesh().empty())
            continue;

        const Transform3d to_cut_space = cut_space_matrix(object, instance_idx, *volume);
        // An unpainted facet prints in whatever filament the volume itself asks for.
        const int base_extruder = std::max(1, volume->extruder_id());

        // One mesh per paint state. Index 0 is the unpainted remainder; index N is the
        // surface painted with ExtruderN, already subdivided where a facet is part painted.
        std::vector<indexed_triangle_set> per_state;
        volume->mmu_segmentation_facets.get_facets(*volume, per_state);

        for (size_t state = 0; state < per_state.size(); ++state) {
            indexed_triangle_set patch = per_state[state];
            if (patch.indices.empty())
                continue;
            its_transform(patch, to_cut_space, true);

            const int extruder = state == 0 ? base_extruder : int(state);
            for (const Vec3i32 &face : patch.indices) {
                const Vec3d a = patch.vertices[face(0)].cast<double>();
                const Vec3d b = patch.vertices[face(1)].cast<double>();
                const Vec3d c = patch.vertices[face(2)].cast<double>();

                PaintedFacet painted;
                painted.centre   = (a + b + c) / 3.;
                painted.area     = 0.5 * (b - a).cross(c - a).norm();
                painted.extruder = extruder;
                if (painted.area > 0.)
                    facets.push_back(painted);
            }
        }
    }
    return facets;
}

std::vector<Vec3d> default_split_directions()
{
    std::vector<Vec3d> directions{Vec3d::UnitX(), Vec3d::UnitY(), Vec3d::UnitZ()};
    const double       r2 = 1. / std::sqrt(2.);
    for (const Vec3d &d : {Vec3d(r2, r2, 0.), Vec3d(r2, -r2, 0.), Vec3d(r2, 0., r2), Vec3d(r2, 0., -r2),
                           Vec3d(0., r2, r2), Vec3d(0., r2, -r2)})
        directions.push_back(d);
    const double r3 = 1. / std::sqrt(3.);
    for (const Vec3d &d : {Vec3d(r3, r3, r3), Vec3d(r3, r3, -r3), Vec3d(r3, -r3, r3), Vec3d(-r3, r3, r3)})
        directions.push_back(d);
    return directions;
}

namespace {

// How well one plane separates the colours of a region.
struct Separation
{
    CutPlane plane;
    // Painted area that would end up on the piece of the wrong colour.
    double misplaced{INF};
    bool   found{false};
};

// Area per colour, and which colour has the most of it.
struct ColourAreas
{
    std::map<int, double> by_extruder;
    double                total{0.};

    int dominant() const
    {
        int    best      = 0;
        double best_area = -1.;
        for (const auto &[extruder, area] : by_extruder)
            if (area > best_area) {
                best_area = area;
                best      = extruder;
            }
        return best;
    }
};

ColourAreas colour_areas(const std::vector<PaintedFacet> &facets, const std::vector<uint32_t> &region)
{
    ColourAreas areas;
    for (const uint32_t i : region) {
        areas.by_extruder[facets[i].extruder] += facets[i].area;
        areas.total += facets[i].area;
    }
    return areas;
}

// The colours worth separating: the ones covering more than a trifling part of the region.
// A handful of stray facets is not a reason to cut a model in two.
size_t significant_colours(const ColourAreas &areas, double ignore_fraction)
{
    size_t count = 0;
    for (const auto &[extruder, area] : areas.by_extruder)
        if (area > areas.total * ignore_fraction)
            ++count;
    return count;
}

// The plane along `direction` that leaves the least painted area on the wrong side.
//
// Facets are binned by the projection of their centre, so one scan of the prefix sums
// gives the best split position over the whole direction rather than a local search.
Separation best_plane_along(const std::vector<PaintedFacet> &facets,
                            const std::vector<uint32_t>     &region,
                            const Vec3d                     &direction,
                            int                              samples)
{
    Separation best;
    if (region.empty() || samples < 2)
        return best;

    const Vec3d normal = direction.normalized();

    double lo = INF;
    double hi = -INF;
    for (const uint32_t i : region) {
        const double d = normal.dot(facets[i].centre);
        lo             = std::min(lo, d);
        hi             = std::max(hi, d);
    }
    if (!(hi > lo))
        return best;

    // Collect the colours present so each gets a row of the histogram.
    std::map<int, size_t> row_of;
    for (const uint32_t i : region)
        row_of.emplace(facets[i].extruder, row_of.size());

    const size_t rows = row_of.size();
    if (rows < 2)
        return best;

    const size_t bins = size_t(samples);
    std::vector<double> histogram(rows * bins, 0.);
    const double        span = hi - lo;
    for (const uint32_t i : region) {
        const double d   = normal.dot(facets[i].centre);
        size_t       bin = size_t((d - lo) / span * double(bins));
        bin              = std::min(bin, bins - 1);
        histogram[row_of[facets[i].extruder] * bins + bin] += facets[i].area;
    }

    std::vector<double> total_of_row(rows, 0.);
    for (size_t row = 0; row < rows; ++row)
        for (size_t bin = 0; bin < bins; ++bin)
            total_of_row[row] += histogram[row * bins + bin];

    // Sweep the split point, keeping a running total of what is below it.
    std::vector<double> below(rows, 0.);
    for (size_t bin = 0; bin + 1 < bins; ++bin) {
        for (size_t row = 0; row < rows; ++row)
            below[row] += histogram[row * bins + bin];

        double misplaced = 0.;
        for (size_t row = 0; row < rows; ++row)
            misplaced += std::min(below[row], total_of_row[row] - below[row]);

        if (misplaced < best.misplaced) {
            best.misplaced    = misplaced;
            best.found        = true;
            const double at   = lo + span * double(bin + 1) / double(bins);
            best.plane.origin = normal * at;
            best.plane.normal = normal;
        }
    }
    return best;
}

} // namespace

Plan plan_by_painted_color(const ModelObject &object, int instance_idx, const ColorSplitParams &params, Error *error)
{
    Plan plan;
    auto fail = [&plan, error](Error reason) {
        plan.nodes.clear();
        plan.pieces.clear();
        if (error != nullptr)
            *error = reason;
        return plan;
    };

    if (error != nullptr)
        *error = Error::None;
    if (instance_idx < 0 || size_t(instance_idx) >= object.instances.size())
        return fail(Error::NoSuchInstance);
    if (params.max_pieces < 1)
        return fail(Error::TooManyPieces);

    const std::vector<PaintedFacet> facets = painted_surface(object, instance_idx);
    if (facets.empty())
        return fail(Error::NothingToSeparate);

    std::vector<uint32_t> whole(facets.size());
    for (size_t i = 0; i < facets.size(); ++i)
        whole[i] = uint32_t(i);

    if (significant_colours(colour_areas(facets, whole), params.ignore_colour_fraction) < 2)
        return fail(Error::NothingToSeparate);

    std::vector<Vec3d> directions = params.directions;
    if (directions.empty())
        directions = {Vec3d::UnitX(), Vec3d::UnitY(), Vec3d::UnitZ()};

    const indexed_triangle_set mesh = cut_space_mesh(object, instance_idx);

    struct Pending
    {
        std::vector<uint32_t> region;
        // The cuts already made that bound this piece, so its connectors stay inside it.
        std::vector<Halfspace> bounds;
        int                    parent{-1};
        bool                   parent_upper{false};
    };
    std::vector<Pending> pending;
    pending.push_back(Pending{std::move(whole), {}, -1, false});

    auto finish_piece = [&plan, &facets](const Pending &region) {
        Piece piece;
        piece.extruder  = colour_areas(facets, region.region).dominant();
        const int branch = piece_to_branch(int(plan.pieces.size()));
        plan.pieces.push_back(piece);
        if (region.parent >= 0)
            (region.parent_upper ? plan.nodes[region.parent].upper : plan.nodes[region.parent].lower) = branch;
    };

    while (!pending.empty()) {
        Pending current = std::move(pending.back());
        pending.pop_back();

        const ColourAreas areas = colour_areas(facets, current.region);
        if (significant_colours(areas, params.ignore_colour_fraction) < 2 || areas.total <= 0.) {
            finish_piece(current);
            continue;
        }

        Separation best;
        for (const Vec3d &direction : directions) {
            const Separation candidate = best_plane_along(facets, current.region, direction, params.position_samples);
            if (candidate.found && candidate.misplaced < best.misplaced)
                best = candidate;
        }

        // No plane separates these colours well enough to be worth a cut. That is the
        // honest answer for paint a plane cannot follow, such as a logo on a flat face:
        // leave the piece whole rather than cut it somewhere that means nothing.
        const bool hopeless = !best.found || best.misplaced > areas.total * params.hopeless_fraction;
        if (hopeless || int(plan.nodes.size()) + 2 > params.max_pieces) {
            finish_piece(current);
            continue;
        }

        PlanNode node;
        node.plane      = best.plane;
        node.connectors = plan_connectors(mesh, node.plane, current.bounds, params.connectors);

        const int index = int(plan.nodes.size());
        plan.nodes.push_back(std::move(node));
        if (current.parent >= 0)
            (current.parent_upper ? plan.nodes[current.parent].upper : plan.nodes[current.parent].lower) = index;

        const Vec3d  normal = best.plane.normal;
        const double offset = normal.dot(best.plane.origin);

        Pending upper{{}, current.bounds, index, true};
        Pending lower{{}, current.bounds, index, false};
        upper.bounds.push_back(Halfspace{normal, offset});
        lower.bounds.push_back(Halfspace{-normal, -offset});

        for (const uint32_t i : current.region)
            (normal.dot(facets[i].centre) >= offset ? upper.region : lower.region).push_back(i);

        if (upper.region.empty() || lower.region.empty()) {
            // The sweep found a split with something on both sides, so this cannot happen;
            // if it somehow does, stop rather than loop on an unchanged region.
            plan.nodes.pop_back();
            if (current.parent >= 0)
                (current.parent_upper ? plan.nodes[current.parent].upper : plan.nodes[current.parent].lower) = -1;
            finish_piece(current);
            continue;
        }

        pending.push_back(std::move(lower));
        pending.push_back(std::move(upper));
    }

    if (plan.nodes.empty()) {
        // Every colour was already on its own, so there is nothing to cut.
        plan.pieces.clear();
        return plan;
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

    const Vec3d          origin_offset = instance_offset(object);
    std::vector<Pending> pending{{work.add_object(object), 0, Vec3d::Zero()}};
    // Indexed by piece, so the caller can line a result up with Plan::pieces.
    std::vector<ModelObject *> pieces(plan.pieces.size(), nullptr);

    while (!pending.empty()) {
        const Pending current = pending.back();
        pending.pop_back();

        if (branch_is_piece(current.node)) {
            const int index = branch_to_piece(current.node);
            if (index < 0 || size_t(index) >= pieces.size() || pieces[index] != nullptr)
                return fail(Error::MalformedPlan);
            pieces[index] = current.object;
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
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (pieces[i] == nullptr)
            return fail(Error::MalformedPlan);

        ModelObject *added = out_model.add_object(*pieces[i]);
        // The pieces are separate objects, not a cut set the cut gizmo can keep in sync.
        added->cut_id.invalidate();
        if (params.number_pieces)
            added->name = object.name + " - " + std::to_string(++number);

        const int extruder = plan.pieces[i].extruder;
        if (params.apply_piece_extruder && extruder > 0) {
            added->config.set("extruder", extruder);
            // The paint said which filament each facet wanted; the piece now prints in one
            // filament, so leaving the paint on would print the colours it was split to
            // separate.
            if (params.clear_paint_on_coloured_pieces)
                for (ModelVolume *volume : added->volumes)
                    volume->mmu_segmentation_facets.reset();
        }
    }

    BOOST_LOG_TRIVIAL(info) << "AutoSplit: " << object.name << " split into " << pieces.size() << " pieces";
    return true;
}

} // namespace AutoSplit
} // namespace Slic3r
