#include "McpPairingDialog.hpp"

#include <cstring>
#include <string>

#include <wx/button.h>
#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/image.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/statbmp.h>
#include <wx/textctrl.h>

#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/format.hpp"

#include "McpQrImage.hpp"
#include "McpService.hpp"

namespace Slic3r { namespace GUI { namespace mcp {

namespace {

// Big enough to scan comfortably off a laptop screen held at arm's length, small enough that the
// dialog still fits a modest display once a version 7-ish symbol has its quiet zone.
constexpr int qr_module_pixels = 5;

wxStaticText *paragraph(wxWindow *parent, const wxString &text, int width)
{
    auto *label = new wxStaticText(parent, wxID_ANY, text);
    label->Wrap(width);
    return label;
}

// A value a person reads off the screen and compares with what their device shows. Read-only, but
// a text control rather than a label so it can be selected and copied, which is the whole point of
// showing a fingerprint at all.
wxTextCtrl *readonly_value(wxWindow *parent, const wxString &text)
{
    auto *field = new wxTextCtrl(parent, wxID_ANY, text, wxDefaultPosition, wxDefaultSize,
                                 wxTE_READONLY | wxTE_CENTRE);
    field->SetFont(wxGetApp().code_font());
    return field;
}

} // namespace

McpPairingDialog::McpPairingDialog(wxWindow *parent)
    : DPIDialog(parent, wxID_ANY, _L("Pair a device"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
{
    SetFont(wxGetApp().normal_font());

    const int   margin = FromDIP(12);
    const int   wrap   = FromDIP(420);
    auto       *sizer  = new wxBoxSizer(wxVERTICAL);
    const auto  pairing = McpService::instance().pairing();

    if (!pairing.valid()) {
        // Three different reasons land here and they want different advice, so say which one it is
        // rather than printing one apologetic sentence that covers none of them.
        wxString explanation;
        if (!McpService::instance().is_running())
            explanation = format_wxstr(_L("OrcaSlicer is not running its control server. Set %1% in the "
                                          "environment before starting it."),
                                       "ORCA_MCP");
        else if (!McpService::instance().serves_lan())
            explanation = format_wxstr(_L("The control server is running, but only for this machine. Set "
                                          "%1% in the environment before starting OrcaSlicer to let other "
                                          "devices reach it."),
                                       "ORCA_MCP_LAN");
        else
            explanation = _L("This machine has no network address to advertise, so a device on the "
                             "network has no way to reach it.");

        sizer->Add(paragraph(this, explanation, wrap), 0, wxALL, margin);
    } else {
        m_uri = from_u8(pairing.uri);

        sizer->Add(paragraph(this, _L("Scan this with the companion app to pair it with this copy of "
                                      "OrcaSlicer."),
                             wrap),
                   0, wxLEFT | wxRIGHT | wxTOP, margin);

        const QrImage qr = render_qr(pairing.uri, FromDIP(qr_module_pixels));
        if (qr.empty()) {
            sizer->Add(paragraph(this, _L("The pairing code could not be drawn. The link below carries "
                                          "the same information."),
                                 wrap),
                       0, wxALL, margin);
        } else {
            // wxImage takes ownership of a buffer handed to it with static_data false; copying into
            // one it allocates is simpler and the image is a few hundred kilobytes at most.
            wxImage image(qr.width, qr.width);
            std::memcpy(image.GetData(), qr.rgb.data(), qr.rgb.size());
            auto *bitmap = new wxStaticBitmap(this, wxID_ANY, wxBitmap(image));
            sizer->Add(bitmap, 0, wxALIGN_CENTER_HORIZONTAL | wxALL, margin);
        }

        auto *address_label = new wxStaticText(this, wxID_ANY, _L_CONTEXT("Address", "Network"));
        sizer->Add(address_label, 0, wxLEFT | wxRIGHT, margin);
        sizer->Add(readonly_value(this, format_wxstr("%1%:%2%", pairing.host, pairing.port)), 0,
                   wxEXPAND | wxLEFT | wxRIGHT, margin);

        auto *fingerprint_label = new wxStaticText(this, wxID_ANY, _L("Certificate fingerprint"));
        sizer->Add(fingerprint_label, 0, wxLEFT | wxRIGHT | wxTOP, margin);
        sizer->Add(readonly_value(this, from_u8(pairing.fingerprint)), 0, wxEXPAND | wxLEFT | wxRIGHT, margin);

        sizer->Add(paragraph(this,
                             _L("The pairing link contains a key that grants full control of this "
                                "application. Treat it like a password: anyone who photographs this code "
                                "can drive your slicer."),
                             wrap),
                   0, wxALL, margin);
    }

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    if (pairing.valid()) {
        auto *copy = new wxButton(this, wxID_ANY, _L("Copy pairing link"));
        copy->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            if (wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(m_uri));
                wxTheClipboard->Close();
            }
        });
        buttons->Add(copy, 0, wxRIGHT, margin);
    }
    buttons->Add(new wxButton(this, wxID_CANCEL, _L("Close")), 0);
    sizer->Add(buttons, 0, wxALIGN_RIGHT | wxALL, margin);

    SetSizerAndFit(sizer);
    sizer->SetSizeHints(this);
    CentreOnParent();
}

void McpPairingDialog::on_dpi_changed(const wxRect & /* suggested_rect */)
{
    // Everything here is laid out from FromDIP() values, so refitting is enough; the QR itself is
    // redrawn at the new scale the next time the dialog is opened.
    Fit();
    Refresh();
}

void show_pairing_dialog(wxWindow *parent)
{
    McpPairingDialog dialog(parent != nullptr ? parent : static_cast<wxWindow *>(wxGetApp().mainframe));
    dialog.ShowModal();
}

}}} // namespace Slic3r::GUI::mcp
