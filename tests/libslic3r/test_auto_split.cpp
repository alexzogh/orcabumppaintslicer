#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "libslic3r/AutoSplit.hpp"
#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/BuildVolume.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/libslic3r.h"
#include "test_utils.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// A box of the given size, sitting with its centre on the origin.
indexed_triangle_set centred_box(double x, double y, double z)
{
    TriangleMesh mesh = make_cube(x, y, z);
    mesh.translate(float(-x / 2.), float(-y / 2.), float(-z / 2.));
    return mesh.its;
}

AutoSplit::FitPlateParams plate(double x, double y, double z)
{
    AutoSplit::FitPlateParams params;
    params.usable_size          = Vec3d(x, y, z);
    params.connectors.enabled   = false; // Connector placement is exercised on its own.
    return params;
}

// Walk the plan the way execute() does and report the box each finished piece occupies,
// starting from `whole`. A plan is only correct if every one of these fits the plate.
// The axis an axis-aligned plane normal points along.
int axis_of(const Vec3d &normal)
{
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(normal[i]) > std::abs(normal[best]))
            best = i;
    return best;
}

void collect_pieces(const AutoSplit::Plan &plan, int node, const BoundingBoxf3 &region, std::vector<BoundingBoxf3> &out)
{
    if (node < 0) {
        out.push_back(region);
        return;
    }
    const AutoSplit::PlanNode &step = plan.nodes[size_t(node)];

    const int    axis     = axis_of(step.plane.normal);
    const double position = step.plane.origin[axis];

    BoundingBoxf3 upper = region;
    BoundingBoxf3 lower = region;
    upper.min[axis]     = position;
    lower.max[axis]     = position;

    collect_pieces(plan, step.upper, upper, out);
    collect_pieces(plan, step.lower, lower, out);
}

std::vector<BoundingBoxf3> pieces_of(const AutoSplit::Plan &plan, const indexed_triangle_set &its)
{
    BoundingBoxf3 whole;
    for (const Vec3f &v : its.vertices)
        whole.merge(v.cast<double>());

    std::vector<BoundingBoxf3> pieces;
    if (plan.empty())
        pieces.push_back(whole);
    else
        collect_pieces(plan, 0, whole, pieces);
    return pieces;
}

// Checked one axis at a time so a failure says which axis did not fit.
void check_fits(const BoundingBoxf3 &box, const Vec3d &limit)
{
    const Vec3d size = box.size();
    CHECK(size.x() <= limit.x() + 1e-6);
    CHECK(size.y() <= limit.y() + 1e-6);
    CHECK(size.z() <= limit.z() + 1e-6);
}

ModelObject *add_box_object(Model &model, double x, double y, double z)
{
    ModelObject *object = model.add_object();
    object->name        = "box";
    TriangleMesh mesh   = make_cube(x, y, z);
    mesh.translate(float(-x / 2.), float(-y / 2.), float(-z / 2.));
    object->add_volume(std::move(mesh));
    object->add_instance();
    return object;
}

// Paints every facet lying wholly on the object's low-X face with `paint`. On a plain box
// that is the end cap, so the model carries two colours separated by a plane.
void paint_low_x_face(ModelVolume *volume, EnforcerBlockerType paint)
{
    const indexed_triangle_set &its   = volume->mesh().its;
    float                       min_x = std::numeric_limits<float>::max();
    for (const Vec3f &vertex : its.vertices)
        min_x = std::min(min_x, vertex.x());

    TriangleSelector selector(volume->mesh());
    int              painted = 0;
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const Vec3i32 &facet = its.indices[i];
        const bool     on_face = its.vertices[facet(0)].x() < min_x + 0.01f &&
                             its.vertices[facet(1)].x() < min_x + 0.01f &&
                             its.vertices[facet(2)].x() < min_x + 0.01f;
        if (on_face) {
            selector.set_facet(int(i), paint);
            ++painted;
        }
    }
    REQUIRE(painted > 0);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
}

ModelObject *add_two_colour_box(Model &model, double x = 100., double y = 40., double z = 40.)
{
    ModelObject *object = add_box_object(model, x, y, z);
    paint_low_x_face(object->volumes.front(), EnforcerBlockerType::Extruder2);
    return object;
}

