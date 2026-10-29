// SPDX-License-Identifier: GPL-3.0-or-later
// GBA, GB and GBC through libmgba, linked directly so the companion screen can
// run a shadow copy of the game and choose which layers it draws.
#include "core/core.hpp"
#include "core/mgba_gb.h"
#include "core/mgba_split.h"

#include <mgba/core/cheats.h>
#include <mgba/core/config.h>
#include <mgba/core/core.h>
#include <mgba/core/log.h>
#include <mgba/core/serialize.h>
#include <mgba-util/audio-buffer.h>
#include <mgba-util/vfs.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>

namespace play {
namespace {

void quiet_log(struct mLogger*, int, enum mLogLevel level, const char* format, va_list args) {
    if (!(level & (mLOG_FATAL | mLOG_ERROR))) return;
    std::fputs("mgba: ", stderr);
    std::vfprintf(stderr, format, args);
    std::fputc('\n', stderr);
}
struct mLogger quiet_logger = {quiet_log, nullptr};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string extension(const std::string& name) {
    auto dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot));
}

// The battery save lives in memory; mGBA writes into it as the game does and
// calls sync() once a write has settled. That moment is when the frontend
// copies the save out and writes it to the card atomically, so a save on the
// card is never half written.
struct SyncFile {
    VFile d;
    VFile* inner;
    std::atomic<uint64_t>* revision;
};
SyncFile* sync_file(VFile* vf) { return reinterpret_cast<SyncFile*>(vf); }
bool sf_close(VFile* vf) {
    auto* f = sync_file(vf);
    bool ok = f->inner->close(f->inner);
    delete f;
    return ok;
}
off_t sf_seek(VFile* vf, off_t offset, int whence) {
    return sync_file(vf)->inner->seek(sync_file(vf)->inner, offset, whence);
}
ssize_t sf_read(VFile* vf, void* buffer, size_t size) {
    return sync_file(vf)->inner->read(sync_file(vf)->inner, buffer, size);
}
ssize_t sf_readline(VFile* vf, char* buffer, size_t size) {
    return sync_file(vf)->inner->readline(sync_file(vf)->inner, buffer, size);
}
ssize_t sf_write(VFile* vf, const void* buffer, size_t size) {
    return sync_file(vf)->inner->write(sync_file(vf)->inner, buffer, size);
}
void* sf_map(VFile* vf, size_t size, int flags) { return sync_file(vf)->inner->map(sync_file(vf)->inner, size, flags); }
void sf_unmap(VFile* vf, void* memory, size_t size) { sync_file(vf)->inner->unmap(sync_file(vf)->inner, memory, size); }
void sf_truncate(VFile* vf, size_t size) { sync_file(vf)->inner->truncate(sync_file(vf)->inner, size); }
ssize_t sf_size(VFile* vf) { return sync_file(vf)->inner->size(sync_file(vf)->inner); }
bool sf_sync(VFile* vf, void*, size_t) {
    sync_file(vf)->revision->fetch_add(1);
    return true;
}
VFile* open_sync_file(const std::vector<uint8_t>& initial, std::atomic<uint64_t>* revision) {
    VFile* inner = VFileMemChunk(initial.empty() ? nullptr : initial.data(), initial.size());
    if (!inner) return nullptr;
    auto* f = new SyncFile{{}, inner, revision};
    f->d.close = sf_close;
    f->d.seek = sf_seek;
    f->d.read = sf_read;
    f->d.readline = sf_readline;
    f->d.write = sf_write;
    f->d.map = sf_map;
    f->d.unmap = sf_unmap;
    f->d.truncate = sf_truncate;
    f->d.size = sf_size;
    f->d.sync = sf_sync;
    return &f->d;
}

// mGBA integrates the motor pin over each frame and hands the result here.
struct RumbleTap {
    mRumbleIntegrator integrator;
    float level;
};
void tap_rumble(mRumbleIntegrator* integrator, float level) { reinterpret_cast<RumbleTap*>(integrator)->level = level; }

class MgbaCore final : public Core {
public:
    ~MgbaCore() override {
        play_split_detach(split_);
        if (core_) {
            core_->unloadROM(core_);
            mCoreConfigDeinit(&core_->config);
            core_->deinit(core_);
        }
    }

