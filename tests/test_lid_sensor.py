"""Replay actual RG DS close/open pulses, with release and data-loss cases."""
import os
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'runtime'))
from lid_sensor import LidSensor, EVENT, discover, read_state, read_snapshot


class SensorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.fifo = self.root/'input'
        os.mkfifo(self.fifo)
        self.writer = os.open(self.fifo, os.O_RDWR | os.O_NONBLOCK)
        self.addCleanup(os.close, self.writer)
        self.sensor = LidSensor(self.fifo)
        self.addCleanup(self.sensor.close)

    def event(self, code, value, kind=1):
        return EVENT.pack(0, 0, kind, code, value)

    def test_real_trace_latches_press_and_ignores_one_second_release(self):
        self.assertEqual(read_state(self.sensor.path), -1)
        for code, value, expected in [(110,1,1),(110,0,1),(111,1,0),(111,0,0),
                                       (116,1,0),(110,2,0),(110,1,1)]:
            os.write(self.writer, self.event(code,value))
            self.sensor.poll()
            self.assertEqual(read_state(self.sensor.path), expected)

    def test_partial_records_and_quick_reopen_drain_to_current_position(self):
        close=self.event(110,1)
        os.write(self.writer,close[:7]);self.sensor.poll()
        self.assertEqual(read_state(self.sensor.path),-1)
        os.write(self.writer,close[7:]+self.event(111,1));self.sensor.poll()
        self.assertEqual(read_state(self.sensor.path),0)

    def test_close_generation_preserves_pairs_between_owner_polls(self):
        os.write(self.writer,self.event(110,1));self.sensor.poll()
        self.assertEqual(read_snapshot(self.sensor.path),(1,1))
        os.write(self.writer,self.event(111,1)+self.event(110,1));self.sensor.poll()
        self.assertEqual(read_snapshot(self.sensor.path),(1,2))
        os.write(self.writer,self.event(110,1)+self.event(110,0));self.sensor.poll()
        self.assertEqual(read_snapshot(self.sensor.path),(1,2))

    def test_dropped_events_and_disconnected_sensor_do_not_claim_closed(self):
        os.write(self.writer,self.event(110,1)+self.event(3,0,0)+self.event(110,1)+self.event(0,0,0))
        self.sensor.poll();self.assertEqual(read_state(self.sensor.path),-1)
        path=self.sensor.path;self.sensor.close()
        self.assertIsNone(read_state(path))

    def test_stale_future_and_malformed_records_fail_closed_to_requests(self):
        for stamp,value in [(time.monotonic()-4,1),(time.monotonic()+10,1),('nan',1),(time.monotonic(),2)]:
            self.sensor.path.write_text(f'{stamp} {value}\n')
            self.assertIsNone(read_state(self.sensor.path))
        self.sensor.path.write_text('3') # Old hallkey is never a valid state.
        self.assertIsNone(read_state(self.sensor.path))

    def test_helper_process_reads_the_same_latched_state_and_cleanup_invalidates_it(self):
        import subprocess
        os.write(self.writer, self.event(110, 1));self.sensor.poll()
        script = "from lid_sensor import read_state; print(read_state())"
        env = dict(os.environ, RGDS_LID_STATE=str(self.sensor.path),
                   PYTHONPATH=str(Path(__file__).resolve().parents[1]/'runtime'))
        result = subprocess.run([sys.executable, '-B', '-c', script], env=env,
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.strip(), '1')
        self.sensor.close()
        result = subprocess.run([sys.executable, '-B', '-c', script], env=env,
                                capture_output=True, text=True, check=True)
        self.assertEqual(result.stdout.strip(), 'None')

    def test_discovery_uses_identity_and_capabilities_not_event_number(self):
        node=self.root/'nodes/event9';node.parent.mkdir();node.touch()
        entry=self.root/'sys/event9/device';(entry/'capabilities').mkdir(parents=True)
        (entry/'name').write_text('rk805 pwrkey')
        value=sum(1<<code for code in (110,111,116))
        (entry/'capabilities/key').write_text(f'{value>>64:x} {value&((1<<64)-1):x}')
        self.assertEqual(discover(self.root/'sys',node.parent),node)
        (entry/'name').write_text('some keyboard')
        self.assertIsNone(discover(self.root/'sys',node.parent))