// Both end caps painted and the sides left bare, so the second colour sits at each end of
// the model with the first between them. One plane cannot separate those.
ModelObject *add_box_painted_at_both_ends(Model &model, double x = 100., double y = 40., double z = 40.)
{
    ModelObject                *object = add_box_object(model, x, y, z);
    const indexed_triangle_set &its    = object->volumes.front()->mesh().its;

    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    for (const Vec3f &vertex : its.vertices) {
        min_x = std::min(min_x, vertex.x());
        max_x = std::max(max_x, vertex.x());
    }

    TriangleSelector selector(object->volumes.front()->mesh());
    int              painted = 0;
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const Vec3i32 &facet = its.indices[i];
        const auto     at    = [&](float plane) {
            return std::abs(its.vertices[facet(0)].x() - plane) < 0.01f &&
                   std::abs(its.vertices[facet(1)].x() - plane) < 0.01f &&
                   std::abs(its.vertices[facet(2)].x() - plane) < 0.01f;
        };
        if (at(min_x) || at(max_x)) {
            selector.set_facet(int(i), EnforcerBlockerType::Extruder2);
            ++painted;
        }
    }
    REQUIRE(painted > 0);
    REQUIRE(object->volumes.front()->mmu_segmentation_facets.set(selector));
    return object;
}

AutoSplit::ColorSplitParams colour_params()
{
    AutoSplit::ColorSplitParams params;
    params.connectors.enabled = false;
    return params;
}

std::vector<int> piece_extruders(const AutoSplit::Plan &plan)
{
    std::vector<int> extruders;
    for (const AutoSplit::Piece &piece : plan.pieces)
        extruders.push_back(piece.extruder);
    std::sort(extruders.begin(), extruders.end());
    return extruders;
}

} // namespace

TEST_CASE("The usable size of a rectangular bed is the bed less the clearance", "[AutoSplit]")
{
    const std::vector<Vec2d> bed{{0., 0.}, {256., 0.}, {256., 256.}, {0., 256.}};
    const BuildVolume        build_volume(bed, 256., {}, {});

    const Vec3d usable = AutoSplit::usable_size(build_volume, 5.);

    CHECK_THAT(usable.x(), WithinAbs(246., 1e-6)); // 256 less 5 mm at each end
    CHECK_THAT(usable.y(), WithinAbs(246., 1e-6));
    // Height is not shrunk: nothing arranges in Z.
    CHECK_THAT(usable.z(), WithinAbs(256., 1e-6));
}

TEST_CASE("The usable size of a round bed is its inscribed square", "[AutoSplit]")
{
    // A 250 mm circular bed as the bed shape editor writes one, at 72 segments — which is
    // also the resolution BuildVolume needs to recognise it as a circle rather than a
    // polygon. Its bounding box is far wider than any square that fits inside it.
    const double       radius = 125.;
    std::vector<Vec2d> bed;
    for (int i = 0; i < 72; ++i) {
        const double angle = 2. * PI * double(i) / 72.;
        bed.emplace_back(radius * std::cos(angle), radius * std::sin(angle));
    }
    const BuildVolume build_volume(bed, 300., {}, {});
    REQUIRE(build_volume.type() == BuildVolume_Type::Circle);

    const Vec3d usable = AutoSplit::usable_size(build_volume, 0.);

    CHECK(usable.x() < 2. * radius);
    CHECK_THAT(usable.x(), WithinAbs(usable.y(), 1e-6));
    // A square of side r * sqrt(2) is the largest that fits in a circle of radius r.
    CHECK_THAT(usable.x(), WithinAbs(radius * std::sqrt(2.), 1.));
}

TEST_CASE("A model that already fits the plate is not cut", "[AutoSplit]")
{
    const indexed_triangle_set box = centred_box(100., 100., 100.);

    AutoSplit::Error error = AutoSplit::Error::CutFailed;
    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(box, plate(200., 200., 200.), &error);

    CHECK(plan.empty());
    CHECK(plan.piece_count() == 1);
    CHECK(error == AutoSplit::Error::None);
}