    bool open(RomBytes rom, const std::vector<uint8_t>& battery, bool silent, std::string& error) {
        rom_ = std::move(rom);
        silent_ = silent;
        VFile* vf = VFileFromConstMemory(rom_->data(), rom_->size());
        core_ = vf ? mCoreFindVF(vf) : nullptr;
        if (!core_) {
            if (vf) vf->close(vf);
            error = "Not a Game Boy or Game Boy Advance ROM";
            return false;
        }
        if (!core_->init(core_)) {
            vf->close(vf);
            core_ = nullptr;
            error = "The emulator could not start";
            return false;
        }
        mCoreInitConfig(core_, nullptr);
        mCoreOptions options{};
        options.useBios = false;
        options.skipBios = true;
        options.volume = 0x100;
        options.audioBuffers = 1024;
        mCoreConfigLoadDefaults(&core_->config, &options);
        // Super Game Boy games keep their SGB colours but not the 256x224 border.
        mCoreConfigSetDefaultIntValue(&core_->config, "sgb.borders", 0);
        // A Super Game Boy cartridge plays on a Game Boy: its screen is the handheld's
        // (core/mgba_gb.h). Cartridges made for the Game Boy Color still run on one.
        mCoreConfigSetDefaultValue(&core_->config, "sgb.model", "DMG");
        // Maps the defaults into core->opts, then applies them (volume included:
        // loadConfig alone reads opts as they were, zero, and mutes the game).
        mCoreLoadForeignConfig(core_, &core_->config);

        // The buffer holds the largest picture the core can draw (a GB core's is
        // the SGB border's 256x224); the game's own size is known once it runs.
        unsigned w = 0, h = 0;
        core_->baseVideoSize(core_, &w, &h);
        stride_ = static_cast<int>(w);
        video_.assign(static_cast<size_t>(w) * h, 0);
        core_->setVideoBuffer(core_, reinterpret_cast<mColor*>(video_.data()), w);
        core_->setAudioBufferSize(core_, 1024);

        if (!core_->loadROM(core_, vf)) {
            error = "The ROM could not be loaded";
            return false;
        }
        VFile* save = open_sync_file(battery, &battery_revision_);
        if (!save || !core_->loadSave(core_, save)) {
            if (save) save->close(save);
            error = "The battery save could not be attached";
            return false;
        }
        if (!silent_) {
            mRumbleIntegratorInit(&rumble_.integrator);
            rumble_.integrator.setRumble = tap_rumble;
            rumble_.level = 0;
            core_->setPeripheral(core_, mPERIPH_RUMBLE, &rumble_.integrator.d);
        }
        core_->reset(core_);
        play_gb_handheld_model(core_);
        core_->currentVideoSize(core_, &w, &h);
        width_ = static_cast<int>(w);
        height_ = static_cast<int>(h);
        mGameInfo info{};
        core_->getGameInfo(core_, &info);
        code_.assign(info.code, strnlen(info.code, sizeof(info.code)));
        title_.assign(info.title, strnlen(info.title, sizeof(info.title)));
        switch (core_->platform(core_)) {
        case mPLATFORM_GBA: system_ = "gba"; break;
        case mPLATFORM_GB:
            system_ = extension(info.system) == "cgb" || std::strncmp(info.system, "CGB", 3) == 0 ? "gbc" : "gb";
            break;
        default: system_ = "unknown";
        }
        int32_t cycles = core_->frameCycles(core_), frequency = core_->frequency(core_);
        frame_rate_ = cycles > 0 ? static_cast<double>(frequency) / cycles : 60.0;
        return true;
    }

    const char* system() const override { return system_; }
    std::string game_code() const override { return code_; }
    std::string header_title() const override { return title_; }
    double frame_rate() const override { return frame_rate_; }

    void run_frame(uint32_t buttons) override {
        core_->setKeys(core_, buttons & 0x3FF);
        core_->runFrame(core_);
        if (silent_) {
            // A shadow never plays its sound; keep its buffer from filling.
            mAudioBuffer* audio = core_->getAudioBuffer(core_);
            if (audio) mAudioBufferClear(audio);
        }
    }
    Picture picture() const override { return {video_.data(), width_, height_, stride_}; }
    float rumble() const override { return rumble_.level; }

