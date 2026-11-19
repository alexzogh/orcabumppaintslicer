#ifndef slic3r_McpQrImage_hpp_
#define slic3r_McpQrImage_hpp_

#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace mcp {

// A QR code rendered to pixels, with no wxWidgets anywhere in it.
//
// Separated from the dialog for the usual reason: the dialog cannot be exercised without a running
// application, while scaling, the quiet zone and the pixel layout are arithmetic, and arithmetic
// that produces an unscannable code is the failure worth catching.
struct QrImage
{
    int modules     = 0; // QR modules per side, not counting the quiet zone
    int quiet       = 0; // width of the quiet zone, in modules
    int module_size = 0; // pixels per module
    int width       = 0; // pixels per side, including the quiet zone

    // Three bytes per pixel, row-major, top row first. Ready to hand to wxImage, which wants
    // exactly this and nothing else.
    std::vector<unsigned char> rgb;

    bool empty() const { return width == 0; }

    // Whether the pixel at (x, y) is dark. Out of bounds reads light, like the quiet zone.
    bool dark_at(int x, int y) const;
};

// Renders `text` as a QR code.
//
// Always black on white, whatever theme the application is wearing: a scanner is looking for dark
// modules on a light field, and a code inverted to match a dark interface is a code that does not
// read. The quiet zone is part of the symbol rather than decoration - the standard asks for four
// modules of clear space, and a code butted against a coloured dialog background often fails.
//
// Returns an empty QrImage when `text` is empty or will not fit in any QR version.
QrImage render_qr(const std::string &text, int module_size, int quiet_modules = 4);

}}} // namespace Slic3r::GUI::mcp

#endif // slic3r_McpQrImage_hpp_
