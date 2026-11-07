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
                physical->opt_string("printhost_apikey"), config.opt_int("bmcu_modules"), physical).ShowModal();
        });
        layout->Add(button, 0, wxALL, 2);
        return layout;
    };
    group.append_line(line);
}

BMCUDialog::BMCUDialog(wxWindow *parent, const std::string &transport, const std::string &endpoint, const std::string &api_key,
    int modules, const DynamicPrintConfig *print_host)
    : wxDialog(parent, wxID_ANY, "BMCU", wxDefaultPosition, wxSize(760, 540), wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_transport(transport)
    , m_endpoint(endpoint)
    , m_api_key(api_key)
    , m_alive(std::make_shared<std::atomic_bool>(true))
    , m_print_host(print_host ? std::make_unique<DynamicPrintConfig>(*print_host) : nullptr)
    , m_timer(this)
    , m_modules(std::max(1, std::min(modules, 4)))
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
    }
    actions->Add(m_status, 1, wxALIGN_CENTER_VERTICAL | wxALL, 8);
    layout->Add(actions, 0, wxEXPAND);
    const char *capabilities = m_transport == "moonraker"
        ? "Klipper / Moonraker: live device detection, color and material sync, and full BMCU controls."
        : m_transport == "prusalink"
        ? "PrusaLink (local): live BMCU status and direct color/material updates. Printer toolchanges require compatible firmware."
        : "Prusa Connect: uploads a small G-code print job; remote discovery and execution confirmation are not available.";
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
    Bind(wxEVT_TIMER, [this](wxTimerEvent &) { flush_prusa(); }, m_timer.GetId());
    SetSizer(layout);
    CentreOnParent();
    refresh();
}

BMCUDialog::~BMCUDialog()
{
    m_alive->store(false);
    m_timer.Stop();
}

void BMCUDialog::refresh()
{
    m_rows->Clear(true);
    m_scroll->FitInside();
    if (m_transport == "prusaconnect") {
        std::vector<BMCU::Slot> slots;
        for (int source = 0; source < m_modules * 4; ++source) {
            const auto module = source / 4;
            slots.push_back(BMCU::Slot { std::string(1, char('A' + module)), "", "#FFFFFF", "Not verified via Connect",
                source % 4, source, false, std::string(1, char('A' + module)) + std::to_string(source % 4 + 1) });
        }
        show_slots(slots);
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
        const bool editable = slot.ready || m_transport == "prusaconnect";
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

        auto *state = new wxStaticText(m_scroll, wxID_ANY, slot.ready || m_transport == "prusaconnect" ? wxString::FromUTF8(slot.state.c_str()) : "Offline");
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
    m_status->SetLabel(m_transport == "prusaconnect" ? wxString::Format("%zu configured slots, cloud discovery unavailable", slots.size()) : wxString::Format("%zu slots, %zu online", slots.size(), connected));
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
        auto changed = slot;
        changed.material = material;
        changed.color = color;
        if (material.empty()) {
            m_status->SetLabel("Select a filament material first");
            return;
        }
        m_pending[slot.tool] = std::move(changed);
        m_timer.StartOnce(900);
        m_status->SetLabel("BMCU configuration queued for Prusa upload");
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

void BMCUDialog::flush_prusa()
{
    if (m_sending || m_pending.empty()) return;
    if (!m_print_host) {
        m_status->SetLabel("Select a PrusaLink / Prusa Connect physical printer");
        return;
    }
    std::vector<BMCU::Slot> updates;
    updates.reserve(m_pending.size());
    for (const auto &[source, slot] : m_pending) updates.push_back(slot);
    m_pending.clear();
    m_sending = true;
    m_status->SetLabel("Uploading BMCU configuration G-code...");
    auto guard = m_alive;
    BMCU::send_prusa(*m_print_host, updates, [this, guard](std::string error) {
        wxTheApp->CallAfter([this, guard, error = std::move(error)] {
            if (!guard->load()) return;
            m_sending = false;
            m_status->SetLabel(error.empty() ? "Prusa upload accepted; waiting for printer execution" : wxString::FromUTF8(error.c_str()));
            if (!m_pending.empty()) m_timer.StartOnce(1500);
        });
    });
}

}