    // A state is the machine followed by mGBA's extra-data block holding the
    // cartridge RAM (the file mGBA itself writes, without the clock metadata,
    // so the same machine always gives the same bytes). The raw machine leaves
    // that RAM out, and games that keep their working variables there (Super
    // Mario Land 3) hang when loaded against another moment's RAM.
    bool save_state(std::vector<uint8_t>& out) override {
        VFile* vf = VFileMemChunk(nullptr, 0);
        if (!vf) return false;
        bool ok = mCoreSaveStateNamed(core_, vf, SAVESTATE_SAVEDATA);
        if (ok) {
            out.resize(static_cast<size_t>(vf->size(vf)));
            vf->seek(vf, 0, SEEK_SET);
            ok = vf->read(vf, out.data(), out.size()) == static_cast<ssize_t>(out.size());
        }
        vf->close(vf);
        return ok;
    }
    bool load_state(const std::vector<uint8_t>& in) override {
        const size_t size = core_->stateSize(core_);
        if (in.size() < size) return false;
        if (!core_->loadState(core_, in.data())) return false;
        // The cartridge RAM it was saved with, when it carries it (older
        // states are the machine alone). Into memory only: the battery on the
        // card follows when the game next writes its RAM, so an older
        // checkpoint never rolls an in-game save back by itself.
        if (in.size() > size) {
            VFile* vf = VFileFromConstMemory(in.data(), in.size());
            mStateExtdata extdata;
            mStateExtdataInit(&extdata);
            mStateExtdataItem item{};
            if (vf && mCoreExtractExtdata(core_, vf, &extdata) && mStateExtdataGet(&extdata, EXTDATA_SAVEDATA, &item) &&
                item.data) {
                void* current = nullptr;
                const size_t have = core_->savedataClone(core_, &current);
                std::free(current);
                // Same size only: the RAM is mapped from the save file, which must not grow.
                if (static_cast<size_t>(item.size) == have) core_->savedataRestore(core_, item.data, have, true);
            }
            mStateExtdataDeinit(&extdata);
            if (vf) vf->close(vf);
        }
        // A state brings its model and the colours it was drawn with.
        play_gb_handheld_model(core_);
        play_gb_set_gbc_colours(core_, gbc_colours_);
        return true;
    }

    bool colour_hardware() const override { return play_gb_colour_model(core_); }
    void set_gbc_colours(bool on) override {
        gbc_colours_ = on;
        play_gb_set_gbc_colours(core_, on);
    }

    uint32_t peek(uint32_t address, int size) override {
        switch (size) {
        case 1: return core_->busRead8(core_, address);
        case 2: return core_->busRead16(core_, address);
        default: return core_->busRead32(core_, address);
        }
    }
    uint32_t peek_bank(uint32_t address, int bank, int size) override {
        switch (size) {
        case 1: return core_->rawRead8(core_, address, bank);
        case 2: return core_->rawRead16(core_, address, bank);
        default: return core_->rawRead32(core_, address, bank);
        }
    }
    void poke8(uint32_t address, int bank, uint8_t value) override { core_->rawWrite8(core_, address, bank, value); }

    size_t audio(int16_t* stereo, size_t max_frames) override {
        mAudioBuffer* audio = core_->getAudioBuffer(core_);
        return audio ? mAudioBufferRead(audio, stereo, max_frames) : 0;
    }
    unsigned audio_rate() const override { return core_->audioSampleRate(core_); }

    std::vector<uint8_t> battery() const override {
        void* data = nullptr;
        size_t size = core_->savedataClone(core_, &data);
        std::vector<uint8_t> out;
        if (data) {
            out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
            std::free(data);
        }
        return out;
    }
    uint64_t battery_revision() const override { return battery_revision_.load(); }
    void restore_battery(const std::vector<uint8_t>& bytes) override {
        if (!bytes.empty()) core_->savedataRestore(core_, bytes.data(), bytes.size(), false);
    }

    // Each code that is on becomes a set of lines in mGBA's cheat device, which runs them every frame. A code's
    // parts are split by "+" or spaces: a Game Boy Advance line is two parts ("AAAAAAAA BBBBBBBB"), a Game Boy one
    // part (GameShark 01VVAAAA, Game Genie XXX-XXX-XXX).
    bool apply_cheats(const std::vector<CheatCode>& codes) override {
        mCheatDevice* device = core_->cheatDevice(core_);
        if (!device) return false;
        mCheatDeviceClear(device);
        const bool advance = std::strcmp(system(), "gba") == 0;
        for (const auto& c : codes) {
            if (!c.on) continue;
            std::vector<std::string> parts;
            std::string part;
            for (char ch : c.code + " ") {
                if (ch == '+' || std::isspace(static_cast<unsigned char>(ch))) {
                    if (!part.empty()) parts.push_back(part);
                    part.clear();
                } else {
                    part += ch;
                }
            }
            mCheatSet* set = device->createSet(device, c.name.c_str());
            for (size_t i = 0; i < parts.size(); i += advance ? 2 : 1) {
                std::string line = parts[i];
                if (advance) {
                    if (i + 1 >= parts.size()) break;
                    line += " " + parts[i + 1];
                }
                if (!mCheatAddLine(set, line.c_str(), 0))
                    std::fprintf(stderr, "play: cheat \"%s\": \"%s\" is not a code for this system\n", c.name.c_str(),
                                 line.c_str());
            }
            set->enabled = true;
            mCheatAddSet(device, set);
        }
        return true;
    }

    void set_layers(unsigned mask) override {
        if (!core_->enableVideoLayer) return;
        for (size_t id = 0; id < 5; ++id) core_->enableVideoLayer(core_, id, (mask >> id) & 1);
    }

