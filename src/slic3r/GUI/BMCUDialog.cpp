#include "BMCUDialog.hpp"
#include "GUI_App.hpp"
#include "GUI.hpp"
#include "I18N.hpp"
#include "OptionsGroup.hpp"
#include "Widgets/WebView.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/app.h>
#include <wx/button.h>
#include <wx/clrpicker.h>
#include <wx/combobox.h>
#include <wx/msgdlg.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/webview.h>

#include <utility>
#include <algorithm>

namespace Slic3r::GUI {

void append_bmcu_settings(ConfigOptionsGroup &group, const DynamicPrintConfig &config)
{
    for (const char *key : { "bmcu_enabled", "bmcu_transport", "bmcu_modules", "bmcu_endpoint" })
        group.append_single_option_line(key);
    Line line { L("BMCU channels"), L("Detect modules and configure each channel's material and color.") };
    line.widget = [&config](wxWindow *parent) -> wxSizer * {
        auto *layout = new wxBoxSizer(wxHORIZONTAL);
        auto *button = new wxButton(parent, wxID_ANY, _(L("Manage BMCU")));
        button->Bind(wxEVT_BUTTON, [&config, parent](wxCommandEvent &) {
            if (!config.opt_bool("bmcu_enabled")) {
                wxMessageDialog(parent, _(L("Enable BMCU in this printer profile first.")), "BMCU", wxOK | wxICON_INFORMATION).ShowModal();
                return;
            }
            const DynamicPrintConfig *physical = wxGetApp().preset_bundle->physical_printers.get_selected_printer_config();
            if (!physical) physical = &config;
            auto endpoint = config.opt_string("bmcu_endpoint");
            if (endpoint.empty()) endpoint = physical->opt_string("print_host");
            if (!endpoint.empty() && endpoint.find("://") == std::string::npos) endpoint = "http://" + endpoint;
            BMCUDialog(parent, config.opt_string("bmcu_transport"), endpoint,
                physical->opt_string("printhost_apikey")).ShowModal();
        });
        layout->Add(button, 0, wxALL, 2);
        return layout;
    };
    group.append_line(line);
}

BMCUDialog::BMCUDialog(wxWindow *parent, const std::string &transport, const std::string &endpoint, const std::string &api_key)
    : wxDialog(parent, wxID_ANY, "BMCU", wxDefaultPosition, wxSize(760, 540), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_transport(transport)
    , m_endpoint(endpoint)
    , m_api_key(api_key)
    , m_alive(std::make_shared<std::atomic_bool>(true))
{
    auto *layout = new wxBoxSizer(wxVERTICAL);
    auto *actions = new wxBoxSizer(wxHORIZONTAL);
    m_status = new wxStaticText(this, wxID_ANY, "");
    m_refresh = new wxButton(this, wxID_ANY, "Detect / Refresh");
    actions->Add(m_refresh, 0, wxALL, 8);
    if (m_transport == "moonraker") {
        auto *advanced = new wxButton(this, wxID_ANY, "Full BMCU controls");
        actions->Add(advanced, 0, wxALL, 8);
        advanced->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { open_full_panel(); });
    } else if (m_transport == "prusalink") {
        auto *external = new wxButton(this, wxID_ANY, "External spool");
        actions->Add(external, 0, wxALL, 8);
        external->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            m_status->SetLabel("Selecting external spool...");
            auto guard = m_alive;
            BMCU::select_prusalink(m_endpoint, m_api_key, 16, [this, guard](std::string error) {
                wxTheApp->CallAfter([this, guard, error = std::move(error)] {
                    if (guard->load()) m_status->SetLabel(error.empty() ? "External spool selected" : wxString::FromUTF8(error.c_str()));
                });
            });
        });
    }
    actions->Add(m_status, 1, wxALIGN_CENTER_VERTICAL | wxALL, 8);
    layout->Add(actions, 0, wxEXPAND);
    const char *capabilities = m_transport == "moonraker"
        ? "Klipper / Moonraker: live BMCU device detection and filament synchronization."
        : m_transport == "prusalink"
        ? "PrusaLink (local): BMCU status and direct color/material updates require compatible BMCU Buddy firmware."
        : "Prusa Connect does not expose a BMCU metadata API. Cloud changes are disabled; use PrusaLink on the local network.";
    auto *capability_notice = new wxStaticText(this, wxID_ANY, capabilities);
    capability_notice->Wrap(670);
    layout->Add(capability_notice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 12);

    m_scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL);
    m_scroll->SetScrollRate(0, 12);
    m_rows = new wxBoxSizer(wxVERTICAL);
    m_scroll->SetSizer(m_rows);
    layout->Add(m_scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, 10);

    auto *close = new wxButton(this, wxID_CLOSE, "Close");
    layout->Add(close, 0, wxALIGN_RIGHT | wxALL, 10);
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });
    m_refresh->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { refresh(); });
    SetSizer(layout);
    CentreOnParent();
    refresh();
}

