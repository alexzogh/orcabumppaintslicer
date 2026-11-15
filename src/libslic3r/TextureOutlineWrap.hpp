#ifndef slic3r_TextureOutlineWrap_hpp_
#define slic3r_TextureOutlineWrap_hpp_

#include <vector>

#include <admesh/stl.h>

#include "Point.hpp"

namespace Slic3r {

// Tables that map a point of a mesh onto the part's own silhouette outline around the world Z axis,
// so a texture can run *along* that outline instead of by angle around it.
//
// Why this exists: TextureProjectionMethod::Cylindrical measures u as an angle times the point's own
// radius, so on anything that is not a circular cylinder the cell size of a regular pattern varies
// with that radius - stretched across a box's flat faces, bunched at its corners. Measuring u as the
// distance travelled along the cross-section's outline instead keeps the cell size constant wherever
// the outline is, which is what TextureProjectionMethod::OutlineWrap does with these tables.
//
// The outline is taken from the whole volume mesh, not from the painted patch: it is a property of
// the part's shape, so painting more of a box must not move the pattern already on it.

// Spacing of the cross-sections along Z. Half a millimetre is fine enough that a chamfer or a draft
// angle is followed smoothly and coarse enough that a 200 mm part costs a few hundred slices.
static constexpr float OUTLINE_WRAP_CUT_STEP_MM = 0.5f;

// How much outline a single fold may hide from the axis before the cross-section is given up on.
// A point finds its place by the angle it is seen at from the axis, so the axis has to see the whole
// outline. Where it does not - a step, or a recess whose wall faces away from the axis - the hidden
// run is skipped and the distance jumps across it. Up to this much is a small enough jump to be
// worth the wrap still working; beyond it the pattern would visibly tear, so the cross-section is
// marked unusable and that band of the part falls back to Cylindrical.
static constexpr float OUTLINE_WRAP_FOLD_MM = 2.f;

// One cross-section of the part, as the wrap axis sees it.
//
// `theta` is the angle around the axis, unwrapped so it ascends over exactly one turn starting at the
// seam; `s` is the distance travelled along the outline from the seam; `p` is the outline point. Entry
// i and i + 1 bracket one straight piece of the outline, so a point at angle theta is located by
// finding its piece and cutting it with the ray from the axis. Where a fold hides part of the outline
// `s` jumps between one piece and the next, which is exactly the hidden length.
struct OutlineWrapCut
{
    float              z = 0.f;
    std::vector<float> theta;
    std::vector<float> s;
    std::vector<Vec2f> p;
    // Integral of dz / L(z) from the bottom of the part up to `z`, dimensionless. v is this times the
    // same length u is measured in, so one repeat of the texture is as tall as it is wide even where
    // the outline grows or shrinks along Z (a pot's draft).
    float              rise = 0.f;

    // The outline's full length. Zero for a cross-section the wrap could not follow - see `usable`.
    float length() const { return s.empty() ? 0.f : s.back(); }
    bool  usable() const { return s.size() >= 2 && s.back() > 0.f; }
};

struct OutlineWrap
{
    // The wrap axis: world +Z through this point of the mesh's bounding box in XY.
    Vec2f axis_xy     = Vec2f::Zero();
    float min_z       = 0.f;
    float max_z       = 0.f;
    // Mean outline length over the usable cross-sections. The number of texture repeats that fit a
    // loop is chosen from this one length, so every height gets the same whole number of them.
    float mean_length = 0.f;
    // Evenly spaced in z, ascending, the first at min_z + step / 2. A cross-section the wrap could
    // not follow is kept in place but left unusable, so the band of the part around it can fall back
    // to Cylindrical while the rest still wraps.
    std::vector<OutlineWrapCut> cuts;

    bool usable() const { return mean_length > 0.f && !cuts.empty(); }
};

// Slice `mesh` across Z and build the tables above. `mesh` is taken in the frame the texture is
// measured in (world millimetres for a bake). Returns a table with usable() false when no
// cross-section of the part could be followed at all.
OutlineWrap build_outline_wrap(const indexed_triangle_set &mesh);

// Where `position` sits on the wrap: `share` is its distance along its own cross-section's outline
// from the seam as a fraction of that outline's length, in [0, 1), and `rise` is OutlineWrapCut::rise
// carried to this exact height. False when the wrap has nothing to say about this point - no usable
// cross-section brackets it - and the caller should project it some other way.
bool outline_wrap_place(const OutlineWrap &wrap, const Vec3f &position, float &share, float &rise);

} // namespace Slic3r

#endif // slic3r_TextureOutlineWrap_hpp_
