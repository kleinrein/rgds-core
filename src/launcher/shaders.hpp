#pragma once
#include "launcher/model.hpp"
#include <sstream>
#include "platform/shader_catalog.hpp"
namespace rgds {
struct Shader {
    std::string name, file, description;
    int asset_id = 0;
};
inline std::vector<Shader> load_shaders(const fs::path& path) {
    std::ifstream source(path);
    if (!source) throw std::runtime_error("Shader catalog missing");
    std::vector<Shader> result;
    std::string line;
    while (std::getline(source, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream row(line);
        std::string id, kind;
        Shader shader;
        if (!std::getline(row, id, '\t') || !std::getline(row, shader.name, '\t') ||
            !std::getline(row, shader.file, '\t') || !std::getline(row, kind, '\t') ||
            !std::getline(row, shader.description))
            throw std::runtime_error("Invalid shader catalog");
        if (std::stoi(id) != int(result.size())) throw std::runtime_error("Invalid shader order");
        if (result.size() >= shader_count || shader.file != shader_files[result.size()])
            throw std::runtime_error("Shader catalog/pipeline mismatch");
        shader.asset_id = std::stoi(kind);
        result.push_back(std::move(shader));
    }
    if (result.size() != shader_count) throw std::runtime_error("Incomplete shader catalog");
    return result;
}
inline bool shader_supported(int shader, int core) {
    return core >= 0 && core < core_count && shader >= 0 && shader < shader_count;
}
inline bool filter_field(const std::string& line, int& value) {
    if (line.rfind("filter=", 0) != 0) return false;
    try {
        size_t used = 0;
        int parsed = std::stoi(line.substr(7), &used);
        if (used != line.size() - 7 || parsed < 0 || parsed >= shader_count) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}
}