BMCUDialog::~BMCUDialog()
{
    m_alive->store(false);
}

void BMCUDialog::refresh()
{
    m_rows->Clear(true);
    m_scroll->FitInside();
    if (m_transport == "prusaconnect") {
        m_status->SetLabel("BMCU updates over Prusa Connect are not supported; select PrusaLink instead");
        return;
    }
    if (m_transport != "moonraker" && m_transport != "prusalink") {
        m_status->SetLabel("Unsupported BMCU transport");
        return;
    }
    if (m_endpoint.empty()) {
        m_status->SetLabel("Enter printer connection address in BMCU settings");
        return;
    }
    m_refresh->Disable();
    m_status->SetLabel("Discovering BMCU modules...");
    auto guard = m_alive;
    auto callback = [this, guard](std::vector<BMCU::Slot> slots, std::string error) {
        wxTheApp->CallAfter([this, guard, slots = std::move(slots), error = std::move(error)] {
            if (!guard->load()) return;
            m_refresh->Enable();
            if (!error.empty()) {
                m_status->SetLabel(wxString::FromUTF8(error.c_str()));
                return;
            }
            show_slots(slots);
        });
    };
    if (m_transport == "prusalink") BMCU::discover_prusalink(m_endpoint, m_api_key, std::move(callback));
    else BMCU::discover(m_endpoint, m_api_key, std::move(callback));
}

