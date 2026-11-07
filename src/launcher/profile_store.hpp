#pragma once
#include "launcher/profile_pin.hpp"
#include "launcher/profile_visibility.hpp"
#include "launcher/profiles.hpp"
#include "launcher/settings.hpp"
#include <algorithm>
#include <fcntl.h>
#include <map>
#include <sstream>
#include <unistd.h>
#include <vector>

namespace rgds {
// Who can play: the owner (userdata itself) and each profile's folder (runtime/profiles.py says why).
//     profiles/index.ini     ask at boot, and the owner's name and picture, which have no folder of their own
//     profiles/active        the current profile's id
//     profiles/<id>/profile.ini    name, picture, restricted (a Kid profile)
// A profile's colour is the accent in its own ui.ini, so the Theme page and the tile can never disagree.
inline constexpr size_t profile_limit = 6;

struct Profile {
    std::string id, name, ds_name, last_game, last_system; // last-game.txt: title, path, system (older files: title)
    fs::path last_path;
    fs::path root;
    int picture = 0, colour = -1;
    bool restricted = false;
    bool all_games = false; // Games opens every game in one list, not the Systems step (its ui.ini)
    std::string language;   // its menus' language (its ui.ini); empty until read
    // Who sees what (profile_visibility.hpp). A Kid profile hides what has no rating and starts at seven.
    int age = no_age_limit;
    bool unrated_hidden = false, allow_ports = false, allow_homebrew = false;
    // What a Kid profile may also use. Off unless the owner says so.
    bool allow_pictochat = false, allow_cheats = false, allow_wireless = false;
    std::string pin; // salt:digest (profile_pin.hpp), empty for none
    bool owner() const { return id == "owner"; }
};

inline std::map<std::string, std::string> profile_fields(const fs::path& path) {
    std::map<std::string, std::string> values;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto at = line.find('=');
        if (at != std::string::npos) values[line.substr(0, at)] = line.substr(at + 1);
    }
    return values;
}

