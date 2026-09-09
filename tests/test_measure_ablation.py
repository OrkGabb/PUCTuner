import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("measure", Path(__file__).parents[1] / "tools/measure_ablation.py")
measure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measure)


class MeasurementTest(unittest.TestCase):
    def test_fps_uses_observed_duration_not_window_count(self):
        rows = []
        for count, span in [(180, 6000), (240, 8000)]:
            rows.append(dict(accepted=True, arm="observe", frames=count, frame_time_ms=span,
                             p95_ms=34, slow_frames_50ms=0, temp=60, watts="unavailable",
                             measured_action="0", actual_pelt="2"))
        rows.append(dict(accepted=False, arm="observe", frames=10000))
        result = measure.summarize(rows)["observe"]
        self.assertEqual(result["fps"], 30)
        self.assertEqual(result["windows"], 2)
        self.assertIsNone(result["watts_mean"])

    @staticmethod
    def memory(at, faults, reads, start="55"):
        fields = ["0"] * 20
        fields[9], fields[19] = str(faults), start
        return dict(proc_stat="123 (game with spaces) " + " ".join(fields),
                    uptime=str(at), read_bytes=str(reads), swap_kb="1048576")

    def test_memory_64_bit_bytes_and_fractional_rate(self):
        before = self.memory(100, 5000, 9_000_000_000)
        after = self.memory(120, 5080, 9_010_485_760)
        result = measure.memory_delta(before, after)
        self.assertEqual(result["major_faults_per_second"], 4)
        self.assertEqual(result["read_mib_per_second"], .5)

    def test_restarted_process_and_backwards_counter_are_rejected(self):
        before = self.memory(100, 5000, 9_000_000_000)
        self.assertIsNone(measure.memory_delta(before, self.memory(120, 5080, 9_100_000_000, "56")))
        self.assertIsNone(measure.memory_delta(before, self.memory(120, 5080, 1)))


if __name__ == "__main__":
    unittest.main()
