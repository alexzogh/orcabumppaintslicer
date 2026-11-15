#ifndef slic3r_AutoSplit_hpp_
#define slic3r_AutoSplit_hpp_

#include <cstddef>
#include <string>
#include <vector>

#include "Model.hpp"
#include "Point.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

class BuildVolume;
class Model;
class ModelObject;

// Automatic splitting: work out where to cut a model so that the pieces satisfy some
// constraint, then drive the existing Cut machinery to make those cuts.
//
// The planning half is pure geometry and knows nothing about Model or the GUI, so the
// choice of planes can be tested on a bare indexed_triangle_set. The execution half turns
// a plan into model objects using Cut, and is shared by every strategy.
namespace AutoSplit {

// Cut space
// ---------
// Every position here is in the frame Cut applies its matrix in: the instance's rotation,
// scaling and mirroring are baked into the mesh, and the instance's offset is removed, so
// the axes are the world axes and the origin is the instance origin. cut_space_matrix()
// builds the transform that takes an object's volumes into it.

// The transform taking object `object`'s volume `volume` into the cut space of instance
// `instance_idx`. Matches what Cut::perform_with_plane() applies to the meshes it cuts.
Transform3d cut_space_matrix(const ModelObject &object, int instance_idx, const ModelVolume &volume);

// The model parts of `object` merged into a single mesh in the cut space of `instance_idx`.
// Modifiers, negative volumes and support blockers are left out: they are not what has to
// fit on the plate, and Cut carries them across on its own.
indexed_triangle_set cut_space_mesh(const ModelObject &object, int instance_idx);

// Why a split could not be planned or carried out. libslic3r has no translation lookup,
// so the reason travels as a code and whoever shows it picks the wording.
enum class Error
{
    None = 0,
    // The printer's usable volume came out empty.
    EmptyBuildVolume,
    // The instance index does not name an instance of the object.
    NoSuchInstance,
    // Fitting the model would need pieces thinner than FitPlateParams::min_piece_size.
    PiecesTooThin,
    // Fitting the model would need more pieces than FitPlateParams::max_pieces.
    TooManyPieces,
    // Splitting by colour was asked for, but the model is painted in one colour or none.
    NothingToSeparate,
    // A cut did not produce the two halves it should have.
    CutFailed,
    // The plan is inconsistent, which is a bug rather than something the user can fix.
    MalformedPlan,
};

// A short, untranslated identifier for `error`, for logs and tests.
const char *error_name(Error error);

// A planar cut. The +normal side is the "upper" side in Cut's sense.
struct CutPlane
{
    Vec3d origin{Vec3d::Zero()};
    Vec3d normal{Vec3d::UnitZ()};
};

// One connector on a cut plane. `pos` is the centre of the connector *on the plane*; the
// half-height shift Cut expects for plugs and snaps is applied when the plan is executed.
struct Connector
{
    Vec3d  pos{Vec3d::Zero()};
    double radius{2.5};
    double height{5.};
};

// A finished piece of the model: one leaf of the cut tree.
struct Piece
{
    // The filament this piece should print in, 1-based, or 0 to leave it as it is. Set by
    // a strategy that knows what a piece is for, such as splitting by painted colour.
    int extruder{0};
    // The direction of the piece that should end up pointing up the printer's Z, in cut
    // space. +Z leaves the piece standing as it was. Set by a strategy that gains from
    // turning a piece over, such as splitting to reduce supports.
    Vec3d build_direction{Vec3d::UnitZ()};
};

// Which node of the tree splits one side of a cut, or which piece that side already is.
// Non-negative is an index into Plan::nodes; negative encodes an index into Plan::pieces.
inline bool branch_is_piece(int branch) { return branch < 0; }
inline int  branch_to_piece(int branch) { return -branch - 1; }
inline int  piece_to_branch(int piece) { return -piece - 1; }

// A node of the binary cut tree. `upper` and `lower` are branches, as above.
struct PlanNode
{
    CutPlane               plane;
    std::vector<Connector> connectors;
    int                    upper{piece_to_branch(0)};
    int                    lower{piece_to_branch(0)};
};

// A cut plan. nodes[0] is the root. An empty plan means the model needs no cutting.
struct Plan
{
    std::vector<PlanNode> nodes;
    std::vector<Piece>    pieces;