void BMCUDialog::open_full_panel()
{
    m_status->SetLabel("Finding BMCU-Klipper panel...");
    auto guard = m_alive;
    BMCU::discover_panel(m_endpoint, m_api_key, [this, guard](std::string url) {
        wxTheApp->CallAfter([this, guard, url = std::move(url)] {
            if (!guard->load()) return;
            if (url.empty()) {
                m_status->SetLabel("BMCU-Klipper panel is not available from Moonraker");
                return;
            }
            wxDialog window(this, wxID_ANY, "BMCU-Klipper", wxDefaultPosition, wxSize(1120, 820), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
            auto *browser = WebView::CreateWebView(&window, wxString::FromUTF8(url.c_str()));
            if (!browser) {
                m_status->SetLabel("WebView backend unavailable");
                return;
            }
            auto *layout = new wxBoxSizer(wxVERTICAL);
            layout->Add(browser, 1, wxEXPAND);
            window.SetSizer(layout);
            window.CentreOnParent();
            window.ShowModal();
        });
    });
}

void BMCUDialog::show_slots(const std::vector<BMCU::Slot> &slots)
{
    m_rows->Clear(true);
    if (slots.empty()) {
        m_status->SetLabel("No BMCU modules detected");
        m_scroll->FitInside();
        return;
    }
    size_t connected = 0;
    for (const auto &slot : slots) {
        if (slot.ready) ++connected;
        const bool editable = slot.ready;
        if (slot.channel == 0) {
            auto *heading = new wxStaticText(m_scroll, wxID_ANY, "BMCU " + wxString::FromUTF8(slot.label.substr(0, 1).c_str()));
            auto font = heading->GetFont();
            font.SetWeight(wxFONTWEIGHT_BOLD);
            heading->SetFont(font);
            m_rows->Add(heading, 0, wxTOP | wxLEFT | wxBOTTOM, 8);
        }
        auto *line = new wxBoxSizer(wxHORIZONTAL);
        auto *name = new wxStaticText(m_scroll, wxID_ANY, wxString::FromUTF8(slot.label.c_str()));
        name->SetMinSize(wxSize(42, -1));
        line->Add(name, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

        wxColour color(wxString::FromUTF8(slot.color.c_str()));
        if (!color.IsOk()) color = *wxWHITE;
        auto *picker = new wxColourPickerCtrl(m_scroll, wxID_ANY, color);
        line->Add(picker, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

        wxArrayString materials;
        for (const char *type : { "PLA", "PETG", "ABS", "ASA", "TPU", "FLEX", "PA", "PC", "PVA", "PVB", "HIPS", "PP", "PPS", "PPA", "BVOH" })
            materials.Add(type);
        auto *material = new wxComboBox(m_scroll, wxID_ANY, wxString::FromUTF8(slot.material.c_str()), wxDefaultPosition, wxSize(110, -1), materials, wxCB_DROPDOWN | wxTE_PROCESS_ENTER);
        line->Add(material, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 12);

        auto *state = new wxStaticText(m_scroll, wxID_ANY, slot.ready ? wxString::FromUTF8(slot.state.c_str()) : "Offline");
        line->Add(state, 1, wxALIGN_CENTER_VERTICAL);
        picker->Enable(editable);
        material->Enable(editable);

        picker->Bind(wxEVT_COLOURPICKER_CHANGED, [this, slot, material](wxColourPickerEvent &event) {
            update(slot, material->GetValue().ToStdString(), event.GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
        });
        auto apply_material = [this, slot, picker] (wxString text) {
            update(slot, text.ToStdString(), picker->GetColour().GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
        };
        material->Bind(wxEVT_COMBOBOX, [apply_material, material](wxCommandEvent &) { apply_material(material->GetValue()); });
        material->Bind(wxEVT_TEXT_ENTER, [apply_material, material](wxCommandEvent &) { apply_material(material->GetValue()); });

        if (m_transport == "prusalink" && slot.ready && slot.tool >= 0 &&
            slot.state != "In use" && slot.state != "Selected" && slot.state != "Reload pending") {
            auto *select = new wxButton(m_scroll, wxID_ANY, "Select path", wxDefaultPosition, wxSize(100, -1));
            line->Add(select, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
            select->Bind(wxEVT_BUTTON, [this, slot](wxCommandEvent &) {
                m_status->SetLabel("Selecting BMCU path " + wxString::FromUTF8(slot.label.c_str()));
                auto guard = m_alive;
                BMCU::select_prusalink(m_endpoint, m_api_key, slot.tool, [this, guard](std::string error) {
                    wxTheApp->CallAfter([this, guard, error = std::move(error)] {
                        if (!guard->load()) return;
                        if (!error.empty()) m_status->SetLabel(wxString::FromUTF8(error.c_str()));
                        else refresh();
                    });
                });
            });
        }

        const auto *colors = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        const auto &preset_names = wxGetApp().preset_bundle->filament_presets;
        if (editable && slot.tool >= 0 && colors && static_cast<size_t>(slot.tool) < colors->values.size()
            && static_cast<size_t>(slot.tool) < preset_names.size()) {
            auto *from_slicer = new wxButton(m_scroll, wxID_ANY, "Use slicer", wxDefaultPosition, wxSize(102, -1));
            line->Add(from_slicer, 0, wxALIGN_CENTER_VERTICAL);
            from_slicer->Bind(wxEVT_BUTTON, [this, slot, picker, material](wxCommandEvent &) {
                const auto &project = wxGetApp().preset_bundle->project_config;
                const auto *colors = project.option<ConfigOptionStrings>("filament_colour");
                const auto &presets = wxGetApp().preset_bundle->filament_presets;
                if (!colors || slot.tool < 0 || static_cast<size_t>(slot.tool) >= colors->values.size()
                    || static_cast<size_t>(slot.tool) >= presets.size()) return;
                auto *preset = wxGetApp().preset_bundle->filaments.find_preset(presets[slot.tool]);
                if (!preset) return;
                std::string display;
                const auto type = preset->get_filament_type(display);
                const auto &color = colors->values[slot.tool];
                wxColour selected(wxString::FromUTF8(color.c_str()));
                if (!selected.IsOk() || type.empty()) return;
                picker->SetColour(selected);
                material->SetValue(wxString::FromUTF8(type.c_str()));
                update(slot, type, color);
            });
        }

        m_rows->Add(line, 0, wxEXPAND | wxALL, 8);
    }
    m_status->SetLabel(wxString::Format("%zu slots, %zu online", slots.size(), connected));
    m_scroll->Layout();
    m_scroll->FitInside();
}

void BMCUDialog::update(const BMCU::Slot &slot, const std::string &material, const std::string &color)
{
    m_status->SetLabel("Updating " + wxString::FromUTF8(slot.label.c_str()) + "...");
    if (m_transport == "prusalink") {
        auto guard = m_alive;
        BMCU::update_prusalink(m_endpoint, m_api_key, slot, material, color, [this, guard, label = slot.label](std::string error) {
            wxTheApp->CallAfter([this, guard, label, error = std::move(error)] {
                if (guard->load()) m_status->SetLabel(error.empty() ? "Updated " + wxString::FromUTF8(label.c_str()) : wxString::FromUTF8(error.c_str()));
            });
        });
        return;
    }
    if (m_transport == "prusaconnect") {
        m_status->SetLabel("Prusa Connect cannot update BMCU metadata; use PrusaLink");
        return;
    }
    auto guard = m_alive;
    BMCU::set_filament(m_endpoint, m_api_key, slot, material, color, [this, guard, label = slot.label](std::string error) {
        wxTheApp->CallAfter([this, guard, label, error = std::move(error)] {
            if (!guard->load()) return;
            m_status->SetLabel(error.empty() ? "Updated " + wxString::FromUTF8(label.c_str()) : wxString::FromUTF8(error.c_str()));
        });
    });
}

}
