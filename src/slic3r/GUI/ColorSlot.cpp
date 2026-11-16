#include "ColorSlot.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>

#include "libslic3r/PresetBundle.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Plater.hpp"

namespace Slic3r { namespace GUI {

namespace {

size_t loaded_filament_count()
{
    const PresetBundle *bundle = wxGetApp().preset_bundle;
    return bundle == nullptr ? 0 : bundle->filament_presets.size();
}

} // namespace

void physical_filament_palette(std::vector<ColorDecomposePhysicalFilament> &palette,
                               int                                         preferred_config_index,
                               std::string                                &preferred_type)
{
    palette.clear();
    preferred_type.clear();

    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return;

    // The sidebar already keeps the mapping from physical filament to project config slot, so its
    // physical-only list is exactly the candidate set; rebuilding it here would only drift.
    std::vector<std::string> colors, names, types;
    std::vector<size_t>      config_indices;
    plater->sidebar().collect_physical_filament_info(colors, names, types, &config_indices);

    const size_t count = std::min({colors.size(), names.size(), types.size(), config_indices.size()});
    palette.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        ColorDecomposePhysicalFilament filament;
        filament.color_hex      = colors[i];
        filament.name           = names[i];
        filament.type           = types[i];
        filament.filament_index = unsigned(config_indices[i] + 1);
        palette.push_back(std::move(filament));

        if (preferred_config_index >= 0 && config_indices[i] == size_t(preferred_config_index))
            preferred_type = types[i];
    }
}

ColorSlotResult slot_for_color(const ColorRGB &wanted, int preferred_config_index)
{
    ColorSlotResult result;

    std::vector<ColorDecomposePhysicalFilament> palette;
    std::string                                 preferred_type;
    physical_filament_palette(palette, preferred_config_index, preferred_type);
    if (palette.size() < 2)
        return result;

    const ColorDecomposeRgb          target{wanted.r_uchar(), wanted.g_uchar(), wanted.b_uchar()};
    const ColorDecomposeRecipeResult recipe = recommend_from_physical_filaments(target, palette, preferred_type);
    if (!recipe.valid || recipe.components.empty())
        return result;

    result.matched_color_hex = recipe.matched_color_hex;

    // One component means a loaded filament already prints this colour. Spending a filament slot on
    // a "mix" of it with itself would buy nothing.
    if (recipe.components.size() == 1) {
        const unsigned int id = recipe.components.front().filament_index;
        result.slot           = id == 0 ? -1 : int(id) - 1;
        return result;
    }

    for (const ColorDecomposeRecipeComponent &component : recipe.components) {
        // A zero index is one of the solver's standard base colours rather than a loaded filament.
        // recommend_from_physical_filaments() only ever names loaded ones, but there is nothing to
        // interleave if that changes, so this does not guess at a substitute.
        if (component.filament_index == 0)
            return ColorSlotResult{-1, recipe.matched_color_hex, false, {}, {}};
        result.components.push_back(component.filament_index);
        result.ratios.push_back(component.ratio);
    }

    Plater *plater = wxGetApp().plater();
    if (plater == nullptr)
        return ColorSlotResult{-1, recipe.matched_color_hex, false, {}, {}};

    // Reuses a slot that already blends these filaments in these proportions, and only creates one
    // when none does - so painting several shades of the same colour does not fill the project with
    // near-duplicate slots. Counting either side is how we tell which of the two happened, since
    // ensure_mixed_filament reports only the slot.
    const size_t before = loaded_filament_count();
    result.slot         = plater->sidebar().ensure_mixed_filament(result.components, result.ratios);
    result.created      = loaded_filament_count() > before;
    return result;
}

}} // namespace Slic3r::GUI
