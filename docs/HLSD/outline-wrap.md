# Outline Wrap Texture Projection — High Level Design

## Purpose and scope

Outline wrap is a `TextureProjectionMethod` for texture displacement. It maps a
layer's texture onto the painted patch by distance travelled along the part's
own silhouette outline around the world Z axis.

It exists because `Cylindrical`, the mapping it sits next to, measures u as an
angle about an axis times the point's own radius from it. On a circular cylinder
that is a distance; on anything else it is not. A box's flat face covers few
degrees per millimetre and its corner covers many, so a regular pattern mapped
cylindrically is stretched across the faces and bunched at the corners, and a
brick or knurl pattern visibly changes size as it goes round. Measuring u along
the outline instead keeps the cell size the same everywhere the outline is, and
carries a pattern unbroken around a corner.

The scope is the mapping only. The texture, the paint mask, the displacement
itself, tiling, blending and every other layer control are unchanged, and no
other mapping's behaviour is affected: a layer that is not an outline-wrap layer
never builds or consults an outline.

## How a point is placed

`src/libslic3r/TextureOutlineWrap.{hpp,cpp}` owns the geometry.
`build_outline_wrap()` slices the volume mesh across Z every
`OUTLINE_WRAP_CUT_STEP_MM`, each cut in the middle of its step so a flat cap
never lands on a slicing plane. Each cut keeps the largest counterclockwise
contour its cross-section has (`MeshSlicingParams::SlicingMode::PositiveLargestContour`),
since holes and loose islands are not part of the part's silhouette and the
outline has to be one closed run.

The wrap axis is world +Z through the centre of the mesh's bounding box in XY.
Unlike `Cylindrical`, which picks an axis from the patch's average normal, the
axis here is fixed: "the outline of the part" only means anything about the axis
the part is printed around. For the same reason the outline is taken from the
whole volume mesh rather than from the painted patch — a pattern meant to run
round a part must not move when more of that part is painted.

A point finds its place on a cut by the angle it is seen at from the axis. That
makes a cut usable only when the axis can see the whole of its outline:

- A cut whose outline does not go around the axis at all — a C-shaped
  cross-section whose opening faces the axis — is marked unusable. It is
  detected by summing the signed angle steps between neighbouring outline
  corners: a loop that encircles the axis turns through a full turn, one that
  does not turns through about zero.
- A cut that folds back on itself — a step, or a recess whose wall faces away
  from the axis — hides a run of its outline from the axis. The walk over three
  unwrapped laps of the loop keeps only the pieces the axis actually sees, and
  the distance along the outline jumps across what is hidden. Up to
  `OUTLINE_WRAP_FOLD_MM` of hidden outline is accepted as a jump too small to
  see; beyond that the cut is marked unusable.

`outline_wrap_place()` returns the point's `share` — its distance along its own
cut's outline from the seam, over that outline's whole length — and its `rise`,
the integral of dz / L(z) from the bottom of the part. Both are dimensionless and
independent of the tile size, so changing the tile size does not re-slice
anything. Between two cuts the two are interpolated; the rise's integral is
evaluated exactly for an outline whose length runs linearly across the step, so v
grows correctly where a part drafts in or out.

The seam is on the -Y side, running counterclockwise seen from +Z. That is where
`project_cylindrical()` already puts its own u discontinuity, so switching a
layer between the two mappings leaves its texture where it was.

## From a place to a texture coordinate

`compute_outline_wrap_uvs()` and `compute_outline_wrap_corner_uvs()` in
`TextureDisplacement.cpp` turn places into the millimetre coordinates every other
mapping produces, which the layer's tiling, rotation and offset are then applied
to.

One repeat loop is `n` tile sizes long, where `n` is the whole number of tiles
nearest to fitting the mean outline length, at least 1. Then
`u = share * n * tile` and `v = -rise * n * tile`. Dividing by the tile size when
the texture is sampled therefore advances u by exactly `n` repeats over a full
loop — at every height, because every height is given the same `n`. The pattern
closes on itself at the seam instead of meeting a part-repeat there, and on a
part whose cross-section changes along Z the repeats stay whole and merely
stretch a little, rather than drifting out of register as the part widens.

A rotation that is not a multiple of 90 degrees mixes u and v, so the whole
number of repeats along the loop is no longer whole along the image's own axes
and the seam shows. This is inherent to rotating a wrap that closes in one
direction only, and is stated in the mapping's tooltip rather than worked around.

Where a cut is unusable the point takes `project_cylindrical()` about the very
same axis instead, shifted by half a loop so the two agree on where the seam is —
the cylindrical angle is measured from +Y and so reads zero a half turn past the
seam, where the wrap reads half a loop. The two still part company away from the
seam, which is the difference between measuring along an outline and measuring by
angle, but a band of a part that cannot be followed degrades to the behaviour of
the neighbouring mapping rather than to an arbitrary offset into the texture. When
no cut of the part is usable at all, both functions return nothing and the layer
falls back to `Cylindrical` entirely, the same way `LSCM` falls back to
`Triplanar` for a patch that will not flatten.

## Where it plugs in

Outline wrap is a formula from a point to a coordinate, like `Cylindrical`, but
one that needs the whole part's silhouette rather than just a centre and an axis.
It is therefore evaluated with the precomputed placements — `patch_uvs` and
`patch_corner_uvs` in `build_texture_displacement_in_place()`, and `LscmLookup`
in `prepare_sampleable_layers()` — rather than inside `sample_layer_height()`,
which only receives a point. Those two sampling functions keep an `OutlineWrap`
case alongside `Cylindrical` purely for the fallback above.

The per-corner form exists for the consumers that sample per triangle: the bake's
colour pass and the gizmo's fast preview. It brings the other two corners of each
triangle within half a loop of the first, so a triangle straddling the seam
interpolates across it instead of running the whole texture backwards through
that one triangle. Because a loop is a whole number of repeats, the shifted
corner still samples the same texel. The per-vertex form needs no such shift:
displacement is a point sample per vertex, and u jumps at the seam by exactly
that whole number of repeats.

Building the tables slices the whole mesh, so the result is cached on the mesh's
geometry alone (`cached_outline_wrap()`). Dragging any slider on the layer costs
a hash rather than a re-slice.

The gizmo's shaded preview uses the same two functions over the volume's mesh in
the same world frame the bake measures in, so the preview and the bake agree by
construction. When they return nothing, `layer_projection_frame()` reports the
cylindrical frame, so the preview shader falls back exactly as the bake does.

## Limitations

- A face turned along Z — the top or bottom of a box — has no meaningful place on
  an outline around Z, and the texture smears radially across it. This is the
  same behaviour `Cylindrical` has there; the mapping is for the sides of a part.
- The outline is followed as sliced, so a recess cut into a side wall (an
  engraved letter, say) is part of the outline and lengthens it. Over the height
  of such a recess the pattern shifts slightly relative to the part above and
  below it.

## Project files

The method is stored as `TextureProjectionMethod`'s integer value in the layer
JSON under `TEXTURE_DISPLACEMENT_KEY`, as every other method is. A project
written before the method existed carries no new keys and loads unchanged. A
project carrying a method an older build does not know reads back as the default
`Triplanar` rather than as whichever enumerator the number happens to land on.