TEST_CASE("A model just over the plate is cut once", "[AutoSplit]")
{
    // 210 mm along X on a 200 mm plate needs two slabs, so one cut.
    const indexed_triangle_set box = centred_box(210., 100., 100.);

    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(box, plate(200., 200., 200.));

    REQUIRE(plan.nodes.size() == 1);
    CHECK(plan.piece_count() == 2);
    CHECK(axis_of(plan.nodes[0].plane.normal) == X);
    // Two equal slabs, so the cut lands on the middle of the box.
    CHECK_THAT(plan.nodes[0].plane.origin.x(), WithinAbs(0., 1e-6));
}

TEST_CASE("Every piece of a planned split fits the plate", "[AutoSplit]")
{
    const double x = GENERATE(90., 210., 640., 1000.);
    const double y = GENERATE(90., 450.);
    const double z = GENERATE(90., 300.);

    const indexed_triangle_set   box    = centred_box(x, y, z);
    const AutoSplit::FitPlateParams params = plate(200., 200., 250.);

    AutoSplit::Error error = AutoSplit::Error::CutFailed;
    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(box, params, &error);
    REQUIRE(error == AutoSplit::Error::None);

    const std::vector<BoundingBoxf3> pieces = pieces_of(plan, box);
    CHECK(pieces.size() == plan.piece_count());
    for (const BoundingBoxf3 &piece : pieces)
        check_fits(piece, params.usable_size);
}

TEST_CASE("A split uses no more pieces than slicing each axis separately would", "[AutoSplit]")
{
    // 640 x 450 x 90 on a 200 x 200 x 250 plate needs 4 slabs in X and 3 in Y: 12 pieces.
    const indexed_triangle_set box = centred_box(640., 450., 90.);

    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(box, plate(200., 200., 250.));

    CHECK(plan.piece_count() == 12);
}

TEST_CASE("The cut axis is the one that overshoots the plate most", "[AutoSplit]")
{
    // X is 1.5 plates long, Z is 2.5, so Z goes first even though X is the longer side.
    const indexed_triangle_set box = centred_box(300., 50., 250.);

    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(box, plate(200., 200., 100.));

    REQUIRE(!plan.empty());
    CHECK(axis_of(plan.nodes[0].plane.normal) == Z);
}

TEST_CASE("Planning a split refuses rather than returning a plan it cannot keep", "[AutoSplit]")
{
    const indexed_triangle_set box = centred_box(1000., 1000., 1000.);

    SECTION("when the piece budget is too small")
    {
        AutoSplit::FitPlateParams params = plate(100., 100., 100.);
        params.max_pieces                = 8;

        AutoSplit::Error error = AutoSplit::Error::None;
        const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(box, params, &error);

        CHECK(plan.empty());
        CHECK(error == AutoSplit::Error::TooManyPieces);
    }

    SECTION("when the pieces would be too thin to print")
    {
        AutoSplit::FitPlateParams params = plate(1000., 1000., 0.5);
        params.min_piece_size            = 5.;

        AutoSplit::Error error = AutoSplit::Error::None;
        const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(box, params, &error);

        CHECK(plan.empty());
        CHECK(error == AutoSplit::Error::PiecesTooThin);
    }

    SECTION("when the printer has no usable volume")
    {
        AutoSplit::Error error = AutoSplit::Error::None;
        const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(box, plate(0., 0., 0.), &error);

        CHECK(plan.empty());
        CHECK(error == AutoSplit::Error::EmptyBuildVolume);
    }
}

TEST_CASE("An empty mesh needs no cutting", "[AutoSplit]")
{
    AutoSplit::Error error = AutoSplit::Error::CutFailed;
    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(indexed_triangle_set{}, plate(200., 200., 200.), &error);

    CHECK(plan.empty());
    CHECK(error == AutoSplit::Error::None);
}

