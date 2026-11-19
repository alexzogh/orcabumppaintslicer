#ifndef slic3r_McpPairingDialog_hpp_
#define slic3r_McpPairingDialog_hpp_

#include <wx/string.h>

#include "slic3r/GUI/GUI_Utils.hpp"

class wxWindow;

namespace Slic3r { namespace GUI { namespace mcp {

// The dialog a person points their phone or tablet at.
//
// Pairing has to cross from this machine to a device that shares nothing with it: the device needs
// an address, a bearer token and the fingerprint of the certificate it should accept, and two of
// those are sixty-four hex characters that nobody is going to retype. So the whole pairing URI
// goes into a QR code, and the parts a person might want to check - the address, the fingerprint -
// are repeated underneath as text.
//
// Opens whatever the state of the server: when the network listener is not running there is
// nothing to scan, and saying so plainly is more use than a greyed-out menu entry that does not
// explain itself.
class McpPairingDialog : public DPIDialog
{
public:
    explicit McpPairingDialog(wxWindow *parent);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    wxString m_uri;
};

// Opens the dialog modally, parented to the main frame.
void show_pairing_dialog(wxWindow *parent);

}}} // namespace Slic3r::GUI::mcp

#endif // slic3r_McpPairingDialog_hpp_
