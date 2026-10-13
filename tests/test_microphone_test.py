"""Real PCM level calculations and refusal to compete with a running engine."""
import array,math,sys,tempfile,unittest
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'runtime'))
import microphone_test
from fields import write_fields
from records import write_record

def pcm(values):
 result=array.array('h',values)
 if sys.byteorder!='little':result.byteswap()
 return result.tobytes()

class MicrophoneTests(unittest.TestCase):
 def test_silence_and_dc_offset_are_quiet(self):
  self.assertEqual(microphone_test.levels(pcm([0]*800)),(0,0,-60))
  self.assertEqual(microphone_test.levels(pcm([250]*800)),(0,0,-60))
 def test_levels_rise_with_real_sample_amplitude(self):
  def tone(amplitude):return pcm([round(amplitude*math.sin(i*math.pi/8)) for i in range(800)])
  low=microphone_test.levels(tone(100));high=microphone_test.levels(tone(20000))
  self.assertLess(low[0],high[0]);self.assertGreaterEqual(high[1],high[0]);self.assertAlmostEqual(high[2],-7,delta=1)
 def test_active_game_never_opens_capture(self):
  with tempfile.TemporaryDirectory() as tmp:
   write_record(Path(tmp)/'session/core.json',dict(pid=123,start='456'))
   with patch('microphone_test.process_identity',return_value='456'),patch('microphone_test.subprocess.Popen') as spawn,patch('builtins.print') as output:
    microphone_test.run(tmp);spawn.assert_not_called();output.assert_called_once_with('busy',flush=True)
if __name__=='__main__':unittest.main()