TEST_CASE("Connectors are planned inside the cut face", "[AutoSplit]")
{
    const indexed_triangle_set box = centred_box(300., 100., 100.);

    AutoSplit::FitPlateParams params  = plate(200., 200., 200.);
    params.connectors.enabled         = true;
    params.connectors.radius          = 2.5;
    params.connectors.height          = 5.;
    params.connectors.spacing         = 30.;
    params.connectors.wall_margin     = 1.5;

    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(box, params);
    REQUIRE(plan.nodes.size() == 1);

    const AutoSplit::PlanNode &node = plan.nodes[0];
    CHECK(!node.connectors.empty());

    // The cut face is the 100 x 100 square at x = 0. Every connector has to sit on that
    // plane and keep its wall clear of the edge.
    const double clearance = params.connectors.radius + params.connectors.wall_margin;
    for (const AutoSplit::Connector &connector : node.connectors) {
        CHECK_THAT(connector.pos.x(), WithinAbs(0., 1e-6));
        CHECK(std::abs(connector.pos.y()) <= 50. - clearance + 1e-6);
        CHECK(std::abs(connector.pos.z()) <= 50. - clearance + 1e-6);
    }
}

TEST_CASE("A cut face with no room holds no connectors", "[AutoSplit]")
{
    // A 4 mm thick slab cannot seat a connector 2.5 mm in radius with a 1.5 mm margin.
    const indexed_triangle_set slab = centred_box(300., 100., 4.);

    AutoSplit::FitPlateParams params = plate(200., 200., 200.);
    params.connectors.enabled        = true;

    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(slab, params);

    REQUIRE(plan.nodes.size() == 1);
    CHECK(plan.nodes[0].connectors.empty());
}

TEST_CASE("Connectors are spread rather than piled up", "[AutoSplit]")
{
    const indexed_triangle_set box = centred_box(300., 200., 200.);

    AutoSplit::FitPlateParams params = plate(200., 400., 400.);
    params.connectors.enabled        = true;
    params.connectors.spacing        = 50.;
    params.connectors.max_per_plane  = 16;

    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(box, params);
    REQUIRE(plan.nodes.size() == 1);

    const std::vector<AutoSplit::Connector> &connectors = plan.nodes[0].connectors;
    REQUIRE(connectors.size() > 1);
    CHECK(connectors.size() <= size_t(params.connectors.max_per_plane));

    for (size_t i = 0; i < connectors.size(); ++i)
        for (size_t j = i + 1; j < connectors.size(); ++j)
            CHECK((connectors[i].pos - connectors[j].pos).norm() > 2. * params.connectors.radius);
}

TEST_CASE("Executing a plan produces one object per piece", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_box_object(model, 300., 100., 100.);

    AutoSplit::FitPlateParams params = plate(200., 200., 200.);
    const AutoSplit::Plan     plan   = AutoSplit::plan_fit_build_plate(AutoSplit::cut_space_mesh(*object, 0), params);
    REQUIRE(plan.piece_count() == 2);

    AutoSplit::ExecuteParams execute_params;
    execute_params.connectors.enabled = false;

    Model            pieces;
    AutoSplit::Error error = AutoSplit::Error::CutFailed;
    REQUIRE(AutoSplit::execute(plan, *object, 0, execute_params, pieces, &error));
    CHECK(error == AutoSplit::Error::None);

    REQUIRE(pieces.objects.size() == 2);
    for (const ModelObject *piece : pieces.objects) {
        CHECK(!piece->volumes.empty());
        CHECK(piece->instances.size() == object->instances.size());
    }
}

TEST_CASE("Each executed piece fits the plate it was planned for", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_box_object(model, 420., 300., 100.);

    AutoSplit::FitPlateParams params = plate(200., 200., 200.);
    const AutoSplit::Plan     plan   = AutoSplit::plan_fit_build_plate(AutoSplit::cut_space_mesh(*object, 0), params);
    REQUIRE(plan.piece_count() == 6); // 3 slabs in X, 2 in Y

    AutoSplit::ExecuteParams execute_params;
    execute_params.connectors.enabled = false;

    Model pieces;
    REQUIRE(AutoSplit::execute(plan, *object, 0, execute_params, pieces, nullptr));
    REQUIRE(pieces.objects.size() == 6);

    for (const ModelObject *piece : pieces.objects) {
        const BoundingBoxf3 box = piece->raw_mesh_bounding_box();
        check_fits(box, params.usable_size);
        // A cut that produced nothing would also "fit", so check the piece has substance.
        CHECK(box.size().z() > 1.);
    }
}

