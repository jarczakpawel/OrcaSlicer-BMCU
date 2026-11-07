#include "BMCUIntegration.hpp"
#include "GUI_App.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <map>
#include <utility>

namespace Slic3r::GUI::BMCU {

namespace {

constexpr std::array<const char *, 14> prusa_materials {"PLA", "PETG", "ASA", "PC", "PVB", "ABS", "HIPS", "PP", "FLEX", "PA", "PPS", "PPA", "PVA", "BVOH"};

bool valid_endpoint(const std::string &value)
{
    const size_t prefix = value.compare(0, 7, "http://") == 0 ? 7 :
        value.compare(0, 8, "https://") == 0 ? 8 : 0;
    return prefix != 0 && value.size() > prefix && value[prefix] != '/' &&
        value.find_first_of(" \r\n\t?#") == std::string::npos &&
        value.find('@') == std::string::npos;
}

std::string base_url(const std::string &endpoint)
{
    const auto end = endpoint.find_last_not_of('/');
    return end == std::string::npos ? std::string() : endpoint.substr(0, end + 1);
}

std::string upper(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

bool valid_word(const std::string &text)
{
    return !text.empty() && text.size() <= 32 && std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '+';
    });
}

bool valid_color(const std::string &text)
{
    return text.size() == 7 && text[0] == '#' && std::all_of(text.begin() + 1, text.end(), [](unsigned char c) {
        return std::isxdigit(c);
    });
}

}

void discover(const std::string &endpoint, const std::string &api_key, SlotsCallback callback)
{
    if (!valid_endpoint(endpoint)) {
        callback({}, "Invalid Moonraker URL");
        return;
    }
    auto result = std::make_shared<bool>(false);
    auto http = Http::get(base_url(endpoint) + "/printer/objects/query?bmcu");
    http.timeout_connect(4).timeout_max(10).tls_verify(true);
    if (!api_key.empty()) http.header("X-Api-Key", api_key);
    http.on_complete([callback, result](std::string body, unsigned) {
        *result = true;
        try {
            const auto document = nlohmann::json::parse(body);
            const auto &devices = document.at("result").at("status").at("bmcu").at("devices");
            if (!devices.is_array()) throw std::runtime_error("Invalid BMCU device list");
            std::vector<Slot> slots;
            for (size_t index = 0; index < devices.size() && index < 4; ++index) {
                const auto &device = devices.at(index);
                if (!device.is_object() || !device.contains("channels") || !device.at("channels").is_array()) continue;
                const auto name = device.value("name", std::string());
                int module = static_cast<int>(index);
                if (name.size() == 5 && name.compare(0, 4, "bmcu") == 0 && name[4] >= '0' && name[4] <= '3')
                    module = name[4] - '0';
                const bool ready = device.value("ready", false) && device.value("connected", false);
                for (size_t channel = 0; channel < device.at("channels").size() && channel < 4; ++channel) {
                    const auto &value = device.at("channels").at(channel);
                    if (!value.is_object()) continue;
                    const auto material = value.value("material", std::string("PLA"));
                    const auto color = upper(value.value("color", std::string("#FFFFFF")));
                    slots.push_back(Slot {
                        name, material, color, value.value("state", std::string()), static_cast<int>(channel),
                        value.value("virtual_tool", -1), ready,
                        std::string(1, static_cast<char>('A' + module)) + std::to_string(channel + 1)
                    });
                }
            }
            callback(std::move(slots), {});
        } catch (const std::exception &error) {
            callback({}, error.what());
        }
    });
    http.on_error([callback, result](std::string, std::string error, unsigned code) {
        if (!*result) callback({}, error.empty() ? "Moonraker HTTP " + std::to_string(code) : error);
    });
    http.perform();
}

void set_filament(const std::string &endpoint, const std::string &api_key, const Slot &slot, const std::string &material, const std::string &color, UpdateCallback callback)
{
    const auto type = upper(material);
    const auto hex = upper(color);
    if (!valid_endpoint(endpoint) || !valid_word(slot.device) || slot.channel < 0 || slot.channel > 3 || !valid_word(type) || !valid_color(hex)) {
        callback("Invalid BMCU device, material or color");
        return;
    }
    const std::string command = "BMCU_SET_FILAMENT DEVICE=" + slot.device + " CHANNEL=" + std::to_string(slot.channel)
        + " MATERIAL=" + type + " COLOR=" + hex.substr(1);
    const auto body = nlohmann::json { { "script", command } }.dump();
    auto done = std::make_shared<bool>(false);
    auto http = Http::post(base_url(endpoint) + "/printer/gcode/script");
    http.timeout_connect(4).timeout_max(10).tls_verify(true).header("Content-Type", "application/json").set_post_body(body);
    if (!api_key.empty()) http.header("X-Api-Key", api_key);
    http.on_complete([callback, done](std::string, unsigned) {
        *done = true;
        if (callback) callback({});
    });
    http.on_error([callback, done](std::string, std::string error, unsigned code) {
        if (!*done && callback) callback(error.empty() ? "Moonraker HTTP " + std::to_string(code) : error);
    });
    http.perform();
}

