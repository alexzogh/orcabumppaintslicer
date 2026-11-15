# Mixing a paint color

Painting assigns each triangle a *filament slot*, and a slot prints in whatever is loaded. Asking
for a color the printer does not carry therefore needs two things: filaments that together look like
it, and a way to make the printer alternate between them. Orca already has both. The first is
`libslic3r/ColorDecomposeRecipe.hpp`, which weighs every pair and triple of the loaded filaments
against a table of measured blends. The second is a **mixed filament slot**
(`libslic3r/FilamentMixer.hpp`), a virtual filament that interleaves several physical ones sublayer
by sublayer, resolved in `GCode/ToolOrdering.cpp`.

The Color section of the multi-material paint gizmo is where those two meet while painting. It adds
no color science of its own.

## Why the decompose dialog is not enough on its own

The sidebar already reaches the same solver: right-click a filament, *Decompose color*, and
`Sidebar::decompose_filament_color()` runs `ColorDecomposeDialog` and then
`create_mixed_filament_from_result()`. That flow starts from an existing slot's color, which is the
right shape when setting a project up.

It is the wrong shape in the middle of painting. There the color wanted is the one about to be
brushed on, and no slot holds it yet, so reaching it means leaving the gizmo, adding a filament,
setting its color, decomposing it, and coming back. The Color section takes the color first and
finds or builds the slot behind it, leaving that slot selected as the brush.

What sits under the picker is the existing machinery and nothing else:

- `Sidebar::collect_physical_filament_info()` names the candidates. It is also what decides which
  slots are physical, so the gizmo does not keep a second opinion about that.
- `recommend_from_physical_filaments()` chooses among them.
- `Sidebar::ensure_mixed_filament()` reuses a slot that already blends those filaments in those
  proportions, and only creates one when none does. Painting several shades therefore does not fill
  the project with near-duplicate slots.

## The second swatch

Next to the color asked for sits the color that will actually print. A palette reaches only the
colors spanned by its filaments, and nothing else in the flow reveals that a color lies outside that
span — without the swatch the first sign would be the finished print.

It shows the recipe's `matched_color_hex` rather than an average of the component colors, because
`blend_color_multi()` prefers a measured blend from the standard recipe table wherever one exists.
Where the two differ, the measured one is what comes off the printer.

## Material preference

A mix of filaments that want different temperatures prints badly whatever color it is, so the solver
is asked to prefer the material already being painted with. Only a physical slot names a material;
painting with a mixed slot leaves the preference empty, which the solver reads as no preference
rather than as a constraint it cannot satisfy.

## The gradient

The gradient interpolates between two colors, solves each step on its own, and paints the steps as
bands along a world axis across whatever is already painted with the selected filament — or across
the whole object when nothing is.

World space, not object space: the axis named in the panel is the one the user sees, and a rotated
object would otherwise fade along a direction that is not the one they picked. The ramp is stretched
across the painted region's own extent rather than the build volume's, so a gradient over a small
region is a full fade rather than a sliver of one.

This is a different thing from the Z gradient a mixed filament slot can carry
(`FilamentMixer.hpp`'s `GradientCurve`), which varies one slot's ratio continuously with height
across the whole print and costs a single slot. For a plain fade up a model that one is better. This
one earns its place where that one cannot reach: along X or Y, and confined to a painted region.

## Constraints

- **Slot budget.** Every mix is an ordinary filament slot, drawn from the same budget as physical
  ones (`EnforcerBlockerType::ExtruderMax`). A gradient needs one per step, which is why the step
  count is capped well below what a smooth ramp would want.
- **Ratios.** The solver will not mix below 20% of any component. Interleaving a filament more
  finely than that asks for a sublayer pattern the slicer cannot hold reliably.
- **Mesh resolution.** Painting is per whole triangle, so a gradient band boundary follows the mesh.
  On a coarse model the ramp is visibly stepped for that reason rather than because of the step
  count.
- **Cost.** A solve scans the measured table for every candidate pair and triple, which is slow
  enough to matter. The panel keeps each picker's answer until its color moves and skips re-solving
  while a drag is in progress; the gradient shows a busy cursor because it solves once per step.
