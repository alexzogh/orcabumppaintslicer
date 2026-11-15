#include "TextureOutlineWrap.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

#include <admesh/stl.h>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include "Point.hpp"
#include "Polygon.hpp"
#include "TriangleMeshSlicer.hpp"
#include "libslic3r.h"

namespace Slic3r {

namespace {

constexpr float TWO_PI = 2.f * float(M_PI);

// The seam's angle around the axis: the -Y side, which is where TextureProjectionMethod::Cylindrical
// already puts its own u discontinuity (project_cylindrical() measures its angle from +Y, growing
// anticlockwise, so it wraps at -Y). Keeping the two in the same place means switching a layer
// between the two mappings leaves the pattern where it was instead of spinning it half a turn.
constexpr float SEAM_ANGLE = -0.5f * float(M_PI);

// More than any part needs at OUTLINE_WRAP_CUT_STEP_MM. A taller one simply gets coarser cuts rather
// than an unbounded number of full-mesh slices.
constexpr size_t MAX_CUTS = 2048;

// Into [0, 2 * pi).
float turn(float a)
{
    const float r = std::fmod(a, TWO_PI);
    return r < 0.f ? r + TWO_PI : r;
}

// Into (-pi, pi]. The step between two neighbouring outline corners is always the short way round:
// a single straight piece of the outline cannot subtend half a turn about a point it goes around.
float half_turn(float a)
{
    const float r = turn(a);
    return r > float(M_PI) ? r - TWO_PI : r;
}

// Where the ray from `c` at angle `a` meets the segment a_pt - b_pt, as the fraction along it.
// Zero when the two are parallel, which is what a fold's zero-width step between two pieces is.
float ray_fraction(const Vec2f &a_pt, const Vec2f &b_pt, float a, const Vec2f &c)
{
    const Vec2f r = Vec2f(std::cos(a), std::sin(a));
    const Vec2f e = b_pt - a_pt;
    const float across = e.x() * r.y() - e.y() * r.x();
    if (std::abs(across) < 1e-12f)
        return 0.f;
    const Vec2f rel = a_pt - c;
    return std::clamp(-(rel.x() * r.y() - rel.y() * r.x()) / across, 0.f, 1.f);
}

// Integral of dz / L over the first `t` (0..1) of a step `span` mm tall, where L runs linearly from
// `from` to `to` across it. Exact, so v grows by L* / L(z) per millimetre of height wherever
// outline_wrap_place() interpolates between two cross-sections of different length.
float rise_across(float from, float to, float span, float t)
{
    if (from <= 0.f)
        return 0.f;
    const float x = (to - from) * t / from;
    return span * t * ((x == 0.f) ? 1.f : std::log1p(x) / x) / from;
}

// The distance along `cut`'s outline from the seam to its point at angle `phi` past the seam.
float along(const OutlineWrapCut &cut, float phi, const Vec2f &center)
{
    const float a  = cut.theta.front() + phi;
    size_t      lo = 0;
    size_t      hi = cut.theta.size() - 2;
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (cut.theta[mid] <= a)
            lo = mid;
        else
            hi = mid - 1;
    }
    const float f = ray_fraction(cut.p[lo], cut.p[lo + 1], a, center);
    return cut.s[lo] + f * (cut.s[lo + 1] - cut.s[lo]);
}

// One cross-section's table, from its outline (counterclockwise, no repeated corners) as the axis at
// `center` sees it. Left unusable when the outline does not go around the axis at all, or when a
// single fold hides more than OUTLINE_WRAP_FOLD_MM of it from the axis.
OutlineWrapCut cut_table(float z, const std::vector<Vec2f> &pts, const Vec2f &center)
{
    OutlineWrapCut cut;
    cut.z = z;

    const size_t n = pts.size();
    if (n < 3)
        return cut;

    std::vector<float> angle(n), step(n);
    for (size_t i = 0; i < n; ++i) {
        const Vec2f rel = pts[i] - center;
        angle[i]        = std::atan2(rel.y(), rel.x());
    }
    float total = 0.f;
    for (size_t i = 0; i < n; ++i) {
        step[i] = half_turn(angle[(i + 1) % n] - angle[i]);
        total += step[i];
    }
    // A loop that goes once round the axis turns through 2 * pi; one that does not (a C-shaped
    // cross-section whose opening faces the axis) turns through about zero. Half a turn separates
    // the two with room to spare.
    if (total < float(M_PI))
        return cut;

    // Three laps with the angle unwrapped and the length running on. The first only establishes the
    // largest angle reached so far, so a fold straddling corner 0 is counted whole in the second; the
    // second and third hold the seam's one-turn window wherever in the loop the seam falls.
    const size_t       laps = 3 * n;
    std::vector<float> A(laps + 1, 0.f), S(laps + 1, 0.f);
    A[0] = angle[0];
    for (size_t i = 0; i < laps; ++i) {
        const size_t a = i % n, b = (i + 1) % n;
        A[i + 1] = A[i] + step[a];
        S[i + 1] = S[i] + (pts[b] - pts[a]).norm();
    }

    // What the axis actually sees: each piece runs from where the outline first passes the largest
    // angle reached so far to the end of that segment. Everything between the end of one piece and
    // the start of the next is hidden behind a fold.
    struct Piece
    {
        float a0, s0, a1, s1;
        Vec2f p0, p1;
    };
    std::vector<Piece> pieces;
    float              most   = A[0];
    float              seen_s = 0.f;
    float              hidden = 0.f;
    for (size_t i = 0; i < laps; ++i) {
        if (A[i + 1] <= most)
            continue;
        const size_t a = i % n, b = (i + 1) % n;
        const float  f       = (A[i] >= most) ? 0.f : ray_fraction(pts[a], pts[b], most, center);
        const float  start_s = S[i] + f * (S[i + 1] - S[i]);
        if (i >= n) {
            hidden = std::max(hidden, start_s - seen_s);
            pieces.push_back({ std::max(A[i], most), start_s, A[i + 1], S[i + 1],
                               pts[a] + f * (pts[b] - pts[a]), pts[b] });
        }
        seen_s = S[i + 1];
        most   = A[i + 1];
    }
    if (pieces.empty() || hidden > OUTLINE_WRAP_FOLD_MM)
        return cut;

    // The window from the seam once around, each end piece cut at the seam.
    const float from = pieces.front().a0 + turn(SEAM_ANGLE - pieces.front().a0);
    const float to   = from + TWO_PI;
    // Where the ray at angle `a` meets this piece, as (distance along the outline, point).
    const auto at = [&center](const Piece &piece, float a) {
        const float f = ray_fraction(piece.p0, piece.p1, a, center);
        return std::make_pair(piece.s0 + f * (piece.s1 - piece.s0), Vec2f(piece.p0 + f * (piece.p1 - piece.p0)));
    };
    for (const Piece &piece : pieces) {
        if (piece.a1 <= from || piece.a0 >= to)
            continue;
        const auto head = (piece.a0 < from) ? at(piece, from) : std::make_pair(piece.s0, piece.p0);
        const auto tail = (piece.a1 > to) ? at(piece, to) : std::make_pair(piece.s1, piece.p1);
        cut.theta.push_back(std::max(piece.a0, from));
        cut.s.push_back(head.first);
        cut.p.push_back(head.second);
        cut.theta.push_back(std::min(piece.a1, to));
        cut.s.push_back(tail.first);
        cut.p.push_back(tail.second);
    }
    if (cut.s.size() < 2) {
        cut.theta.clear();
        cut.s.clear();
        cut.p.clear();
        return cut;
    }
    const float zero = cut.s.front();
    for (float &s : cut.s)
        s -= zero;
    return cut;
}

} // namespace