void discover_panel(const std::string &endpoint, const std::string &api_key, UpdateCallback callback)
{
    if (!valid_endpoint(endpoint)) {
        callback({});
        return;
    }
    const auto scheme_end = endpoint.find("://") + 3;
    const auto authority_end = endpoint.find('/', scheme_end);
    const auto authority = endpoint.substr(scheme_end, authority_end == std::string::npos ? std::string::npos : authority_end - scheme_end);
    const auto name_end = authority.front() == '[' ? authority.find(']') + 1 : authority.find(':');
    const auto host = authority.substr(0, name_end);
    if (host.empty() || host == "[" || host.find('@') != std::string::npos) {
        callback({});
        return;
    }
    auto http = Http::get(base_url(endpoint) + "/printer/objects/query?bmcu_panel");
    http.timeout_connect(4).timeout_max(8).tls_verify(true);
    if (!api_key.empty()) http.header("X-Api-Key", api_key);
    auto handled = std::make_shared<bool>(false);
    http.on_complete([callback, host, handled](std::string body, unsigned) {
        *handled = true;
        try {
            const auto panel = nlohmann::json::parse(body).at("result").at("status").at("bmcu_panel");
            const int port = panel.value("port", 0);
            if (!panel.value("enabled", false) || port < 1024 || port > 65535) {
                callback({});
                return;
            }
            callback("http://" + host + ":" + std::to_string(port) + "/");
        } catch (const std::exception &) {
            callback({});
        }
    });
    http.on_error([callback, handled](std::string, std::string, unsigned) {
        if (!*handled) callback({});
    });
    http.perform();
}

void update_prusalink(const std::string &endpoint, const std::string &api_key, const Slot &slot, const std::string &material, const std::string &color, UpdateCallback callback)
{
    auto selected = upper(material);
    if (selected == "TPU") selected = "FLEX";
    const auto type = std::find_if(prusa_materials.begin(), prusa_materials.end(), [&selected](const char *candidate) { return selected == candidate; });
    if (!valid_endpoint(endpoint) || slot.tool < 0 || slot.tool >= 16 || !valid_color(color) || type == prusa_materials.end()) {
        callback("Invalid PrusaLink slot, color or material");
        return;
    }
    const unsigned red = std::stoul(color.substr(1, 2), nullptr, 16);
    const unsigned green = std::stoul(color.substr(3, 2), nullptr, 16);
    const unsigned blue = std::stoul(color.substr(5, 2), nullptr, 16);
    const auto path = "/api/v1/bmcu/set/" + std::to_string(slot.tool) + "/" + std::to_string(red) + "/"
        + std::to_string(green) + "/" + std::to_string(blue) + "/" + std::to_string(type - prusa_materials.begin());
    auto completed = std::make_shared<bool>(false);
    auto http = Http::post(base_url(endpoint) + path);
    http.timeout_connect(4).timeout_max(10).tls_verify(true).set_post_body(std::string {});
    if (!api_key.empty()) http.header("X-Api-Key", api_key);
    http.on_complete([callback, completed](std::string, unsigned) {
        *completed = true;
        if (callback) callback({});
    });
    http.on_error([callback, completed](std::string, std::string error, unsigned code) {
        if (!*completed && callback) callback(error.empty() ? "PrusaLink HTTP " + std::to_string(code) : error);
    });
    http.perform();
}

void select_prusalink(const std::string &endpoint, const std::string &api_key, int source, UpdateCallback callback)
{
    if (!valid_endpoint(endpoint) || source < 0 || source > 16) {
        callback("Invalid PrusaLink source");
        return;
    }
    auto completed = std::make_shared<bool>(false);
    auto http = Http::post(base_url(endpoint) + "/api/v1/bmcu/select/" + std::to_string(source));
    http.timeout_connect(4).timeout_max(10).tls_verify(true).set_post_body(std::string {});
    if (!api_key.empty()) http.header("X-Api-Key", api_key);
    http.on_complete([callback, completed](std::string, unsigned) {
        *completed = true;
        if (callback) callback({});
    });
    http.on_error([callback, completed](std::string, std::string error, unsigned code) {
        if (!*completed && callback) callback(error.empty() ? "PrusaLink HTTP " + std::to_string(code) : error);
    });
    http.perform();
}