    bool empty() const { return nodes.empty(); }
    // A binary tree with n internal nodes has n + 1 leaves, and every leaf is one piece.
    size_t piece_count() const { return nodes.empty() ? 1 : pieces.size(); }
};

struct ConnectorParams
{
    bool   enabled{true};
    double radius{2.5};
    double height{5.};
    // Target centre-to-centre distance between connectors on one cut face.
    double spacing{30.};
    // Smallest gap to leave between a connector's wall and the piece's wall.
    double wall_margin{1.5};
    // Most connectors to put on one cut face, so a large face does not produce hundreds.
    int max_per_plane{16};

    CutConnectorAttributes attributes{CutConnectorType::Plug, CutConnectorStyle::Prism, CutConnectorShape::Circle};
};

struct FitPlateParams
{
    // The box every piece has to fit inside, in mm. usable_size() derives it from a bed.
    Vec3d usable_size{Vec3d::Zero()};
    // Give up rather than produce more pieces than this.
    int max_pieces{64};
    // Give up rather than produce a slab thinner than this, in mm.
    double min_piece_size{5.};

    ConnectorParams connectors;
};

// The box a piece may occupy on `build_volume`, shrunk by `clearance` on every side in X
// and Y so the pieces still arrange with a gap between them. Height is not shrunk.
Vec3d usable_size(const BuildVolume &build_volume, double clearance);

// Choose the planes that cut `its` into pieces that each fit inside params.usable_size.
//
// `its` must already be in cut space. Returns an empty plan when the mesh already fits.
// On failure returns an empty plan and, if `error` is not null, sets it to a reason; an
// empty plan with Error::None means "nothing to do".
Plan plan_fit_build_plate(const indexed_triangle_set &its, const FitPlateParams &params, Error *error = nullptr);

// Splitting by painted colour
// ---------------------------
//
// Multi-material paint is a property of the *surface*: mmu_segmentation_facets says which
// filament each facet prints in, and says nothing about the material behind it. A piece,
// on the other hand, is a volume. Something has to decide which filament the inside of the
// model belongs to, and the rule here is:
//
//   A plane decides the volume. Each piece takes all the material on its side of the
//   separating planes, and its filament is the one most of the paint it carries asks for.
//   The planes are chosen to leave as little painted area as possible on the wrong piece.
//
// The alternative — giving each point of the interior the colour of the nearest painted
// facet, and splitting along the resulting boundary — fits the paint exactly and produces
// parts nobody can use. Those boundaries are curved and re-entrant, so the parts interlock
// and cannot be pulled apart in any direction, cannot be joined by the cut connectors that
// already exist, and are usually worse to print than the model was whole. Separating the
// filaments is only worth anything if the pieces go back together, so the rule trades
// exactness at a colour boundary for parts that assemble, and reports how much paint ended
// up on the wrong piece so the choice can be judged.
//
// Where the paint cannot be separated by planes at all — a logo on a flat face, say — the
// planner says so instead of returning a split that does not mean anything.

// One facet of the painted surface. Area-weighted, because the triangle selector subdivides
// a partly painted facet and the pieces are nothing like the same size.
struct PaintedFacet
{
    Vec3d  centre{Vec3d::Zero()};
    double area{0.};
    // 1-based filament, as EnforcerBlockerType::ExtruderN encodes it. Unpainted facets take
    // the volume's own filament.
    int extruder{1};
};

// The painted surface of `object`'s instance `instance_idx`, in cut space.
std::vector<PaintedFacet> painted_surface(const ModelObject &object, int instance_idx);

struct ColorSplitParams
{
    // Directions to try for each cut. Need not be axis-aligned: the cut machinery takes any
    // plane. Empty means the three axes.
    std::vector<Vec3d> directions;
    // Positions tried along each direction.
    int position_samples{256};
    // Ignore a colour covering less than this fraction of a piece's painted area, rather
    // than cutting the model up over a few stray facets. A piece left with one colour by
    // this rule is finished.
    double ignore_colour_fraction{0.005};
    // Give up on a piece when the best plane still leaves more than this fraction of its
    // painted area on the wrong side: the colours there are not separable by a plane, and
    // cutting anyway would make a worse model rather than a more colourful one.
    double hopeless_fraction{0.45};

    int max_pieces{32};