TEST_CASE("Executed pieces add back up to the original volume", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_box_object(model, 300., 100., 100.);

    AutoSplit::FitPlateParams params = plate(200., 200., 200.);
    const AutoSplit::Plan     plan   = AutoSplit::plan_fit_build_plate(AutoSplit::cut_space_mesh(*object, 0), params);

    AutoSplit::ExecuteParams execute_params;
    execute_params.connectors.enabled = false;

    Model pieces;
    REQUIRE(AutoSplit::execute(plan, *object, 0, execute_params, pieces, nullptr));

    double split_volume = 0.;
    for (const ModelObject *piece : pieces.objects)
        for (const ModelVolume *volume : piece->volumes)
            if (volume->is_model_part())
                split_volume += double(its_volume(volume->mesh().its));

    const double whole_volume = 300. * 100. * 100.;
    CHECK_THAT(split_volume, WithinAbs(whole_volume, whole_volume * 1e-3));
}

TEST_CASE("Executing an empty plan hands back the model unchanged", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_box_object(model, 50., 50., 50.);

    Model pieces;
    REQUIRE(AutoSplit::execute(AutoSplit::Plan{}, *object, 0, AutoSplit::ExecuteParams{}, pieces, nullptr));

    REQUIRE(pieces.objects.size() == 1);
    CHECK_THAT(pieces.objects[0]->raw_mesh_bounding_box().size().x(), WithinAbs(50., 1e-6));
}

TEST_CASE("Executing against a missing instance fails rather than cutting", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_box_object(model, 300., 100., 100.);

    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(AutoSplit::cut_space_mesh(*object, 0),
                                                                 plate(200., 200., 200.));

    Model            pieces;
    AutoSplit::Error error = AutoSplit::Error::None;
    CHECK(!AutoSplit::execute(plan, *object, 3, AutoSplit::ExecuteParams{}, pieces, &error));
    CHECK(error == AutoSplit::Error::NoSuchInstance);
    CHECK(pieces.objects.empty());
}

TEST_CASE("Connectors carve a negative volume out of the pieces", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_box_object(model, 300., 100., 100.);

    AutoSplit::FitPlateParams params = plate(200., 200., 200.);
    params.connectors.enabled        = true;

    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(AutoSplit::cut_space_mesh(*object, 0), params);
    REQUIRE(!plan.nodes[0].connectors.empty());

    AutoSplit::ExecuteParams execute_params;
    execute_params.connectors = params.connectors;

    Model pieces;
    REQUIRE(AutoSplit::execute(plan, *object, 0, execute_params, pieces, nullptr));
    REQUIRE(pieces.objects.size() == 2);

    // Cut turns the planned connectors into connector volumes on the pieces: a stud on one
    // half and the hole it seats in on the other.
    size_t connector_volumes = 0;
    for (const ModelObject *piece : pieces.objects)
        for (const ModelVolume *volume : piece->volumes)
            if (volume->is_cut_connector())
                ++connector_volumes;

    CHECK(connector_volumes >= plan.nodes[0].connectors.size());
}

