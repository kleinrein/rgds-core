"""Settings > Power > Battery health: the gauge reader and the history analysis (src/launcher/battery_health.hpp)."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PROGRAM = r'''
#include "launcher/battery_health.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace rgds;
int main(int argc, char** argv) {
    assert(argc == 2);
    // The gauge: the first Battery supply, a USB charger's online, current signed by the status.
    const BatteryGauge gauge = read_battery_gauge(argv[1]);
    assert(gauge.present && gauge.level == 50 && gauge.full == 3600000 && gauge.design == 4000000);
    assert(gauge.status == 'D' && gauge.current == -600000 && !gauge.plugged && gauge.voltage == 3800000);
    assert(!read_battery_gauge(std::string(argv[1]) + "/missing").present);

    // A day: in use on battery, a 10-hour sleep, 15 minutes on the charger, half an hour of DS, half an hour of menus.
    std::stringstream file;
    long long charge = 3200000;
    auto row = [&](long long t, bool plugged, char kind, long long charged = 8000000) {
        file << t << '\t' << charge * 100 / 3600000 << '\t' << charge << "\t-1\t3800000\t" << (plugged ? 'C' : 'D')
             << '\t' << plugged << '\t' << kind << '\t' << charged << "\t3600000\n";
    };
    row(1000, false, 'b');
    for (long long t = 1300; t <= 4600; t += 300) {
        charge -= 10000; // 120 mA
        row(t, false, 'a');
    }
    charge -= 36000; // 3.6 mA for 10 hours
    row(40600, false, 's');
    file << "not a row\n" << "39000\t50\t1\t1\t1\tD\t0\ta\t1\t1\n"; // junk, and a clock set back: both skipped
    charge += 100000;
    row(40900, true, 'a');
    charge += 100000;
    row(41200, true, 'a');
    row(41500, false, 'a');
    for (long long t = 41800; t <= 43300; t += 300) {
        charge -= 50000; // 600 mA in the game
        row(t, false, 'a');
    }
    for (long long t = 43600; t <= 45100; t += 300) {
        charge -= 20000; // 240 mA in the menus
        row(t, false, 'a');
    }
    const auto rows = read_battery_history(file);
    assert(rows.size() == 29 && rows.front().row == 'b' && rows[13].row == 's');

    BatteryGauge now = gauge;
    now.charge = charge;
    now.level = int(charge * 100 / 3600000);
    const auto report = battery_report(now, rows, {{41500, 1800, "nds"}}, 45200);
    assert(report.health == 90);
    assert(std::abs(report.cycles - 2.0) < 1e-9);
    assert(std::abs(report.watts + 2.28) < 1e-9);
    assert(report.minutes > 0);
    // since unplugged at 41500, charged to the last plugged row's level; the 100 s since the last row count as use
    assert(report.unplugged == 41500 && report.charged_to == int((3200000 - 120000 - 36000 + 200000) * 100 / 3600000));
    assert(report.awake == 45200 - 41500 && report.asleep == 0);
    assert(std::abs(report.awake_used - 100.0 * 420000 / 3600000) < 1e-6);
    // what used it: the DS, then the menus (before and after), then the sleep
    assert(report.uses.size() == 3);
    assert(report.uses[0].first == "nds" && std::abs(report.uses[0].second - 300) < 1e-6);
    assert(report.uses[1].first == "" && std::abs(report.uses[1].second - 240) < 1e-6);
    assert(report.uses[2].first == "sleep" && std::abs(report.uses[2].second - 36) < 1e-6);
    assert(std::abs(report.game_watts - 300 * 3.8 / 1000 / .5) < 1e-6);
    assert(std::abs(report.sleep_ma - 3.6) < 1e-6);
    // the chart: nothing before the first row, the sleep as asleep, the last slot the gauge's
    int asleep = 0, unknown = 0;
    for (int i = 0; i < BatteryReport::slots; i++) {
        asleep += report.kind[size_t(i)] == 's';
        unknown += report.level[size_t(i)] < 0;
    }
    assert(asleep >= 19 && asleep <= 21);
    assert(unknown >= 22);
    assert(report.level[BatteryReport::slots - 1] == now.level && report.kind[BatteryReport::slots - 1] == 'a');

    // On the charger: time to full from the charge current; nothing "since unplugged".
    BatteryGauge plugged = now;
    plugged.plugged = true;
    plugged.status = 'C';
    plugged.current = 1800000;
    const auto charging = battery_report(plugged, rows, {}, 45200);
    assert(charging.minutes == (3600000 - charge) * 60 / 1800000 && charging.unplugged == 0 && charging.watts > 0);
    // No history and no design capacity: nothing made up.
    BatteryGauge bare;
    bare.present = true;
    bare.level = 40;
    const auto empty = battery_report(bare, {}, {}, 45200);
    assert(empty.health == -1 && empty.cycles < 0 && empty.uses.empty() && empty.unplugged == 0);
    std::cout << "ok\n";
}
'''


class BatteryHealthTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('clang++'), 'clang++ required')
    def test_gauge_and_history_analysis(self):
        with tempfile.TemporaryDirectory() as folder:
            d = Path(folder)
            supplies = d / 'power_supply'
            for name, values in (
                (
                    'battery',
                    dict(
                        type='Battery',
                        capacity=50,
                        charge_now=1800000,
                        charge_full=3600000,
                        charge_full_design=4000000,
                        current_avg=600000,
                        voltage_avg=3800000,
                        status='Discharging'
                    )
                ),
                ('charger', dict(type='USB', online=0)),
            ):
                (supplies / name).mkdir(parents=True)
                for key, value in values.items():
                    (supplies / name / key).write_text(f'{value}\n')
            source, binary = d / 'test.cpp', d / 'test'
            source.write_text(PROGRAM)
            subprocess.run(
                ['clang++', '-std=c++17', '-I' + str(ROOT / 'src'),
                 str(source), '-o', str(binary)], check=True
            )
            result = subprocess.run([str(binary), str(supplies)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout, 'ok\n')


if __name__ == '__main__':
    unittest.main()