    ConnectorParams connectors;
};

// The standard set of directions to try: the three axes, the six face diagonals and the
// four body diagonals of a cube. Colour boundaries rarely line up with the axes.
std::vector<Vec3d> default_split_directions();

// Choose the planes that separate the painted colours of `object` into pieces that each
// print in one filament, and record each piece's filament in Plan::pieces.
//
// Returns an empty plan with Error::NothingToSeparate when the model carries fewer than two
// colours, and an empty plan with Error::None when the colours are already separate.
Plan plan_by_painted_color(const ModelObject   &object,
                           int                  instance_idx,
                           const ColorSplitParams &params,
                           Error               *error = nullptr);

// Splitting to reduce supports
// ----------------------------
//
// A part prints without support when every downward-facing surface is steep enough that
// the layer below reaches it: a shape with a flat base that rises without overhangs, which
// the literature calls pyramidal. Cutting a model into pyramidal parts is the exact version
// of this problem, and it is NP-hard; the published approximate method (Hu, Li, Zhang and
// Cohen-Or, "Approximate pyramidal shape decomposition", SIGGRAPH Asia 2014) recasts it as
// an exact-cover problem over clustered building blocks and solves that globally.
//
// This is not that, and does not claim to be. It is a greedy planar heuristic: try planes,
// keep the one whose two pieces — each free to be stood on whichever face suits it — leave
// the least overhanging area, and stop when no plane helps enough. It reduces overhang area
// and reports by how much. It does not promise support-free parts, the fewest parts, or the
// best cut.
//
// Cutting only helps because a piece can then be turned over: a cut makes a new flat face
// to stand the piece on, and the overhang that needed support is often above it. So each
// piece carries the direction it should be printed in, and the planner scores a cut by what
// the pieces cost once they are turned.

struct SupportSplitParams
{
    // Plane directions to try. Empty means default_split_directions().
    std::vector<Vec3d> directions;
    // Positions tried along each direction.
    int position_samples{64};
    // Surfaces whose slope is below this need support, matching the print setting of the
    // same name. Degrees from horizontal: 0 is a ceiling, 90 is a wall.
    double threshold_angle{30.};
    // Make a cut only when it removes at least this fraction of the overhanging area. A
    // cut that barely helps is not worth gluing afterwards.
    double min_improvement{0.15};
    // Never cut a piece down below this, in mm, along the direction of the cut.
    double min_piece_size{10.};

    int max_pieces{8};

    ConnectorParams connectors;
};

// What the split achieved, in square mm of overhanging surface.
struct SupportSplitReport
{
    // Printing the model as it stands, upright.
    double overhang_before{0.};
    // Summed over the pieces, each printed in the direction the plan gives it.
    double overhang_after{0.};
};

// The area of `its` that would need support if it were printed with `build_direction`
// pointing up. `threshold_angle` is in degrees, as the print setting is.
double overhanging_area(const indexed_triangle_set &its, const Vec3d &build_direction, double threshold_angle);

// Choose planes that leave the pieces with less overhanging surface than the whole model
// has, and record in Plan::pieces which way up each piece should print.
//
// `its` must already be in cut space. Returns an empty plan when no cut helps enough, which
// is not a failure: the model is better printed whole.
Plan plan_to_reduce_supports(const indexed_triangle_set &its,
                             const SupportSplitParams   &params,
                             SupportSplitReport         *report = nullptr,
                             Error                      *error  = nullptr);

// Executing a plan
// ----------------

struct ExecuteParams
{
    ConnectorParams connectors;
    // Name each piece "<object name> - <n>".
    bool number_pieces{true};
    // Apply Piece::extruder to the pieces that carry one.
    bool apply_piece_extruder{true};
    // Stand each piece the way the plan asks: turn it so its Piece::build_direction points
    // up, and set it down on the bed. Turn this off to keep every piece where it was
    // inside the original object, which is what lines the pieces up for assembly.
    bool apply_piece_build_direction{true};
    // Drop the multi-material paint from a piece that was given a filament of its own, so
    // it prints in that one filament instead of in the colours it was painted.
    bool clear_paint_on_coloured_pieces{true};
};

// Apply `plan` to instance `instance_idx` of `object`, appending the resulting pieces to
// `out_model`, which owns them. `object` is not modified.
//
// Returns false and sets `error` if a cut fails. The pieces keep the position and
// orientation they had inside the original object, so the caller is free to arrange them.
bool execute(const Plan          &plan,
             const ModelObject   &object,
             int                  instance_idx,
             const ExecuteParams &params,
             Model               &out_model,
             Error               *error = nullptr);

} // namespace AutoSplit
} // namespace Slic3r

#endif // slic3r_AutoSplit_hpp_