// On the card, and through a power cut: which saves a game writes to depends on these files.
inline bool profile_write(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const auto pending = path.string() + ".pending";
    const int fd = ::open(pending.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    bool ok = ::write(fd, text.data(), text.size()) == ssize_t(text.size());
    ok = ::fsync(fd) == 0 && ok;
    ok = ::close(fd) == 0 && ok;
    if (ok) ok = ::rename(pending.c_str(), path.c_str()) == 0;
    else ::unlink(pending.c_str());
    if (ok) {
        const int folder = ::open(path.parent_path().c_str(), O_RDONLY);
        if (folder >= 0) {
            ::fsync(folder); // exFAT may refuse; the rename stands
            ::close(folder);
        }
    }
    return ok;
}

inline bool profile_write_fields(const fs::path& path, const std::map<std::string, std::string>& values) {
    std::string text;
    for (const auto& [key, value] : values) text += key + "=" + value + "\n";
    return profile_write(path, text);
}

// A name on one line, as the files hold it.
inline std::string profile_clean_name(std::string name) {
    for (char& c : name)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    while (!name.empty() && name.back() == ' ') name.pop_back();
    while (!name.empty() && name.front() == ' ') name.erase(0, 1);
    return name;
}

// What a DS keeps of a name: ten UTF-16 units, never half a character.
inline std::string profile_ds_name(const std::string& name) {
    std::string kept;
    size_t units = 0;
    for (size_t at = 0; at < name.size();) {
        const auto lead = static_cast<unsigned char>(name[at]);
        const size_t bytes = lead < 0x80 ? 1 : lead < 0xE0 ? 2 : lead < 0xF0 ? 3 : 4;
        units += bytes == 4 ? 2 : 1;
        if (units > 10) break;
        kept += name.substr(at, bytes);
        at += bytes;
    }
    return kept;
}

inline VisibilityRules visibility_rules(const Profile& profile) {
    VisibilityRules rules;
    rules.age = profile.age;
    rules.unrated_hidden = profile.unrated_hidden;
    rules.restricted = profile.restricted;
    rules.allow_ports = profile.allow_ports;
    rules.allow_homebrew = profile.allow_homebrew;
    rules.overrides = load_visibility(profile.root);
    return rules;
}

class ProfileStore {
    fs::path data;
    std::vector<Profile> list;
    std::string active_id = "owner";
    bool ask = true, unclear = false;

    Profile read_one(const std::string& id, const fs::path& root) const {
        Profile profile;
        profile.id = id;
        profile.root = root;
        Preferences look;
        try {
            load_ui_preferences(root / "ui.ini", look);
            profile.colour = look.accent;
            profile.all_games = look.show_systems && look.all_games;
            profile.language = look.language;
        } catch (const std::exception&) {}
        profile.ds_name = profile_fields(root / "ds.ini")["nickname"];
        std::ifstream last(root / "last-game.txt");
        std::string path;
        std::getline(last, profile.last_game);
        std::getline(last, path);
        std::getline(last, profile.last_system);
        profile.last_path = path;
        return profile;
    }
    std::map<std::string, std::string> index_fields() const {
        auto values = profile_fields(data / "profiles/index.ini");
        values["schema"] = "1";
        return values;
    }

public:
    // Everything from the card: a profile can change behind a cached copy only through this class, or a helper.
    void load(const fs::path& device) {
        data = device;
        list.clear();
        const auto folder = data / "profiles";
        const auto index = profile_fields(folder / "index.ini");
        const auto value = [&](const char* key) {
            const auto found = index.find(key);
            return found == index.end() ? std::string() : found->second;
        };
        ask = value("ask_at_boot") != "0";
        Profile owner = read_one("owner", data);
        owner.name = value("owner_name");
        if (owner.name.empty()) owner.name = owner.ds_name == "RG DS Core" ? "" : owner.ds_name;
        if (owner.name.empty()) owner.name = "Owner";
        owner.picture = std::atoi(value("owner_picture").c_str());
        owner.pin = value("owner_pin");
        list.push_back(std::move(owner));
        std::vector<std::string> ids;
        std::error_code ec;
        for (fs::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
            const auto id = it->path().filename().string();
            if (profile_id(id) && it->is_directory(ec)) ids.push_back(id);
        }
        std::sort(ids.begin(), ids.end(), [](const std::string& a, const std::string& b) {
            return a.size() == b.size() ? a < b : a.size() < b.size();
        });
        for (const auto& id : ids) {
            Profile profile = read_one(id, folder / id);
            auto values = profile_fields(profile.root / "profile.ini");
            profile.name = values["name"].empty() ? id : values["name"];
            profile.picture = std::atoi(values["picture"].c_str());
            profile.restricted = values["restricted"] == "1";
            if (!values["age"].empty()) profile.age = std::atoi(values["age"].c_str());
            profile.unrated_hidden = values["unrated"] == "hide";
            profile.allow_ports = values["ports"] == "1";
            profile.allow_homebrew = values["homebrew"] == "1";
            profile.allow_pictochat = values["pictochat"] == "1";
            profile.allow_cheats = values["cheats"] == "1";
            profile.allow_wireless = values["wireless"] == "1";
            profile.pin = values["pin"];
            list.push_back(std::move(profile));
        }
        bool broken = false;
        const auto root = player_root(data, &broken);
        unclear = broken;
        active_id = root == data ? "owner" : root.filename().string();
    }
    const std::vector<Profile>& profiles() const { return list; }
    bool enabled() const { return list.size() > 1; }
    bool ask_at_boot() const { return ask; }
    bool active_unclear() const { return unclear; }
    const std::string& active() const { return active_id; }
    int active_index() const {
        for (size_t i = 0; i < list.size(); i++)
            if (list[i].id == active_id) return int(i);
        return 0;
    }
    bool can_add() const { return list.size() < profile_limit; }
    // Removed but not erased: their saves wait in profiles/.removed.
    int removed_count() const {
        int count = 0;
        std::error_code ec;
        for (fs::directory_iterator it(data / "profiles/.removed", ec), end; !ec && it != end; it.increment(ec))
            count++;
        return count;
    }

    // Written before anything that depends on it: a card with an index but no active profile opens the picker.
    bool select(const std::string& id) {
        const auto found = std::find_if(list.begin(), list.end(), [&](const Profile& p) { return p.id == id; });
        if (found == list.end() || !profile_write(data / "profiles/active", id + "\n")) return false;
        active_id = id;
        unclear = false;
        return true;
    }

    bool set_ask_at_boot(bool value) {
        auto values = index_fields();
        values["ask_at_boot"] = value ? "1" : "0";
        if (!profile_write_fields(data / "profiles/index.ini", values)) return false;
        ask = value;
        return true;
    }

    // Never an id used before, removed profiles included: their saves wait under that name.
    std::string next_id() const {
        int highest = 0;
        const auto folder = data / "profiles";
        for (const auto& parent : {folder, folder / ".removed"}) {
            std::error_code ec;
            for (fs::directory_iterator it(parent, ec), end; !ec && it != end; it.increment(ec)) {
                const auto name = it->path().filename().string();
                const auto id = name.substr(0, name.find('-'));
                if (profile_id(id)) highest = std::max(highest, std::atoi(id.c_str() + 1));
            }
        }
        return "p" + std::to_string(highest + 1);
    }

    // A new profile: its own empty saves, the owner's look to start from, its own name for DS games and PictoChat.
    // The index comes last, so a power cut leaves either nothing new or a whole profile.
    std::string add(const std::string& wanted, bool restricted, int picture) {
        const auto name = profile_clean_name(wanted);
        if (name.empty() || !can_add()) return {};
        const auto id = next_id();
        const auto root = data / "profiles" / id;
        std::error_code ec;
        fs::create_directories(root, ec);
        if (ec) return {};
        if (!fs::exists(data / "profiles/active", ec) && !profile_write(data / "profiles/active", active_id + "\n"))
            return {};
        for (const char* file : {"preferences.ini", "ui.ini"})
            if (fs::exists(data / file, ec)) fs::copy_file(data / file, root / file, ec);
        // A small library chosen for someone needs no Systems step: Games opens every game (the last line wins).
        std::ofstream(root / "ui.ini", std::ios::app) << "all_games=1\n";
        std::map<std::string, std::string> values = {{"schema", "1"},
                                                     {"name", name},
                                                     {"picture", std::to_string(picture)},
                                                     {"restricted", restricted ? "1" : "0"},
                                                     {"age", restricted ? "7" : "-1"},
                                                     {"unrated", restricted ? "hide" : "show"}};
        const auto ds_name = profile_ds_name(name);
        if (!profile_write_fields(root / "profile.ini", values) ||
            !profile_write(root / "ds.ini", "nickname=" + ds_name + "\n") ||
            !profile_write(root / "pictochat.ini", "name=" + ds_name + "\ncolour=auto\n"))
            return {};
        auto index = index_fields();
        const auto owner_name = list.front().name;
        index["owner_name"] = owner_name;
        index["owner_picture"] = std::to_string(list.front().picture);
        if (!profile_write_fields(data / "profiles/index.ini", index)) return {};
        load(data);
        return id;
    }

    // Forgot it: a file on the card, made on a computer, a job for a grown-up with a card reader. Only the owner's goes.
    bool honour_pin_reset() {
        const auto request = data.parent_path() / "reset-pin";
        std::error_code ec;
        if (data.empty() || !fs::exists(request, ec)) return false;
        Profile owner = list.front();
        owner.pin.clear();
        const bool saved = save(owner);
        fs::remove(data / "profiles/pin-state.ini", ec);
        if (saved) fs::remove(request, ec);
        return saved;
    }

    bool save(const Profile& changed) {
        bool ok;
        if (changed.owner()) {
            auto index = index_fields();
            index["owner_name"] = profile_clean_name(changed.name);
            index["owner_picture"] = std::to_string(changed.picture);
            if (changed.pin.empty()) index.erase("owner_pin");
            else index["owner_pin"] = changed.pin;
            ok = profile_write_fields(data / "profiles/index.ini", index);
        } else {
            auto values = profile_fields(changed.root / "profile.ini");
            values["schema"] = "1";
            values["name"] = profile_clean_name(changed.name);
            values["picture"] = std::to_string(changed.picture);
            values["restricted"] = changed.restricted ? "1" : "0";
            values["age"] = std::to_string(changed.age);
            values["unrated"] = changed.unrated_hidden ? "hide" : "show";
            values["ports"] = changed.allow_ports ? "1" : "0";
            values["homebrew"] = changed.allow_homebrew ? "1" : "0";
            values["pictochat"] = changed.allow_pictochat ? "1" : "0";
            values["cheats"] = changed.allow_cheats ? "1" : "0";
            values["wireless"] = changed.allow_wireless ? "1" : "0";
            if (changed.pin.empty()) values.erase("pin");
            else values["pin"] = changed.pin;
            ok = profile_write_fields(changed.root / "profile.ini", values);
        }
        load(data);
        return ok;
    }
};
}
