# Automatic splitting — High Level Design

## Purpose and scope

Automatic splitting cuts an object into pieces for a reason the user states, instead of asking them to place each
plane by hand in the Cut tool. The reason in place today is *Split to Fit the Plate*: the object is larger than the
printer, and the pieces have to be small enough to print. The same machinery is meant to carry further reasons —
splitting a painted model so each piece prints in one filament, splitting to reduce overhangs — because the part
that differs between them is only the choice of planes.

It is reached from *Modify → Split to Fit the Plate* in the object context menu. It does nothing to an object that
already fits, and it is absent from the menu for SLA and for objects that came out of the Cut tool.

The geometry lives in `src/libslic3r/AutoSplit.{hpp,cpp}` and the menu and plater wiring in
`src/slic3r/GUI/GUI_Factories.cpp` and `src/slic3r/GUI/Plater.cpp`.

## Planning and execution are separate

`AutoSplit` is in two halves that meet at a `Plan`.

**Planning** takes an `indexed_triangle_set` and returns a `Plan`: a binary tree of `PlanNode`, each a plane plus
the connectors to seat on it, each side pointing either at the node that splits it further or at nothing, meaning
that side is a finished piece. Planning touches no `Model`, no instance and no GUI, so a strategy can be tested on
a bare mesh — which is what `tests/libslic3r/test_auto_split.cpp` does. A strategy is a function that produces a
`Plan`; `plan_fit_build_plate()` is the first.

**Execution** is shared. `execute()` walks the tree, drives the existing `Cut` class once per node, and appends the
leaves to a `Model` the caller owns. Nothing about it is specific to why the planes were chosen, so a new strategy
costs a planner and nothing else.

This split is why the plan is a tree of planes rather than a list: `Cut` cuts one object into two, so the pieces
after the first cut are different objects, and the planes that apply to each of them have to be known per piece.

## Cut space

Every position in a `Plan` is in the frame `Cut` applies its matrix in: the instance's rotation, scaling and
mirroring are baked into the mesh and the instance's offset is removed, which makes it world space less the
instance offset. `cut_space_mesh()` produces a mesh in it and `cut_space_matrix()` the transform that gets there.

Because the frame is defined by the offset alone, a plane stays valid across a cut up to a translation: `execute()`
carries the difference between the original instance offset and each piece's, and shifts the planes left in that
piece's subtree by it. Plane normals never need adjusting, since the frame's axes are the world axes whatever the
instance is rotated to.

Pieces keep the position and orientation they had inside the object, which is what lets the planes stay meaningful
between cuts. Laying them out is left to the caller; the plater runs the ordinary arrange afterwards.

## Choosing planes to fit the plate

`plan_fit_build_plate()` works on a region of the model, which starts as the whole of it. For a region it measures
what the model occupies there, and if that fits the usable volume the region is a finished piece. Otherwise it
takes the axis that overshoots the plate by the largest factor, works out how many equal slabs that axis needs —
`ceil(extent / limit)` — and cuts in the middle of that run, so the two halves are a whole number of slabs each.
Recursing on both halves bottoms out in slabs that all fit and are close to the same size.

Cutting the most-overshooting axis in the middle rather than slicing plate-sized slabs off one end matters for two
reasons: the pieces come out a similar size, so they take a similar time to print, and no piece ends up a sliver.
The piece count is the same as slicing each axis independently would give, and lower for a model whose arms do not
fill its bounding box, because a region is measured on the geometry actually in it rather than on its share of the
box.

The extent of a region is measured by clamping each face's vertices into it. That is exact on every axis the region
is bounded on — which is every axis a cut has been made on, and so every axis the fit test can turn on — and on an
unbounded axis a face reaching only partly into the region still contributes its whole span. The measurement can
therefore come out slightly large, which costs an occasional extra cut and never leaves a piece too big.

Planning refuses rather than returning a plan it cannot keep: if the slabs would be thinner than
`FitPlateParams::min_piece_size`, or there would be more pieces than `max_pieces`. The reason comes back as an
`AutoSplit::Error`, not a sentence, because libslic3r has no translation lookup; `Plater` turns the code into
words.

The usable volume comes from `usable_size()`, the bed's bounding volume less a clearance on each side in X and Y so
the pieces still arrange with a gap between them. On a round bed the bounding box is mostly unreachable, so the
inscribed square is used instead.

## Connectors

The planner puts connectors on each plane so the pieces locate and glue together. It slices the model at the plane,
clips the cross-section to the region the piece owns, and erodes it by the connector radius plus a margin, which
leaves the area a connector can sit in without breaking out through a wall. Points are taken from that area on a
grid, widened until there are few enough of them, so they spread over the face rather than crowd one end. A face
with nothing left after the erosion — a thin slab, say — gets no connectors, which is correct: there is no room for
one.

`execute()` hangs them on the object as `CutConnector`s and the negative volumes `Cut` expects, exactly as the Cut
gizmo does, so the existing connector types, styles and shapes all apply and no second joint system exists.
`make_cut_connector_mesh()` in `CutUtils` builds the unit shape and is shared with the gizmo.

The cross-section is taken from the original solid, which does not know about holes an earlier cut's connectors
left. A plane passing through an earlier connector can therefore seat one where material has been removed.

## What it does not do

The pieces are not a cut set: `cut_id` is cleared on them, so the Cut gizmo does not treat them as halves of one
cut whose connectors it keeps in sync. That machinery is built around a two-piece cut and does not describe a tree
of a dozen.

Planes are axis-aligned in cut space. A model that would fit in fewer pieces cut on a diagonal, or rotated first,
is cut into more than the minimum. Auto-orientation (`src/libslic3r/Orient.cpp`) is the existing tool for the
rotation half of that and is left to the user to run first.
