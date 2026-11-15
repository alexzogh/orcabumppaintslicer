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

// A node of the binary cut tree. `upper` and `lower` index back into Plan::nodes and say
// which node splits that side further; -1 means that side is a finished piece.
struct PlanNode
{
    CutPlane               plane;
    std::vector<Connector> connectors;
    int                    upper{-1};
    int                    lower{-1};
};

// A cut plan. nodes[0] is the root. An empty plan means the model needs no cutting.
struct Plan
{
    std::vector<PlanNode> nodes;

    bool   empty() const { return nodes.empty(); }
    // A binary tree with n internal nodes has n + 1 leaves, and every leaf is one piece.
    size_t piece_count() const { return nodes.size() + 1; }
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

// Executing a plan
// ----------------

struct ExecuteParams
{
    ConnectorParams connectors;
    // Name each piece "<object name> - <n>".
    bool number_pieces{true};
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