TEST_CASE("An oversized real model is split into connected pieces that each fit", "[AutoSplit]")
{
    // frog_legs is a non-convex organic shape, so the pieces are nothing like their share
    // of the bounding box and the planner has to measure what is actually in each region.
    TriangleMesh mesh = load_model("frog_legs.obj");
    REQUIRE(!mesh.empty());
    // Blown up well past a 256 mm bed, the size at which this stops being printable whole.
    mesh.scale(6.f);

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "frog legs";
    object->add_volume(std::move(mesh));
    object->add_instance();

    const std::vector<Vec2d> bed{{0., 0.}, {256., 0.}, {256., 256.}, {0., 256.}};
    const BuildVolume        build_volume(bed, 256., {}, {});

    AutoSplit::FitPlateParams params;
    params.usable_size        = AutoSplit::usable_size(build_volume, 5.);
    params.connectors.enabled = true;

    const indexed_triangle_set its = AutoSplit::cut_space_mesh(*object, 0);
    REQUIRE(its.indices.size() > 0);

    AutoSplit::Error      error = AutoSplit::Error::CutFailed;
    const AutoSplit::Plan plan  = AutoSplit::plan_fit_build_plate(its, params, &error);
    REQUIRE(error == AutoSplit::Error::None);
    REQUIRE(!plan.empty());

    AutoSplit::ExecuteParams execute_params;
    execute_params.connectors = params.connectors;

    Model pieces;
    REQUIRE(AutoSplit::execute(plan, *object, 0, execute_params, pieces, &error));
    REQUIRE(error == AutoSplit::Error::None);
    REQUIRE(pieces.objects.size() == plan.piece_count());

    size_t connector_volumes = 0;
    for (const ModelObject *piece : pieces.objects) {
        check_fits(piece->raw_mesh_bounding_box(), params.usable_size);
        for (const ModelVolume *volume : piece->volumes)
            if (volume->is_cut_connector())
                ++connector_volumes;
    }

    // The pieces have to be joinable, not just small enough.
    CHECK(connector_volumes > 0);
}

TEST_CASE("Planning works on the cut-space mesh of a rotated instance", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_box_object(model, 300., 100., 100.);
    // Turned a quarter turn about Z, the long side now runs along Y.
    object->instances[0]->set_rotation(Vec3d(0., 0., 0.5 * PI));

    const indexed_triangle_set its  = AutoSplit::cut_space_mesh(*object, 0);
    const AutoSplit::Plan      plan = AutoSplit::plan_fit_build_plate(its, plate(200., 200., 200.));

    REQUIRE(plan.nodes.size() == 1);
    CHECK(axis_of(plan.nodes[0].plane.normal) == Y);
}

// Splitting by painted colour
// ---------------------------

TEST_CASE("A plan numbers its pieces so every branch names one", "[AutoSplit]")
{
    const indexed_triangle_set box = centred_box(640., 450., 90.);

    const AutoSplit::Plan plan = AutoSplit::plan_fit_build_plate(box, plate(200., 200., 250.));

    REQUIRE(!plan.empty());
    // A binary tree with n internal nodes has n + 1 leaves.
    CHECK(plan.pieces.size() == plan.nodes.size() + 1);

    std::vector<bool> named(plan.pieces.size(), false);
    for (const AutoSplit::PlanNode &node : plan.nodes)
        for (const int branch : {node.upper, node.lower})
            if (AutoSplit::branch_is_piece(branch)) {
                const int piece = AutoSplit::branch_to_piece(branch);
                REQUIRE(piece >= 0);
                REQUIRE(size_t(piece) < named.size());
                CHECK(!named[size_t(piece)]); // no piece is claimed twice
                named[size_t(piece)] = true;
            }
    for (const bool claimed : named)
        CHECK(claimed);
}

TEST_CASE("An unpainted model has no colours to separate", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_box_object(model, 100., 40., 40.);

    AutoSplit::Error      error = AutoSplit::Error::None;
    const AutoSplit::Plan plan  = AutoSplit::plan_by_painted_color(*object, 0, colour_params(), &error);

    CHECK(plan.empty());
    CHECK(error == AutoSplit::Error::NothingToSeparate);
}

TEST_CASE("A model painted in a single colour has no colours to separate", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_box_object(model, 100., 40., 40.);

    // Every facet the same colour is still one colour, however much paint there is.
    TriangleSelector selector(object->volumes.front()->mesh());
    for (size_t i = 0; i < object->volumes.front()->mesh().its.indices.size(); ++i)
        selector.set_facet(int(i), EnforcerBlockerType::Extruder2);
    REQUIRE(object->volumes.front()->mmu_segmentation_facets.set(selector));

    AutoSplit::Error      error = AutoSplit::Error::None;
    const AutoSplit::Plan plan  = AutoSplit::plan_by_painted_color(*object, 0, colour_params(), &error);

    CHECK(plan.empty());
    CHECK(error == AutoSplit::Error::NothingToSeparate);
}