    void hud_objects(const std::vector<ObjRule>& rules) override {
        objects_.clear();
        for (const auto& r : rules) objects_.push_back({uint16_t(r.lo), uint16_t(r.hi), int8_t(r.priority)});
        if (split_) play_split_set_objects(split_, objects_.data(), int(objects_.size()));
    }
    void hud_window_effects_off(unsigned windows) override {
        effects_off_ = windows;
        if (split_) play_split_set_window_effects_off(split_, windows);
    }
    bool hud_split(bool on, unsigned bg_mask, int sprite_priority) override {
        if (!split_ && on) {
            hud_.assign(static_cast<size_t>(width_) * height_, play_split_key());
            split_ = play_split_attach(core_, hud_.data());
        }
        if (!split_) return false;
        play_split_set_objects(split_, objects_.data(), int(objects_.size()));
        play_split_set_window_effects_off(split_, effects_off_);
        play_split_set(split_, on, bg_mask, sprite_priority);
        return true;
    }
    Picture hud() const override {
        if (hud_.empty()) return {};
        return {hud_.data(), width_, height_, width_};
    }
    uint32_t hud_key() const override { return play_split_key(); }

    std::unique_ptr<Core> shadow() const override {
        auto copy = std::make_unique<MgbaCore>();
        std::string error;
        // With the battery memory as it is now (in memory only; see restore_battery).
        if (!copy->open(rom_, battery(), true, error)) return nullptr;
        copy->set_gbc_colours(gbc_colours_);
        return copy;
    }

private:
    mCore* core_ = nullptr;
    PlaySplit* split_ = nullptr;
    std::vector<PlayObjRule> objects_;
    unsigned effects_off_ = 0;
    std::vector<uint32_t> hud_;
    RomBytes rom_;
    std::vector<uint32_t> video_;
    int width_ = 0, height_ = 0, stride_ = 0;
    RumbleTap rumble_{};
    bool silent_ = false;
    std::atomic<bool> gbc_colours_{false}; // shadows are made on the companion's threads
    const char* system_ = "unknown";
    std::string code_, title_;
    double frame_rate_ = 60.0;
    std::atomic<uint64_t> battery_revision_{0};
};

std::vector<uint8_t> read_vfile(VFile* vf) {
    std::vector<uint8_t> bytes;
    ssize_t size = vf->size(vf);
    if (size <= 0 || size > 64 * 1024 * 1024) return bytes;
    bytes.resize(static_cast<size_t>(size));
    vf->seek(vf, 0, SEEK_SET);
    size_t done = 0;
    while (done < bytes.size()) {
        ssize_t n = vf->read(vf, bytes.data() + done, bytes.size() - done);
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    bytes.resize(done);
    return bytes;
}

} // namespace

bool is_rom_name(const std::string& name) {
    auto ext = extension(name);
    return ext == ".gba" || ext == ".gb" || ext == ".gbc" || ext == ".sgb" || ext == ".agb" || ext == ".bin";
}

RomBytes read_rom(const std::string& path, std::string& error, std::string* inner_name) {
    static bool logger_set = false;
    if (!logger_set) {
        mLogSetDefaultLogger(&quiet_logger);
        logger_set = true;
    }
    auto ext = extension(path);
    std::vector<uint8_t> bytes;
    if (ext == ".zip" || ext == ".7z") {
        VDir* dir = VDirOpenArchive(path.c_str());
        if (!dir) {
            error = "The archive could not be opened";
            return nullptr;
        }
        for (VDirEntry* entry = dir->listNext(dir); entry; entry = dir->listNext(dir)) {
            std::string name = entry->name(entry);
            if (entry->type(entry) != VFS_FILE || !is_rom_name(name) || ext == ".bin") continue;
            VFile* vf = dir->openFile(dir, name.c_str(), O_RDONLY);
            if (!vf) continue;
            bytes = read_vfile(vf);
            vf->close(vf);
            if (!bytes.empty()) {
                if (inner_name) *inner_name = name;
                break;
            }
        }
        dir->close(dir);
        if (bytes.empty()) error = "No Game Boy or Game Boy Advance ROM inside the archive";
    } else {
        VFile* vf = VFileOpen(path.c_str(), O_RDONLY);
        if (!vf) {
            error = "The ROM could not be opened";
            return nullptr;
        }
        bytes = read_vfile(vf);
        vf->close(vf);
        if (bytes.empty()) error = "The ROM is empty or too large";
        if (inner_name) *inner_name = path;
    }
    if (bytes.empty()) return nullptr;
    return std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
}

OpenResult open_mgba(RomBytes rom, const std::string&, const std::vector<uint8_t>& battery) {
    OpenResult result;
    auto core = std::make_unique<MgbaCore>();
    if (core->open(std::move(rom), battery, false, result.error)) result.core = std::move(core);
    return result;
}

} // namespace play