OutlineWrap build_outline_wrap(const indexed_triangle_set &mesh)
{
    OutlineWrap wrap;
    if (mesh.indices.empty() || mesh.vertices.empty())
        return wrap;

    Vec3f lo = mesh.vertices.front(), hi = lo;
    for (const Vec3f &v : mesh.vertices) {
        lo = lo.cwiseMin(v);
        hi = hi.cwiseMax(v);
    }
    wrap.axis_xy = Vec2f(0.5f * (lo.x() + hi.x()), 0.5f * (lo.y() + hi.y()));
    wrap.min_z   = lo.z();
    wrap.max_z   = hi.z();

    const float range = wrap.max_z - wrap.min_z;
    if (!(range > 0.f))
        return wrap;
    const size_t count = std::clamp<size_t>(size_t(std::ceil(range / OUTLINE_WRAP_CUT_STEP_MM)), 1, MAX_CUTS);
    const float  step  = range / float(count);

    // Each cut sits in the middle of its step, so a flat top or bottom face never lands exactly on a
    // slicing plane - where the cross-section would be the cap's own outline rather than the sides'.
    std::vector<float> zs(count);
    for (size_t k = 0; k < count; ++k)
        zs[k] = wrap.min_z + (float(k) + 0.5f) * step;

    MeshSlicingParams params;
    // One counterclockwise loop per cut, the largest the cross-section has: holes and loose islands
    // are not part of the part's silhouette, and the outline has to be a single closed run.
    params.mode = MeshSlicingParams::SlicingMode::PositiveLargestContour;
    const std::vector<Polygons> layers = slice_mesh(mesh, zs, params);

    // Every cut keeps its height whether or not it has an outline, because outline_wrap_place() finds
    // a point's cut by stepping evenly between the first and the last.
    wrap.cuts.resize(count);
    for (size_t k = 0; k < count; ++k)
        wrap.cuts[k].z = zs[k];

    tbb::parallel_for(tbb::blocked_range<size_t>(0, std::min(count, layers.size())),
                      [&](const tbb::blocked_range<size_t> &range) {
        std::vector<Vec2f> pts;
        for (size_t k = range.begin(); k < range.end(); ++k) {
            if (layers[k].empty())
                continue;
            // Repeated corners (a cut passing exactly through a mesh vertex) would make zero-length
            // steps with a meaningless angle, so they go first.
            const Points &loop = layers[k].front().points;
            pts.clear();
            pts.reserve(loop.size());
            for (size_t i = 0; i < loop.size(); ++i) {
                const Vec2f p(unscale<float>(loop[i].x()), unscale<float>(loop[i].y()));
                if (pts.empty() || (p - pts.back()).norm() > 1e-6f)
                    pts.push_back(p);
            }
            if (pts.size() > 2 && (pts.front() - pts.back()).norm() <= 1e-6f)
                pts.pop_back();
            wrap.cuts[k] = cut_table(zs[k], pts, wrap.axis_xy);
        }
    });

    double length_sum = 0.;
    size_t usable     = 0;
    for (const OutlineWrapCut &cut : wrap.cuts)
        if (cut.usable()) {
            length_sum += double(cut.length());
            ++usable;
        }
    if (usable == 0) {
        wrap.cuts.clear();
        return wrap;
    }
    wrap.mean_length = float(length_sum / double(usable));

    // The height each cut has risen from the bottom of the part. A cut the wrap could not follow still
    // has to pass the integral on, or v would jump across the band that falls back to Cylindrical, so
    // it borrows the length of the nearest cut that is usable.
    std::vector<float> effective(count, 0.f);
    for (size_t k = 0; k < count; ++k)
        if (wrap.cuts[k].usable())
            effective[k] = wrap.cuts[k].length();
    float last = 0.f;
    for (size_t k = 0; k < count; ++k)
        if (effective[k] > 0.f)
            last = effective[k];
        else
            effective[k] = last;
    last = 0.f;
    for (size_t k = count; k-- > 0;)
        if (effective[k] > 0.f)
            last = effective[k];
        else
            effective[k] = last;
    wrap.cuts[0].rise = (zs[0] - wrap.min_z) / effective[0];
    for (size_t k = 1; k < count; ++k)
        wrap.cuts[k].rise = wrap.cuts[k - 1].rise +
                            rise_across(effective[k - 1], effective[k], zs[k] - zs[k - 1], 1.f);
    return wrap;
}

