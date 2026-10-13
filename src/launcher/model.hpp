#pragma once
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace rgds {
namespace fs = std::filesystem;
struct Game { std::string title; fs::path path; bool demo = false; };
inline constexpr const char* core_ids[] = {"drastic", "dsperate", "nnddss", "melonds", "rgds-experimental"};
inline constexpr const char* core_names[] = {"DraStic", "DSperate", "NNDDSS", "melonDS", "RG DS Experimental"};
inline constexpr int core_count = sizeof(core_ids)/sizeof(core_ids[0]);
inline std::string lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
inline std::string library_title(std::string name) {
    // Remove recognized dump tags, retaining meaningful parenthetical titles.
    for(size_t at=0;(at=name.find('(',at))!=std::string::npos;){
        auto end=name.find(')',at);if(end==std::string::npos)break;
        auto tag=lower(name.substr(at+1,end-at-1));
        bool language=!tag.empty();for(size_t i=0;i<tag.size();i++)if(i%3==2?tag[i]!=',':tag[i]<'a' || tag[i]>'z')language=false;
        language=language && tag.size()%3==2;
        const std::vector<std::string> regions={"usa","europe","japan","world","australia","canada","france","germany","italy","spain","korea","china","taiwan","norway","denmark","sweden","netherlands","united kingdom","scandinavia","russia","ndsi enhanced"};
        if(language || tag.rfind("rev ",0)==0 || std::find(regions.begin(),regions.end(),tag)!=regions.end())name.erase(at,end-at+1);else at=end+1;
    }
    if(name.size()>5 && std::all_of(name.begin(),name.begin()+4,[](char c){return c>='0' && c<='9';})){
        auto start=name.find_first_not_of(' ',4);if(start!=std::string::npos && (name[start]=='-' || name[start]=='.'))name.erase(0,start+1);
    }
    std::string clean;bool space=false;for(char c:name){if(c==' ' || c=='\t'){space=!clean.empty();continue;}if(space)clean+=' ';clean+=c;space=false;}
    if(clean.size()>5 && clean.compare(clean.size()-5,5,", The")==0)clean="The "+clean.substr(0,clean.size()-5);
    return clean.empty()?name:clean;
}
inline std::vector<Game> scan(const std::vector<fs::path>& roots) {
    std::map<std::string, Game> unique;
    for (const auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        while (it != end) {
            auto entry = *it;
            if (entry.is_symlink(ec)) { it.disable_recursion_pending(); }
            else if (entry.is_regular_file(ec)) {
                const auto ext = lower(entry.path().extension().string());
                if (ext == ".nds" || ext == ".dsi" || ext == ".zip") {
                    auto canonical = fs::weakly_canonical(entry.path(), ec);
                    if (!ec) unique[canonical.string()] = {entry.path().stem().string(), canonical, false};
                }
            }
            ec.clear(); it.increment(ec);
            if (ec) break;
        }
    }
    std::vector<Game> games;
    for (const auto& pair : unique) games.push_back(pair.second);
    std::sort(games.begin(), games.end(), [](const Game& a, const Game& b) {
        auto at = lower(a.title), bt = lower(b.title);
        return at == bt ? a.path.string() < b.path.string() : at < bt;
    });
    return games;
}
struct Model {
    std::vector<Game> games;
    size_t selected = 0;
    unsigned core = 0;
    void move(int delta) {
        if (games.empty()) return;
        auto n = static_cast<long>(games.size());
        selected = static_cast<size_t>((static_cast<long>(selected) + delta % n + n) % n);
    }
    void next_core() { core = (core + 1) % core_count; }
};
struct Color { unsigned char r, g, b; };
using Theme = std::map<std::string, Color>;
inline Theme default_theme() {
    return {{"background", {242,245,247}}, {"panel",{255,255,255}}, {"ink",{45,57,68}},
        {"muted",{108,124,138}}, {"accent",{44,137,218}}, {"line",{191,204,215}}, {"slot",{221,230,237}},
        {"on_accent",{255,255,255}}, {"focus",{224,240,252}}};
}
inline Theme default_dark_theme() {
    return {{"background",{12,17,26}}, {"panel",{23,32,46}}, {"ink",{233,244,255}},
        {"muted",{145,167,190}}, {"accent",{78,187,255}}, {"line",{45,67,90}}, {"slot",{17,26,39}},
        {"on_accent",{7,21,36}}, {"focus",{26,61,89}}};
}
inline Theme load_theme(const fs::path& path,Theme t = default_theme()) {
    if (path.empty()) return t;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot read theme: " + path.string());
    std::string line; bool schema = false;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto p = line.find('=');
        if (p == std::string::npos) throw std::runtime_error("Theme line lacks =");
        const auto key = line.substr(0,p), value = line.substr(p+1);
        if (key == "schema") { if (value != "1") throw std::runtime_error("Unsupported theme schema"); schema = true; continue; }
        if (key == "mode") { if(value!="light" && value!="dark")throw std::runtime_error("Invalid colour mode");continue; }
        if (!t.count(key) || value.size() != 6 || value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
            throw std::runtime_error("Invalid theme field: " + key);
        const auto rgb = std::stoul(value, nullptr, 16);
        t[key] = {static_cast<unsigned char>(rgb >> 16), static_cast<unsigned char>(rgb >> 8), static_cast<unsigned char>(rgb)};
    }
    if (!schema) throw std::runtime_error("Theme schema is missing");
    return t;
}
}
