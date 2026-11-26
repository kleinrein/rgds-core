#pragma once
// Settings > Power > Battery health: the fuel gauge now, and what battery-watch wrote down about it
// (os/buildroot/rootfs/usr/libexec/rgds/battery-watch, one tab-separated row every 5 minutes, one when the charger
// comes or goes, two around a sleep). Read when the page opens and every half minute while it is open, never from a
// draw callback. The RK817 counts charge in and out (charge_now, uAh) and keeps its own estimate of a full charge
// (charge_full) against the cell's design (charge_full_design): health is the one over the other, as a phone shows it.
#include "launcher/model.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

namespace rgds {
struct BatteryGauge {
    bool present = false;
    int level = -1;
    long long charge = 0, full = 0, design = 0, current = 0, voltage = 0; // uAh, uA (+ into the battery), uV
    char status = 'N'; // C charging, D discharging, F full, N not charging
    bool plugged = false;
};

struct BatterySample {
    long long time = 0;
    int level = -1;
    long long charge = 0, current = 0, voltage = 0;
    char status = 'N';
    bool plugged = false;
    char row = 'a'; // b first after boot, s first after a sleep, a any other
    long long charged = 0, full = 0;
};

// A sitting with a game (the journal's), so the battery it used can carry the game's system.
struct BatterySitting {
    long long began = 0, seconds = 0;
    std::string system;
};

struct BatteryReport {
    BatteryGauge gauge;
    size_t rows = 0;        // history rows read
    int health = -1;        // charge_full against the design capacity, % (-1 while the gauge has not measured it)
    bool seeded = false;    // charge_full is still a seed the driver was handed, not one it measured
    double cycles = -1;     // what the charger put in, in design capacities
    double watts = 0;       // now: + charging, - on battery
    long long minutes = -1; // to empty on battery, to full on the charger
    // The last 24 hours in half-hour slots, oldest first: the level at each slot's end and how it was spent.
    static constexpr int slots = 48;
    std::array<int, slots> level{};
    std::array<char, slots> kind{}; // 0 unknown or off, 'a' in use on battery, 'c' on the charger, 's' asleep
    // Since the charger was last unplugged (0: on the charger now, or never seen)
    long long unplugged = 0;
    int charged_to = -1;
    long long awake = 0, asleep = 0;        // seconds
    double awake_used = 0, asleep_used = 0; // % of a full charge
    // The last 7 days on battery: what used it, in mAh, most first ("" is the menus, "sleep" asleep)
    std::vector<std::pair<std::string, double>> uses;
    double used_mah = 0;
    double game_watts = -1, menu_watts = -1, sleep_ma = -1;
};

inline fs::path battery_supply_root() {
    const char* root = std::getenv("RGDS_POWER_SUPPLY");
    return root && *root ? fs::path(root) : fs::path("/sys/class/power_supply");
}

inline BatteryGauge read_battery_gauge(const fs::path& root) {
    BatteryGauge gauge;
    auto read = [](const fs::path& path) {
        std::ifstream file(path);
        std::string value;
        std::getline(file, value);
        return value;
    };
    auto number = [&](const fs::path& path) {
        const std::string value = read(path);
        char* end = nullptr;
        const long long n = std::strtoll(value.c_str(), &end, 10);
        return end && end != value.c_str() ? n : 0LL;
    };
    std::error_code error;
    std::vector<fs::path> supplies;
    for (const auto& entry : fs::directory_iterator(root, error)) supplies.push_back(entry.path());
    std::sort(supplies.begin(), supplies.end());
    for (const auto& folder : supplies) {
        const std::string type = read(folder / "type");
        if (type == "Mains" || type == "Wireless" || type.rfind("USB", 0) == 0)
            gauge.plugged = gauge.plugged || read(folder / "online") == "1";
        if (type != "Battery" || gauge.present) continue;
        gauge.present = true;
        const std::string level = read(folder / "capacity");
        if (!level.empty()) gauge.level = std::clamp(std::atoi(level.c_str()), 0, 100);
        gauge.charge = number(folder / "charge_now");
        gauge.full = number(folder / "charge_full");
        gauge.design = number(folder / "charge_full_design");
        gauge.current = number(folder / "current_avg");
        if (!gauge.current) gauge.current = number(folder / "current_now");
        gauge.voltage = number(folder / "voltage_avg");
        if (!gauge.voltage) gauge.voltage = number(folder / "voltage_now");
        const std::string status = read(folder / "status");
        gauge.status = status == "Charging" ? 'C' : status == "Full" ? 'F' : status == "Discharging" ? 'D' : 'N';
    }
    // The RK817 reports current_avg with no sign of its own on some kernels: the status gives it.
    if (gauge.status == 'D' && gauge.current > 0) gauge.current = -gauge.current;
    if (gauge.status == 'C' && gauge.current < 0) gauge.current = -gauge.current;
    return gauge;
}

inline std::vector<BatterySample> read_battery_history(std::istream& in) {
    std::vector<BatterySample> rows;
    for (std::string line; std::getline(in, line);) {
        std::istringstream cells(line);
        BatterySample sample;
        std::string status, plugged, row;
        if (!(cells >> sample.time >> sample.level >> sample.charge >> sample.current >> sample.voltage >> status >>
              plugged >> row >> sample.charged >> sample.full))
            continue;
        if (sample.time <= 0 || sample.level < 0 || sample.level > 100 || status.size() != 1 || row.size() != 1)
            continue;
        if (!rows.empty() && sample.time < rows.back().time) continue; // a clock set back: keep the order
        sample.status = status[0];
        sample.plugged = plugged == "1";
        sample.row = row[0];
        rows.push_back(sample);
    }
    return rows;
}

inline BatteryReport battery_report(const BatteryGauge& gauge, const std::vector<BatterySample>& rows,
                                    std::vector<BatterySitting> sittings, long long now) {
    BatteryReport report;
    report.gauge = gauge;
    report.rows = rows.size();
    report.level.fill(-1);
    const long long design = gauge.design > 0 ? gauge.design : 0;
    const long long full = gauge.full > 0 ? gauge.full : rows.empty() ? 0 : rows.back().full;
    // The RK817 driver keeps its full charge in the PMIC's NVRAM and only moves it after a run to empty and a charge to
    // full. Until then it holds a seed: a device tree's design figure, its own or the other board's (4000 mAh on the
    // RG DS, 4247 on the Plus, so a new Plus whose PMIC first met an RG DS tree reads 94 %). A measured charge is
    // counted to the mAh, so it landing on one of these exactly is not a reading worth showing as health.
    report.seeded = full > 0 && (full == design || full == 4000000 || full == 4247000);
    if (design > 0 && full > 0 && !report.seeded)
        report.health = int(std::min<long long>(100, (full * 100 + design / 2) / design));
    if (design > 0 && !rows.empty()) report.cycles = double(rows.back().charged) / double(design);
    report.watts = double(gauge.current) * double(gauge.voltage) / 1e12;
    const bool charging = gauge.plugged && gauge.status == 'C';

    // A pair of rows in a row is a stretch of time: in use, asleep or on the charger; a row after a boot follows a
    // stretch nobody saw (powered off).
    auto kind_of = [](const BatterySample& a, const BatterySample& b) {
        return b.row == 'b' ? char(0) : a.plugged && b.plugged ? 'c' : b.row == 's' ? 's' : 'a';
    };
    // what the battery gave in a stretch on battery, uAh (a recalibrating gauge can jump: ignore what can't be)
    auto drop = [](const BatterySample& a, const BatterySample& b) {
        const long long used = a.charge - b.charge, seconds = std::max(1LL, b.time - a.time);
        return used > 0 && used <= seconds * 1500 ? used : 0LL; // 5.4 A at most
    };

    // Time left: the last hour in use on battery when there is enough of it, the gauge's average otherwise.
    if (charging && gauge.current > 0 && full > gauge.charge)
        report.minutes = (full - gauge.charge) * 60 / gauge.current;
    else if (!gauge.plugged && gauge.charge > 0) {
        long long used = 0, seconds = 0;
        for (size_t i = 1; i < rows.size(); i++)
            if (rows[i].time >= now - 3600 && kind_of(rows[i - 1], rows[i]) == 'a' && !rows[i - 1].plugged) {
                used += drop(rows[i - 1], rows[i]);
                seconds += rows[i].time - rows[i - 1].time;
            }
        const long long rate = seconds >= 900 && used > 0 ? used * 3600 / seconds : -gauge.current;
        if (rate > 20000) report.minutes = gauge.charge * 60 / rate;
    }

    // The chart: each slot's end between two rows reads the line between them; after the last row, the gauge.
    const long long span = 24 * 3600, step = span / BatteryReport::slots, start = now - span;
    for (int i = 0; i < BatteryReport::slots; i++) {
        const long long at = start + (i + 1) * step;
        if (i == BatteryReport::slots - 1 || (!rows.empty() && at > rows.back().time && at - rows.back().time < 900)) {
            if (gauge.level >= 0) {
                report.level[size_t(i)] = gauge.level;
                report.kind[size_t(i)] = gauge.plugged ? 'c' : 'a';
            }
            continue;
        }
        const auto next = std::upper_bound(rows.begin(), rows.end(), at,
                                           [](long long t, const BatterySample& row) { return t < row.time; });
        if (next == rows.begin() || next == rows.end()) continue;
        const auto& a = *(next - 1);
        const auto& b = *next;
        const char kind = kind_of(a, b);
        if (!kind) continue;
        const double f = double(at - a.time) / double(std::max(1LL, b.time - a.time));
        report.level[size_t(i)] = int(std::lround(a.level + (b.level - a.level) * f));
        report.kind[size_t(i)] = kind;
    }

    // Since the charger was last unplugged.
    if (!gauge.plugged && !rows.empty()) {
        size_t from = rows.size();
        while (from > 0 && !rows[from - 1].plugged) from--;
        {
            // no charge in the history yet: since the first row
            report.charged_to = from > 0 ? rows[from - 1].level : -1;
            report.unplugged = from < rows.size() ? rows[from].time : now;
            const double whole = double(std::max(1LL, full));
            for (size_t i = from + 1; i < rows.size(); i++) { // from the first row off the charger
                const char kind = kind_of(rows[i - 1], rows[i]);
                const long long seconds = rows[i].time - rows[i - 1].time;
                const double used = 100.0 * double(drop(rows[i - 1], rows[i])) / whole;
                if (kind == 'a') {
                    report.awake += seconds;
                    report.awake_used += used;
                } else if (kind == 's') {
                    report.asleep += seconds;
                    report.asleep_used += used;
                }
            }
            // and since the last row, in use
            const auto& last = rows.back();
            if (now > last.time && now - last.time < 900) {
                report.awake += now - last.time;
                if (gauge.charge > 0 && last.charge > gauge.charge)
                    report.awake_used += 100.0 * double(last.charge - gauge.charge) / whole;
            }
        }
    }

    // The last 7 days on battery, by what was on screen: a game's system while one was played, the menus otherwise.
    std::sort(sittings.begin(), sittings.end(), [](const auto& a, const auto& b) { return a.began < b.began; });
    std::map<std::string, double> used;
    double game_wh = 0, menu_wh = 0, game_h = 0, menu_h = 0, sleep_mah = 0, sleep_h = 0;
    for (size_t i = 1; i < rows.size(); i++) {
        const auto& a = rows[i - 1];
        const auto& b = rows[i];
        if (b.time < now - 7 * 24 * 3600 || a.plugged || b.plugged) continue;
        const char kind = kind_of(a, b);
        const double mah = double(drop(a, b)) / 1000.0, hours = double(b.time - a.time) / 3600.0;
        const double volts = double(a.voltage + b.voltage) / 2e6;
        if (kind == 's') {
            sleep_mah += mah;
            sleep_h += hours;
            continue;
        }
        if (kind != 'a' || hours <= 0) continue;
        double played = 0;
        for (const auto& sitting : sittings) {
            if (sitting.began >= b.time) break;
            const long long overlap =
                std::min(b.time, sitting.began + sitting.seconds) - std::max(a.time, sitting.began);
            if (overlap <= 0) continue;
            const double share = std::min(1.0 - played, double(overlap) / double(b.time - a.time));
            if (share <= 0) continue;
            used[sitting.system] += mah * share;
            game_wh += mah * share * volts / 1000.0;
            game_h += hours * share;
            played += share;
        }
        used[""] += mah * (1 - played);
        menu_wh += mah * (1 - played) * volts / 1000.0;
        menu_h += hours * (1 - played);
    }
    if (sleep_mah > 0) used["sleep"] = sleep_mah;
    for (const auto& [what, mah] : used)
        if (mah >= 1) {
            report.uses.push_back({what, mah});
            report.used_mah += mah;
        }
    std::sort(report.uses.begin(), report.uses.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    if (game_h >= .25) report.game_watts = game_wh / game_h;
    if (menu_h >= .25) report.menu_watts = menu_wh / menu_h;
    if (sleep_h >= 1) report.sleep_ma = sleep_mah / sleep_h;
    return report;
}
} // namespace rgds
