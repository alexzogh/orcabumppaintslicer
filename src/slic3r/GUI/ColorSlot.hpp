#ifndef slic3r_ColorSlot_hpp_
#define slic3r_ColorSlot_hpp_

#include <string>
#include <vector>

#include "libslic3r/Color.hpp"
#include "libslic3r/ColorDecomposeRecipe.hpp"

namespace Slic3r { namespace GUI {

// Turning a wanted colour into a filament slot that prints it.
//
// Lives apart from the paint gizmo because it is not about painting: the MMU gizmo's Color section
// and the MCP interface both need it, and a second copy of the decomposition rules would drift from
// this one the first time either changed.

// The candidate filaments a colour can be mixed from: the loaded physical ones, never the mixed
// slots, since a mixed slot is itself an interleave of physical filaments rather than one in its
// own right.
//
// preferred_config_index is the config slot currently being painted with, or -1 for no preference.
// Interleaving two materials that want different temperatures prints badly whatever colour it comes
// out, so the solver is told which material to keep to; a mixed slot names no material, which reads
// as no preference.
void physical_filament_palette(std::vector<ColorDecomposePhysicalFilament> &palette,
                               int                                         preferred_config_index,
                               std::string                                &preferred_type);

struct ColorSlotResult
{
    // The 0-based filament slot to paint with, or -1 when no slot could be found or made.
    int slot = -1;

    // What that slot actually prints as, which is not the colour that was asked for: reachable
    // colours are bounded by what is loaded, so a request outside that range comes back visibly
    // different. Callers are expected to show this rather than swallow it.
    std::string matched_color_hex;

    // True when a new mixed slot had to be created rather than an existing one reused. Slots are
    // finite, so a caller looping over colours needs to know it is spending them.
    bool created = false;

    // The physical filaments and integer parts the slot interleaves. Empty when an existing
    // physical filament already prints the colour and no mixing was needed.
    std::vector<unsigned int> components;
    std::vector<int>          ratios;
};

// Finds a loaded filament that already prints `wanted`, or finds or creates a mixed slot that does.
//
// Must be called on the GUI thread: it reads the sidebar's filament list and may add a slot.
ColorSlotResult slot_for_color(const ColorRGB &wanted, int preferred_config_index);

}} // namespace Slic3r::GUI

#endif // slic3r_ColorSlot_hpp_
