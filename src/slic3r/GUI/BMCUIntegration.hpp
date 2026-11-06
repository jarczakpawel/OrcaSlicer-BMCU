#ifndef slic3r_BMCUIntegration_hpp_
#define slic3r_BMCUIntegration_hpp_

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
class DynamicPrintConfig;
}

namespace Slic3r::GUI::BMCU {

struct Slot {
    std::string device;
    std::string material;
    std::string color;
    std::string state;
    int channel = -1;
    int tool = -1;
    bool ready = false;
    std::string label;
};

using SlotsCallback = std::function<void(std::vector<Slot>, std::string)>;
using UpdateCallback = std::function<void(std::string)>;

void discover(const std::string &endpoint, const std::string &api_key, SlotsCallback callback);
void set_filament(const std::string &endpoint, const std::string &api_key, const Slot &slot, const std::string &material, const std::string &color, UpdateCallback callback);
void discover_panel(const std::string &endpoint, const std::string &api_key, UpdateCallback callback);
void update_prusalink(const std::string &endpoint, const std::string &api_key, const Slot &slot, const std::string &material, const std::string &color, UpdateCallback callback);
void discover_prusalink(const std::string &endpoint, const std::string &api_key, SlotsCallback callback);
void send_prusa(const DynamicPrintConfig &config, const std::vector<Slot> &slots, UpdateCallback callback);
void queue_prusa(const DynamicPrintConfig &config, const Slot &slot);
void sync_filament(const std::string &endpoint, const std::string &api_key, int tool, const std::string &color, const std::string &material = {});
void sync_from_slicer(int tool, const std::string &material, const std::string &color);

}

#endif
