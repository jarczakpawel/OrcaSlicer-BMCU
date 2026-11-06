#ifndef slic3r_BMCUDialog_hpp_
#define slic3r_BMCUDialog_hpp_

#include "BMCUIntegration.hpp"

#include <wx/dialog.h>
#include <atomic>
#include <memory>
#include <map>
#include <wx/timer.h>
#include <string>
#include <vector>

class wxBoxSizer;
class wxButton;
class wxScrolledWindow;
class wxStaticText;

namespace Slic3r { class DynamicPrintConfig; }

namespace Slic3r::GUI {

class ConfigOptionsGroup;
void append_bmcu_settings(ConfigOptionsGroup &group, const DynamicPrintConfig &config);

class BMCUDialog final : public wxDialog {
public:
    BMCUDialog(wxWindow *parent, const std::string &transport, const std::string &endpoint, const std::string &api_key,
        int modules, const DynamicPrintConfig *print_host);
    ~BMCUDialog() override;

private:
    void refresh();
    void open_full_panel();
    void show_slots(const std::vector<BMCU::Slot> &slots);
    void update(const BMCU::Slot &slot, const std::string &material, const std::string &color);
    void flush_prusa();

    std::string m_transport;
    std::string m_endpoint;
    std::string m_api_key;
    std::shared_ptr<std::atomic_bool> m_alive;
    std::unique_ptr<DynamicPrintConfig> m_print_host;
    std::map<int, BMCU::Slot> m_pending;
    wxTimer m_timer;
    int m_modules;
    bool m_sending = false;
    wxScrolledWindow *m_scroll;
    wxBoxSizer *m_rows;
    wxStaticText *m_status;
    wxButton *m_refresh;
};

}

#endif