TEST_CASE("A two-colour model is cut into one piece per colour", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_two_colour_box(model);

    AutoSplit::Error      error = AutoSplit::Error::None;
    const AutoSplit::Plan plan  = AutoSplit::plan_by_painted_color(*object, 0, colour_params(), &error);

    REQUIRE(error == AutoSplit::Error::None);
    REQUIRE(plan.nodes.size() == 1);
    CHECK(plan.piece_count() == 2);

    // The paint is on the low-X cap, so the cut has to run across X.
    CHECK(axis_of(plan.nodes[0].plane.normal) == X);

    // One piece prints in the painted filament, the other in the volume's own.
    CHECK(piece_extruders(plan) == std::vector<int>{1, 2});
}

TEST_CASE("A speck of a second colour is not worth cutting the model for", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_two_colour_box(model);

    // The painted cap is a twelfth of the surface; told to ignore anything under half, the
    // planner should treat it as a stray dab rather than a colour worth separating.
    AutoSplit::ColorSplitParams params = colour_params();
    params.ignore_colour_fraction      = 0.5;

    AutoSplit::Error      error = AutoSplit::Error::None;
    const AutoSplit::Plan plan  = AutoSplit::plan_by_painted_color(*object, 0, params, &error);

    CHECK(plan.empty());
    CHECK(error == AutoSplit::Error::NothingToSeparate);
}

TEST_CASE("A colour at both ends of a model takes a cut at each end", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_box_painted_at_both_ends(model);

    const AutoSplit::Plan plan = AutoSplit::plan_by_painted_color(*object, 0, colour_params(), nullptr);

    // No single plane separates the two painted ends from the bare middle, but two do.
    REQUIRE(plan.nodes.size() == 2);
    CHECK(plan.piece_count() == 3);
    CHECK(piece_extruders(plan) == std::vector<int>{1, 2, 2});
}

TEST_CASE("A cut that would leave too much paint on the wrong piece is not made", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_box_painted_at_both_ends(model);

    // The best plane here still strands one painted cap, a twelfth of the painted area, on
    // the wrong side. Told not to tolerate that much, the planner leaves the model whole
    // rather than cutting somewhere that does not separate the colours.
    AutoSplit::ColorSplitParams params = colour_params();
    params.hopeless_fraction           = 0.05;

    AutoSplit::Error      error = AutoSplit::Error::None;
    const AutoSplit::Plan plan  = AutoSplit::plan_by_painted_color(*object, 0, params, &error);

    CHECK(plan.empty());
    CHECK(error == AutoSplit::Error::None); // not a failure: there is simply nothing worth doing
}

TEST_CASE("A colour split stops once every piece is one colour", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_two_colour_box(model);

    // The cap separates cleanly, so one cut leaves two single-coloured pieces and the
    // planner has no reason to cut either of them again.
    const AutoSplit::Plan plan = AutoSplit::plan_by_painted_color(*object, 0, colour_params(), nullptr);

    REQUIRE(!plan.empty());
    CHECK(plan.nodes.size() == 1);
}

TEST_CASE("A colour split never makes more pieces than it is allowed", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_two_colour_box(model);

    AutoSplit::ColorSplitParams params = colour_params();
    params.max_pieces                  = 1;

    const AutoSplit::Plan plan = AutoSplit::plan_by_painted_color(*object, 0, params, nullptr);

    CHECK(plan.piece_count() <= 1);
}

TEST_CASE("Executing a colour split gives each piece its own filament", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_two_colour_box(model);

    const AutoSplit::Plan plan = AutoSplit::plan_by_painted_color(*object, 0, colour_params(), nullptr);
    REQUIRE(plan.piece_count() == 2);

    AutoSplit::ExecuteParams execute_params;
    execute_params.connectors.enabled = false;

    Model            pieces;
    AutoSplit::Error error = AutoSplit::Error::CutFailed;
    REQUIRE(AutoSplit::execute(plan, *object, 0, execute_params, pieces, &error));
    CHECK(error == AutoSplit::Error::None);
    REQUIRE(pieces.objects.size() == 2);

    // execute() returns the pieces in Plan::pieces order, so the two line up.
    for (size_t i = 0; i < pieces.objects.size(); ++i) {
        const ConfigOption *extruder = pieces.objects[i]->config.option("extruder");
        REQUIRE(extruder != nullptr);
        CHECK(extruder->getInt() == plan.pieces[i].extruder);
    }
}

