#include "BMCUIntegration.hpp"
#include "GUI_App.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cctype>
#include <memory>
#include <boost/filesystem.hpp>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>
#include <stdexcept>
#include <chrono>
#include <map>
#include <iterator>
#include <utility>

namespace Slic3r::GUI::BMCU {

namespace {

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
    constexpr const char *types[] = { "PLA", "PETG", "ASA", "PC", "PVB", "ABS", "HIPS", "PP", "FLEX", "PA", "PPS", "PPA", "PVA", "BVOH" };
    auto selected = upper(material);
    if (selected == "TPU") selected = "FLEX";
    const auto type = std::find_if(std::begin(types), std::end(types), [&selected](const char *candidate) { return selected == candidate; });
    if (!valid_endpoint(endpoint) || slot.tool < 0 || slot.tool >= 16 || !valid_color(color) || type == std::end(types)) {
        callback("Invalid PrusaLink slot, color or material");
        return;
    }
    const unsigned red = std::stoul(color.substr(1, 2), nullptr, 16);
    const unsigned green = std::stoul(color.substr(3, 2), nullptr, 16);
    const unsigned blue = std::stoul(color.substr(5, 2), nullptr, 16);
    const auto path = "/api/v1/bmcu/set/" + std::to_string(slot.tool) + "/" + std::to_string(red) + "/"
        + std::to_string(green) + "/" + std::to_string(blue) + "/" + std::to_string(type - std::begin(types));
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
            constexpr const char *material_names[] = { "PLA", "PETG", "ASA", "PC", "PVB", "ABS", "HIPS", "PP", "FLEX", "PA", "PPS", "PPA", "PVA", "BVOH" };
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
                    const auto type = material >= 0 && material < static_cast<int>(sizeof(material_names) / sizeof(material_names[0])) ? material_names[material] : "Unknown";
                    const uint16_t rgb = static_cast<uint16_t>(value.value("color565", 0));
                    char color[8];
                    std::snprintf(color, sizeof(color), "#%02X%02X%02X",
                        (((rgb >> 11) & 31) * 255 + 15) / 31, (((rgb >> 5) & 63) * 255 + 31) / 63, ((rgb & 31) * 255 + 15) / 31);
                    slots.push_back(Slot { std::string(1, char('A' + index)), type,
                        value.value("color_valid", false) ? color : "#FFFFFF",
                        value.value("loaded", false) ? "Loaded" : value.value("present", false) ? "Present" : "Empty",
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

void send_prusa(const DynamicPrintConfig &config, const std::vector<Slot> &slots, UpdateCallback callback)
{
    if (slots.empty()) {
        callback("No slots selected");
        return;
    }
    const auto host_type = config.option<ConfigOptionEnum<PrintHostType>>("host_type");
    if (!host_type || (host_type->value != htPrusaLink && host_type->value != htPrusaConnect)) {
        callback("Select a PrusaLink or Prusa Connect physical printer first");
        return;
    }
    std::string gcode = "; OrcaSlicer-BMCU metadata synchronization\n";
    constexpr const char *materials[] = { "PLA", "PETG", "ASA", "PC", "PVB", "ABS", "HIPS", "PP", "FLEX", "PA", "PVA", "BVOH" };
    for (const Slot &slot : slots) {
        if (slot.tool < 0 || slot.tool >= 16 || !valid_color(slot.color)) {
            callback("Invalid Prusa BMCU slot or color");
            return;
        }
        auto material = upper(slot.material);
        if (material == "TPU") material = "FLEX";
        if (std::find_if(std::begin(materials), std::end(materials), [&material](const char *value) { return material == value; }) == std::end(materials)) {
            callback("Unsupported Prusa filament material: " + material);
            return;
        }
        unsigned red = 0, green = 0, blue = 0;
        if (std::sscanf(slot.color.c_str(), "#%02X%02X%02X", &red, &green, &blue) != 3 || red > 255 || green > 255 || blue > 255) {
            callback("Invalid color");
            return;
        }
        gcode += "M8735 T" + std::to_string(slot.tool) + " R" + std::to_string(red) + " G" + std::to_string(green)
            + " B" + std::to_string(blue) + " S\"" + material + "\"\n";
    }
    const bool via_connect = host_type->value == htPrusaConnect;
    auto saved = std::make_shared<DynamicPrintConfig>(config);
    std::thread([saved, via_connect, gcode = std::move(gcode), callback = std::move(callback)] {
        boost::filesystem::path filename;
        std::string error;
        try {
            filename = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("bmcu-sync-%%%%%%%%.gcode");
            {
                std::ofstream file(filename.string(), std::ios::binary | std::ios::trunc);
                if (!file || !(file << gcode)) throw std::runtime_error("Cannot write BMCU G-code file");
            }
            std::unique_ptr<PrintHost> host(PrintHost::get_print_host(saved.get()));
            if (!host) throw std::runtime_error("Prusa print host is unavailable");
            PrintHostUpload upload;
            upload.source_path = filename;
            upload.upload_path = via_connect ? filename.filename() : boost::filesystem::path("BMCU_SYNC.gcode");
            upload.post_action = PrintHostPostUploadAction::StartPrint;
            if (!host->upload(std::move(upload), [](Http::Progress, bool &) {},
                    [&error](wxString message) { error = message.ToStdString(); },
                    [](wxString, wxString) {})) {
                if (error.empty()) error = "Prusa Connect/Link upload failed";
            }
        } catch (const std::exception &exception) {
            error = exception.what();
        }
        if (!filename.empty()) {
            boost::system::error_code ignored;
            boost::filesystem::remove(filename, ignored);
        }
        callback(std::move(error));
    }).detach();
}

namespace {
struct PrusaQueue {
    std::mutex mutex;
    std::map<int, Slot> pending;
    std::shared_ptr<DynamicPrintConfig> config;
    bool active = false;
};

PrusaQueue &prusa_queue()
{
    static PrusaQueue queue;
    return queue;
}

void flush_prusa_queue()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(900));
    auto &queue = prusa_queue();
    std::vector<Slot> updates;
    std::shared_ptr<DynamicPrintConfig> config;
    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        for (const auto &[source, slot] : queue.pending) updates.push_back(slot);
        queue.pending.clear();
        config = queue.config;
    }
    send_prusa(*config, updates, [](std::string error) {
        if (!error.empty()) BOOST_LOG_TRIVIAL(error) << "BMCU Prusa synchronization failed: " << error;
        auto &queue = prusa_queue();
        bool repeat = false;
        {
            std::lock_guard<std::mutex> lock(queue.mutex);
            repeat = !queue.pending.empty();
            if (!repeat) queue.active = false;
        }
        if (repeat) std::thread(flush_prusa_queue).detach();
    });
}
}

void queue_prusa(const DynamicPrintConfig &config, const Slot &slot)
{
    if (slot.tool < 0 || slot.tool >= 16 || !valid_color(slot.color)) return;
    auto &queue = prusa_queue();
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(queue.mutex);
        queue.pending[slot.tool] = slot;
        queue.config = std::make_shared<DynamicPrintConfig>(config);
        if (!queue.active) {
            queue.active = true;
            start = true;
        }
    }
    if (start) std::thread(flush_prusa_queue).detach();
}

void sync_filament(const std::string &endpoint, const std::string &api_key, int tool, const std::string &color, const std::string &material)
{
    if (tool < 0 || !valid_color(color)) return;
    discover(endpoint, api_key, [endpoint, api_key, tool, color, material](std::vector<Slot> slots, std::string error) {
        if (!error.empty()) return;
        for (const Slot &slot : slots) {
            if (slot.ready && slot.tool == tool) {
                set_filament(endpoint, api_key, slot, material.empty() ? slot.material : material, color, {});
                return;
            }
        }
    });
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
    if (transport == "prusaconnect") {
        queue_prusa(*physical, slot);
        return;
    }
    auto endpoint = printer.opt_string("bmcu_endpoint");
    if (endpoint.empty()) endpoint = physical ? physical->opt_string("print_host") : printer.opt_string("print_host");
    if (endpoint.empty()) return;
    if (endpoint.find("://") == std::string::npos) endpoint = "http://" + endpoint;
    const auto api_key = physical ? physical->opt_string("printhost_apikey") : std::string {};
    if (transport == "prusalink") update_prusalink(endpoint, api_key, slot, material, color, {});
    else if (transport == "moonraker") sync_filament(endpoint, api_key, tool, color, material);
}

}
