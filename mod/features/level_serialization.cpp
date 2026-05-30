#include "level_serialization.h"
#include "../utils/logger.h"

#include <nlohmann/json.hpp>
#include <fstream>

using json = nlohmann::json;

namespace LevelIO {

static constexpr const char* FORMAT_TAG = "blacklist-level";

bool Save(const std::string& path, const LevelFile& lvl, std::string& err) {
    try {
        json root;
        root["format"]  = FORMAT_TAG;
        root["version"] = lvl.version;
        root["map"]     = lvl.map;

        json arr = json::array();
        for (const auto& p : lvl.props) {
            json j;
            j["id"]        = p.id;
            j["className"] = p.className;
            j["position"]  = { {"x", p.position.x}, {"y", p.position.y}, {"z", p.position.z} };
            j["rotation"]  = { {"pitch", p.rotation.pitch}, {"yaw", p.rotation.yaw}, {"roll", p.rotation.roll} };
            j["scale"]     = { {"x", p.scale.x}, {"y", p.scale.y}, {"z", p.scale.z} };
            arr.push_back(std::move(j));
        }
        root["props"] = std::move(arr);

        std::ofstream out(path, std::ios::trunc);
        if (!out) { err = "cannot open file for writing: " + path; return false; }
        out << root.dump(2);
        Logger::Info("LevelIO: saved %zu props to %s", lvl.props.size(), path.c_str());
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

bool Load(const std::string& path, LevelFile& out, std::string& err) {
    try {
        std::ifstream in(path);
        if (!in) { err = "cannot open file: " + path; return false; }

        json root;
        in >> root;

        if (root.value("format", "") != FORMAT_TAG) {
            err = "not a Blacklist level file";
            return false;
        }

        out = LevelFile{};
        out.version = root.value("version", 1);
        out.map     = root.value("map", "");

        for (const auto& j : root.value("props", json::array())) {
            SavedProp p;
            p.id        = j.value("id", 0u);
            p.className = j.value("className", "");
            if (auto it = j.find("position"); it != j.end()) {
                p.position.x = it->value("x", 0.f);
                p.position.y = it->value("y", 0.f);
                p.position.z = it->value("z", 0.f);
            }
            if (auto it = j.find("rotation"); it != j.end()) {
                p.rotation.pitch = it->value("pitch", 0.f);
                p.rotation.yaw   = it->value("yaw", 0.f);
                p.rotation.roll  = it->value("roll", 0.f);
            }
            if (auto it = j.find("scale"); it != j.end()) {
                p.scale.x = it->value("x", 1.f);
                p.scale.y = it->value("y", 1.f);
                p.scale.z = it->value("z", 1.f);
            }
            out.props.push_back(std::move(p));
        }

        Logger::Info("LevelIO: loaded %zu props from %s", out.props.size(), path.c_str());
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

} // namespace LevelIO