TEST_CASE("A piece given its own filament no longer carries the paint it was split on", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_two_colour_box(model);

    const AutoSplit::Plan plan = AutoSplit::plan_by_painted_color(*object, 0, colour_params(), nullptr);
    REQUIRE(!plan.empty());

    AutoSplit::ExecuteParams execute_params;
    execute_params.connectors.enabled = false;

    Model pieces;
    REQUIRE(AutoSplit::execute(plan, *object, 0, execute_params, pieces, nullptr));

    // Each piece prints in one filament now, so leaving the paint on would reinstate the
    // colours the split was asked to separate.
    for (const ModelObject *piece : pieces.objects)
        for (const ModelVolume *volume : piece->volumes)
            CHECK(volume->mmu_segmentation_facets.empty());
}

TEST_CASE("Keeping the paint on a colour split is possible when asked for", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_two_colour_box(model);

    const AutoSplit::Plan plan = AutoSplit::plan_by_painted_color(*object, 0, colour_params(), nullptr);
    REQUIRE(!plan.empty());

    AutoSplit::ExecuteParams execute_params;
    execute_params.connectors.enabled             = false;
    execute_params.clear_paint_on_coloured_pieces = false;

    Model pieces;
    REQUIRE(AutoSplit::execute(plan, *object, 0, execute_params, pieces, nullptr));

    size_t painted = 0;
    for (const ModelObject *piece : pieces.objects)
        for (const ModelVolume *volume : piece->volumes)
            if (!volume->mmu_segmentation_facets.empty())
                ++painted;
    CHECK(painted > 0);
}

TEST_CASE("The painted surface reports area and filament per facet", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_two_colour_box(model, 100., 40., 40.);

    const std::vector<AutoSplit::PaintedFacet> facets = AutoSplit::painted_surface(*object, 0);
    REQUIRE(!facets.empty());

    double painted_area = 0.;
    double total_area   = 0.;
    for (const AutoSplit::PaintedFacet &facet : facets) {
        CHECK(facet.area > 0.);
        CHECK(facet.extruder >= 1);
        total_area += facet.area;
        if (facet.extruder == 2)
            painted_area += facet.area;
    }

    // The painted cap is one 40 x 40 face; the whole box is 2 * (100*40 + 40*40 + 100*40).
    CHECK_THAT(painted_area, WithinAbs(1600., 1.));
    CHECK_THAT(total_area, WithinAbs(2. * (100. * 40. + 40. * 40. + 100. * 40.), 1.));
}

TEST_CASE("A colour cut gets connectors like any other cut", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_two_colour_box(model);

    AutoSplit::ColorSplitParams params = colour_params();
    params.connectors.enabled          = true;

    const AutoSplit::Plan plan = AutoSplit::plan_by_painted_color(*object, 0, params, nullptr);
    REQUIRE(plan.nodes.size() == 1);
    CHECK(!plan.nodes[0].connectors.empty());
}

TEST_CASE("The default split directions are distinct unit vectors", "[AutoSplit]")
{
    const std::vector<Vec3d> directions = AutoSplit::default_split_directions();

    REQUIRE(directions.size() >= 3);
    for (const Vec3d &direction : directions)
        CHECK_THAT(direction.norm(), WithinAbs(1., 1e-9));

    // A direction and its opposite describe the same plane, so neither may appear twice.
    for (size_t i = 0; i < directions.size(); ++i)
        for (size_t j = i + 1; j < directions.size(); ++j)
            CHECK(std::abs(directions[i].dot(directions[j])) < 1. - 1e-9);
}

TEST_CASE("Splitting by colour against a missing instance fails rather than cutting", "[AutoSplit]")
{
    Model        model;
    ModelObject *object = add_two_colour_box(model);

    AutoSplit::Error      error = AutoSplit::Error::None;
    const AutoSplit::Plan plan  = AutoSplit::plan_by_painted_color(*object, 7, colour_params(), &error);

    CHECK(plan.empty());
    CHECK(error == AutoSplit::Error::NoSuchInstance);
}