void discover_prusalink(const std::string &endpoint, const std::string &api_key, SlotsCallback callback)
{
    if (!valid_endpoint(endpoint)) {
        callback({}, "Invalid PrusaLink URL");
        return;
    }
    auto http = Http::get(base_url(endpoint) + "/api/v1/bmcu");
    http.timeout_connect(4).timeout_max(10).tls_verify(true);
    if (!api_key.empty()) http.header("X-Api-Key", api_key);
    http.on_complete([callback](std::string body, unsigned) {
        try {
            const auto document = nlohmann::json::parse(body);
            const auto &modules = document.at("modules");
            if (!modules.is_array() || document.value("schema", 0) != 1) throw std::runtime_error("Invalid BMCU API response");
            const int active_source = document.value("active_source", -1);
            const bool reload_pending = document.value("reload_pending", false);
            std::vector<Slot> slots;
            for (const auto &module : modules) {
                if (!module.is_object() || !module.value("online", false)) continue;
                const int index = module.value("index", -1);
                if (index < 0 || index >= 4) continue;
                const auto &channels = module.at("channels");
                if (!channels.is_array()) continue;
                for (size_t channel = 0; channel < channels.size() && channel < 4; ++channel) {
                    const auto &value = channels.at(channel);
                    if (!value.is_object()) continue;
                    const int source = value.value("source", -1);
                    if (source != index * 4 + static_cast<int>(channel)) continue;
                    const int material = value.value("material", 255);
                    const auto type = material >= 0 && material < static_cast<int>(prusa_materials.size()) ? prusa_materials[material] : "Unknown";
                    const uint16_t rgb = static_cast<uint16_t>(value.value("color565", 0));
                    char color[8];
                    std::snprintf(color, sizeof(color), "#%02X%02X%02X",
                        (((rgb >> 11) & 31) * 255 + 15) / 31, (((rgb >> 5) & 63) * 255 + 31) / 63, ((rgb & 31) * 255 + 15) / 31);
                    const bool loaded = value.value("loaded", false);
                    const char *state = source == active_source ?
                        (reload_pending ? "Reload pending" : loaded ? "In use" : "Selected") :
                        loaded ? "Loaded" : value.value("present", false) ? "Present" : "Empty";
                    slots.push_back(Slot { std::string(1, char('A' + index)), type,
                        value.value("color_valid", false) ? color : "#FFFFFF", state,
                        static_cast<int>(channel), source, true,
                        std::string(1, char('A' + index)) + std::to_string(channel + 1) });
                }
            }
            callback(std::move(slots), {});
        } catch (const std::exception &error) {
            callback({}, error.what());
        }
    });
    http.on_error([callback](std::string, std::string error, unsigned code) {
        callback({}, error.empty() ? "PrusaLink HTTP " + std::to_string(code) : error);
    });
    http.perform();
}

namespace {
struct FilamentSync {
    std::string endpoint;
    std::string api_key;
    std::string material;
    std::string color;
    int tool = -1;
    unsigned revision = 0;
    bool prusalink = false;
};

std::mutex filament_sync_mutex;
std::map<std::string, FilamentSync> filament_sync_queue;

void dispatch_filament_sync(const std::string &key);

void complete_filament_sync(const std::string &key, unsigned revision, const std::string &error)
{
    if (!error.empty()) BOOST_LOG_TRIVIAL(error) << "BMCU filament synchronization: " << error;
    bool repeat = false;
    {
        std::lock_guard<std::mutex> lock(filament_sync_mutex);
        const auto it = filament_sync_queue.find(key);
        if (it == filament_sync_queue.end()) return;
        if (it->second.revision == revision) filament_sync_queue.erase(it);
        else repeat = true;
    }
    if (repeat) dispatch_filament_sync(key);
}

void dispatch_filament_sync(const std::string &key)
{
    FilamentSync current;
    {
        std::lock_guard<std::mutex> lock(filament_sync_mutex);
        const auto it = filament_sync_queue.find(key);
        if (it == filament_sync_queue.end()) return;
        current = it->second;
    }
    if (current.prusalink) {
        const int module = current.tool / 4;
        const Slot slot {std::string(1, char('A' + module)), current.material, current.color, {}, current.tool % 4,
            current.tool, true, std::string(1, char('A' + module)) + std::to_string(current.tool % 4 + 1)};
        update_prusalink(current.endpoint, current.api_key, slot, current.material, current.color,
            [key, revision = current.revision](std::string result) { complete_filament_sync(key, revision, result); });
        return;
    }
    discover(current.endpoint, current.api_key, [key, current](std::vector<Slot> slots, std::string error) {
        if (!error.empty()) {
            complete_filament_sync(key, current.revision, error);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(filament_sync_mutex);
            const auto it = filament_sync_queue.find(key);
            if (it == filament_sync_queue.end()) return;
            if (it->second.revision != current.revision) {
                error = "Superseded";
            }
        }
        if (error == "Superseded") {
            complete_filament_sync(key, current.revision, {});
            return;
        }
        for (const Slot &slot : slots) {
            if (slot.ready && slot.tool == current.tool) {
                set_filament(current.endpoint, current.api_key, slot,
                    current.material.empty() ? slot.material : current.material, current.color,
                    [key, revision = current.revision](std::string result) {
                        complete_filament_sync(key, revision, result);
                    });
                return;
            }
        }
        complete_filament_sync(key, current.revision, "Logical tool not assigned to a ready BMCU channel");
    });
}
}

