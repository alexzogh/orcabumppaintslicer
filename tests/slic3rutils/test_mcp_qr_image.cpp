#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "slic3r/GUI/MCP/McpQrImage.hpp"
#include "slic3r/GUI/MCP/McpService.hpp"

using Slic3r::GUI::mcp::QrImage;
using Slic3r::GUI::mcp::make_pairing_uri;
using Slic3r::GUI::mcp::render_qr;

namespace {

// There is no QR decoder in the tree to check a rendered code against, so these tests assert the
// structural properties the standard fixes: where the finder patterns sit, that the timing rows
// alternate, that the quiet zone is clear. A renderer that got the scaling, the offset or the
// orientation wrong fails them, which is what actually goes wrong here.
bool dark_module(const QrImage &image, int module_x, int module_y)
{
    const int x = (module_x + image.quiet) * image.module_size + image.module_size / 2;
    const int y = (module_y + image.quiet) * image.module_size + image.module_size / 2;
    return image.dark_at(x, y);
}

// A pairing URI of the shape the service actually produces: a dotted-quad host, a port, and two
// sixty-four character hex strings. This is the payload the dialog has to fit.
std::string realistic_pairing_uri()
{
    const std::string token       = std::string(64, 'a');
    const std::string fingerprint = std::string(64, '7');
    return make_pairing_uri("192.168.1.20", 51820, token, fingerprint);
}

} // namespace

TEST_CASE("render_qr surrounds the code with a quiet zone", "[McpQrImage]")
{
    const QrImage image = render_qr("orcaslicer", 4, 4);
    REQUIRE_FALSE(image.empty());
    REQUIRE(image.width == (image.modules + 2 * image.quiet) * image.module_size);
    REQUIRE(image.rgb.size() == std::size_t(image.width) * std::size_t(image.width) * 3);

    const int border = image.quiet * image.module_size;
    for (int x = 0; x < image.width; ++x) {
        CHECK_FALSE(image.dark_at(x, border - 1));
        CHECK_FALSE(image.dark_at(x, image.width - border));
    }
    for (int y = 0; y < image.width; ++y) {
        CHECK_FALSE(image.dark_at(border - 1, y));
        CHECK_FALSE(image.dark_at(image.width - border, y));
    }
}

TEST_CASE("render_qr draws a finder pattern in three corners", "[McpQrImage]")
{
    const QrImage image = render_qr(realistic_pairing_uri(), 3, 4);
    REQUIRE_FALSE(image.empty());

    // Each finder is a 7x7 bullseye: dark outer ring, light ring inside it, solid 3x3 core. The
    // fourth corner deliberately has none, which is how a scanner works out the orientation.
    const std::vector<std::pair<int, int>> origins{
        {0, 0}, {image.modules - 7, 0}, {0, image.modules - 7}};
    for (const auto &origin : origins) {
        const int ox = origin.first;
        const int oy = origin.second;
        CHECK(dark_module(image, ox + 0, oy + 0));
        CHECK(dark_module(image, ox + 6, oy + 0));
        CHECK(dark_module(image, ox + 0, oy + 6));
        CHECK(dark_module(image, ox + 6, oy + 6));
        CHECK_FALSE(dark_module(image, ox + 1, oy + 1));
        CHECK_FALSE(dark_module(image, ox + 5, oy + 1));
        CHECK_FALSE(dark_module(image, ox + 1, oy + 5));
        CHECK(dark_module(image, ox + 2, oy + 2));
        CHECK(dark_module(image, ox + 3, oy + 3));
        CHECK(dark_module(image, ox + 4, oy + 4));
    }

    // The specification fixes one module as permanently dark, just above the bottom-left finder.
    // It is the cheapest single check that the grid is the right way up and the right size.
    CHECK(dark_module(image, 8, image.modules - 8));
}

TEST_CASE("render_qr draws alternating timing patterns", "[McpQrImage]")
{
    const QrImage image = render_qr(realistic_pairing_uri(), 3, 4);
    REQUIRE_FALSE(image.empty());

    // Row and column 6 alternate dark and light between the finder patterns, dark on even
    // coordinates. This is the line a scanner measures the module size against.
    for (int i = 8; i < image.modules - 8; ++i) {
        const bool should_be_dark = i % 2 == 0;
        CHECK(dark_module(image, i, 6) == should_be_dark);
        CHECK(dark_module(image, 6, i) == should_be_dark);
    }
}

TEST_CASE("render_qr scales every module into a solid block", "[McpQrImage]")
{
    const int     module_size = 5;
    const QrImage image       = render_qr("orcaslicer", module_size, 2);
    REQUIRE_FALSE(image.empty());
    REQUIRE(image.module_size == module_size);

    // The top-left finder's corner module is dark, and every pixel of it must be, or the code is
    // being drawn with gaps a scanner will read as noise.
    const int left = image.quiet * module_size;
    for (int y = left; y < left + module_size; ++y)
        for (int x = left; x < left + module_size; ++x)
            CHECK(image.dark_at(x, y));
}

TEST_CASE("render_qr is the same every time for the same text", "[McpQrImage]")
{
    const std::string uri = realistic_pairing_uri();
    CHECK(render_qr(uri, 3, 4).rgb == render_qr(uri, 3, 4).rgb);
}

TEST_CASE("render_qr distinguishes two pairing links", "[McpQrImage]")
{
    const std::string one = make_pairing_uri("192.168.1.20", 51820, std::string(64, 'a'), std::string(64, '7'));
    const std::string two = make_pairing_uri("192.168.1.20", 51820, std::string(64, 'b'), std::string(64, '7'));
    CHECK(render_qr(one, 3, 4).rgb != render_qr(two, 3, 4).rgb);
}

TEST_CASE("render_qr has nothing to draw for empty text", "[McpQrImage]")
{
    const QrImage image = render_qr("", 4, 4);
    CHECK(image.empty());
    CHECK(image.rgb.empty());
}

TEST_CASE("render_qr gives up rather than throwing when the text cannot fit", "[McpQrImage]")
{
    // Past the capacity of the largest version at any correction level. The dialog must get an
    // empty image back and fall through to showing the link as text, not an exception.
    const QrImage image = render_qr(std::string(8000, 'x'), 3, 4);
    CHECK(image.empty());
}

TEST_CASE("render_qr clamps a module size or quiet zone that makes no sense", "[McpQrImage]")
{
    const QrImage image = render_qr("orcaslicer", 0, -5);
    REQUIRE_FALSE(image.empty());
    CHECK(image.module_size == 1);
    CHECK(image.quiet == 0);
    CHECK(image.width == image.modules);
}

TEST_CASE("dark_at reads light outside the image", "[McpQrImage]")
{
    const QrImage image = render_qr("orcaslicer", 2, 1);
    REQUIRE_FALSE(image.empty());
    CHECK_FALSE(image.dark_at(-1, 0));
    CHECK_FALSE(image.dark_at(0, -1));
    CHECK_FALSE(image.dark_at(image.width, 0));
    CHECK_FALSE(image.dark_at(0, image.width));
}

TEST_CASE("render_qr fits a real pairing link in a scannable symbol", "[McpQrImage]")
{
    const QrImage image = render_qr(realistic_pairing_uri(), 5, 4);
    REQUIRE_FALSE(image.empty());
    // Versions 1 to 40 are 21 to 177 modules. Anything outside that is not a QR code at all.
    CHECK(image.modules >= 21);
    CHECK(image.modules <= 177);
    // A symbol that needed the largest versions would be unreadable at this pixel size on a
    // laptop screen; a link of this shape should sit comfortably in the middle of the range.
    CHECK(image.modules <= 77);
}