bool outline_wrap_place(const OutlineWrap &wrap, const Vec3f &position, float &share, float &rise)
{
    if (!wrap.usable())
        return false;

    const Vec2f rel = position.head<2>() - wrap.axis_xy;
    const float phi = turn(std::atan2(rel.y(), rel.x()) - SEAM_ANGLE);

    const size_t n = wrap.cuts.size();
    size_t       k = 0;
    float        t = 0.f;
    if (n > 1 && position.z() > wrap.cuts.front().z) {
        if (position.z() >= wrap.cuts.back().z) {
            k = n - 1;
        } else {
            const float step = (wrap.cuts.back().z - wrap.cuts.front().z) / float(n - 1);
            const float f    = (position.z() - wrap.cuts.front().z) / step;
            k                = std::min(size_t(f), n - 2);
            t                = std::clamp(f - float(k), 0.f, 1.f);
        }
    }

    const OutlineWrapCut &a = wrap.cuts[k];
    if (!a.usable())
        return false;
    const float l0 = a.length();
    const float u0 = along(a, phi, wrap.axis_xy);
    if (t <= 0.f) {
        share = u0 / l0;
        rise  = a.rise + (position.z() - a.z) / l0;
        return true;
    }
    const OutlineWrapCut &b = wrap.cuts[k + 1];
    if (!b.usable())
        return false;
    const float l1     = b.length();
    const float u1     = along(b, phi, wrap.axis_xy);
    const float length = l0 + t * (l1 - l0);
    share = (length > 0.f) ? (u0 + t * (u1 - u0)) / length : 0.f;
    rise  = a.rise + rise_across(l0, l1, b.z - a.z, t);
    return true;
}

} // namespace Slic3r