void queue_filament_sync(const std::string &endpoint, const std::string &api_key, int tool,
    const std::string &color, const std::string &material, bool prusalink)
{
    if (!valid_endpoint(endpoint) || tool < 0 || !valid_color(color)) return;
    const auto key = endpoint + (prusalink ? "#prusalink#" : "#moonraker#") + std::to_string(tool);
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(filament_sync_mutex);
        const auto it = filament_sync_queue.find(key);
        if (it == filament_sync_queue.end()) {
            filament_sync_queue.emplace(key, FilamentSync {endpoint, api_key, material, color, tool, 1, prusalink});
            start = true;
        } else {
            it->second.api_key = api_key;
            it->second.material = material;
            it->second.color = color;
            ++it->second.revision;
        }
    }
    if (start) dispatch_filament_sync(key);
}

void sync_filament(const std::string &endpoint, const std::string &api_key, int tool, const std::string &color, const std::string &material)
{
    queue_filament_sync(endpoint, api_key, tool, color, material, false);
}


void sync_slicer_color(int tool)
{
    if (tool < 0) return;
    auto *bundle = wxGetApp().preset_bundle;
    const auto *colors = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (!colors || static_cast<size_t>(tool) >= colors->values.size() ||
        static_cast<size_t>(tool) >= bundle->filament_presets.size()) return;
    auto *filament = bundle->filaments.find_preset(bundle->filament_presets[tool]);
    if (!filament) return;
    std::string display;
    sync_from_slicer(tool, filament->get_filament_type(display), colors->values[tool]);
}

void sync_slicer_material_changes(const DynamicPrintConfig &previous, const DynamicPrintConfig &updated)
{
    const auto *before = previous.option<ConfigOptionStrings>("filament_type");
    const auto *after = updated.option<ConfigOptionStrings>("filament_type");
    const auto *colors = wxGetApp().preset_bundle->project_config.option<ConfigOptionStrings>("filament_colour");
    if (!before || !after || !colors) return;
    const size_t count = std::min({before->values.size(), after->values.size(), colors->values.size(), size_t(16)});
    for (size_t i = 0; i < count; ++i)
        if (before->values[i] != after->values[i])
            sync_from_slicer(static_cast<int>(i), after->values[i], colors->values[i]);
}

void sync_from_slicer(int tool, const std::string &material, const std::string &color)
{
    if (tool < 0 || tool >= 16 || !valid_color(color) || material.empty()) return;
    const auto &printer = wxGetApp().preset_bundle->printers.get_selected_preset().config;
    if (!printer.has("bmcu_enabled") || !printer.opt_bool("bmcu_enabled")) return;
    const auto transport = printer.opt_string("bmcu_transport");
    const auto *physical = wxGetApp().preset_bundle->physical_printers.get_selected_printer_config();
    if ((transport == "prusalink" || transport == "prusaconnect") && !physical) return;
    if (transport == "prusaconnect" || transport == "prusalink") {
        const auto *type = physical->option<ConfigOptionEnum<PrintHostType>>("host_type");
        if (!type || (transport == "prusalink" && type->value != htPrusaLink) ||
            (transport == "prusaconnect" && type->value != htPrusaConnect)) return;
    }
    if (transport != "moonraker" && tool >= printer.opt_int("bmcu_modules") * 4) return;
    const int module = tool / 4;
    Slot slot {std::string(1, char('A' + module)), material, color, {}, tool % 4, tool, true,
        std::string(1, char('A' + module)) + std::to_string(tool % 4 + 1)};
    if (transport == "prusaconnect") return;
    auto endpoint = printer.opt_string("bmcu_endpoint");
    if (endpoint.empty()) endpoint = physical ? physical->opt_string("print_host") : printer.opt_string("print_host");
    if (endpoint.empty()) return;
    if (endpoint.find("://") == std::string::npos) endpoint = "http://" + endpoint;
    const auto api_key = physical ? physical->opt_string("printhost_apikey") : std::string {};
    if (transport == "prusalink") queue_filament_sync(endpoint, api_key, tool, color, material, true);
    else if (transport == "moonraker") sync_filament(endpoint, api_key, tool, color, material);
}

}
