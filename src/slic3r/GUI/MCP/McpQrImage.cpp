#include "McpQrImage.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>

#include <boost/log/trivial.hpp>

#include "qrcodegen.hpp"

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

constexpr unsigned char light = 255;
constexpr unsigned char dark  = 0;

} // namespace

bool QrImage::dark_at(int x, int y) const
{
    if (x < 0 || y < 0 || x >= width || y >= width)
        return false;
    return rgb[(std::size_t(y) * std::size_t(width) + std::size_t(x)) * 3] == dark;
}

QrImage render_qr(const std::string &text, int module_size, int quiet_modules)
{
    QrImage image;
    if (text.empty())
        return image;

    module_size   = std::max(module_size, 1);
    quiet_modules = std::max(quiet_modules, 0);

    try {
        // Medium correction. The code is being read off a screen from a few inches away rather
        // than off a scuffed label, so this is already more redundancy than the situation needs;
        // it buys tolerance for a poor angle without pushing the symbol up a version.
        const qrcodegen::QrCode code = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);

        image.modules     = code.getSize();
        image.quiet       = quiet_modules;
        image.module_size = module_size;
        image.width       = (image.modules + 2 * quiet_modules) * module_size;
        image.rgb.assign(std::size_t(image.width) * std::size_t(image.width) * 3, light);

        for (int module_y = 0; module_y < image.modules; ++module_y) {
            for (int module_x = 0; module_x < image.modules; ++module_x) {
                if (!code.getModule(module_x, module_y))
                    continue;
                const int left = (module_x + quiet_modules) * module_size;
                const int top  = (module_y + quiet_modules) * module_size;
                for (int y = top; y < top + module_size; ++y) {
                    unsigned char *row = image.rgb.data() + (std::size_t(y) * std::size_t(image.width) + std::size_t(left)) * 3;
                    std::fill(row, row + std::size_t(module_size) * 3, dark);
                }
            }
        }
    } catch (const std::exception &e) {
        // The only realistic failure is text too long for any version, which for a pairing URI
        // would mean the format had grown out of all recognition. Report nothing to draw rather
        // than take down the dialog.
        BOOST_LOG_TRIVIAL(error) << "MCP could not render a QR code: " << e.what();
        return QrImage();
    }

    return image;
}

}}} // namespace Slic3r::GUI::mcp
