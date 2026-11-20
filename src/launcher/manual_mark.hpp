#pragma once
// Whether a game has a scanned manual RG DS Play shows on its second screen
// (play/src/reader/manual.cpp, find_manuals, docs/MANUALS_PLAN.md): <rom
// stem>.pdf, .cbz or a .manual folder beside the ROM (a zipped ROM by its zip's
// name), or manual.pdf, manual.cbz or a
// manual folder in the game's library folder. Keep the two in step.
#include <filesystem>
#include <map>
#include <string>

namespace manual_mark_detail {
namespace fs = std::filesystem;

inline bool any_spelling(const fs::path& dir, const std::string& stem) {
    std::error_code error;
    for (const char* ext : {".pdf", ".PDF", ".cbz", ".CBZ", ".manual", ".MANUAL"})
        if (fs::exists(dir / (stem + ext), error)) return true;
    return false;
}
} // namespace manual_mark_detail

// DS and 3DS games play in their own engines (no reader there yet), and Game &
// Watch machines use the lower screen for the game: no mark for them. Answers
// are kept per path; `refresh` forgets them (a manual added while browsing).
inline bool manual_game(const std::filesystem::path& rom, const std::string& system,
                        const std::filesystem::path& library_folder, bool refresh = false) {
    using namespace manual_mark_detail;
    static std::map<fs::path, bool> known;
    if (refresh) known.clear();
    if (system == "nds" || system == "3ds" || system == "gw" || system == "ports" || system == "portmaster")
        return false;
    auto found = known.find(rom);
    if (found != known.end()) return found->second;
    bool& result = known[rom] = false;
    std::error_code error;
    if (any_spelling(rom.parent_path(), rom.stem().string())) return result = true;
    if (!library_folder.empty())
        for (const char* name : {"manual.pdf", "manual.cbz", "manual"})
            if (fs::exists(library_folder / name, error)) return result = true;
    return result;
}
