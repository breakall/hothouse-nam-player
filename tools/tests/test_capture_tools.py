import json
import math
import pathlib
import sys
import tempfile
import unittest


TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import a2_capture
import install_capture


def valid_a2():
    inactive = {"active": False}
    config = {
        "input_size": 1,
        "condition_size": 1,
        "channels": 3,
        "bottleneck": 3,
        "kernel_sizes": a2_capture.KERNEL_SIZES,
        "dilations": a2_capture.DILATIONS,
        "head": {"out_channels": 1, "kernel_size": 16, "bias": True},
        "layer1x1": {"active": True, "groups": 1},
        "head1x1": inactive,
        "groups_input": 1,
        "groups_input_mixin": 1,
        "activation": [
            {"type": "LeakyReLU", "negative_slope": 0.01}
            for _ in range(23)
        ],
        "gating_mode": ["none"] * 23,
        "secondary_activation": [None] * 23,
    }
    for key in (
        "conv_pre_film", "conv_post_film", "input_mixin_pre_film",
        "input_mixin_post_film", "activation_pre_film",
        "activation_post_film", "layer1x1_post_film", "head1x1_post_film",
    ):
        config[key] = inactive
    return {
        "architecture": "WaveNet",
        "sample_rate": 48000,
        "config": {"layers": [config]},
        "weights": [0.0] * a2_capture.WEIGHT_COUNT,
    }


class CaptureValidationTests(unittest.TestCase):
    def test_a2_selects_later_compatible_submodel(self):
        incompatible = valid_a2()
        incompatible["config"]["layers"][0]["channels"] = 8
        container = {
            "architecture": "SlimmableContainer",
            "config": {"submodels": [
                {"model": incompatible},
                {"model": valid_a2()},
            ]},
        }
        selected, index = a2_capture.select_a2_lite(container)
        self.assertEqual(index, 1)
        self.assertEqual(len(a2_capture.validate_model(selected)), 1871)

    def test_a2_rejects_nonfinite_weights(self):
        model = valid_a2()
        model["weights"][10] = math.nan
        with self.assertRaisesRegex(ValueError, "finite"):
            a2_capture.validate_model(model)

    def test_prepare_a2_packs_weights_and_uses_metadata_name(self):
        model = valid_a2()
        model["metadata"] = {"name": " Test   Amp "}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "capture.nam"
            path.write_text(json.dumps(model), encoding="utf-8")
            capture_format, name, payload = install_capture.prepare_capture(
                "a2_lite", path
            )
        self.assertEqual(capture_format, "a2_weights_f32")
        self.assertEqual(name, "Test Amp")
        self.assertEqual(len(payload), 1871 * 4)

    def test_non_nam_input_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "capture.bin"
            path.write_bytes(b"wrong")
            with self.assertRaisesRegex(install_capture.ProtocolError, "accepts .nam"):
                install_capture.prepare_capture("a2_lite", path)

    def test_unknown_backend_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "capture.nam"
            path.write_text(json.dumps(valid_a2()), encoding="utf-8")
            with self.assertRaisesRegex(install_capture.ProtocolError, "Unsupported"):
                install_capture.prepare_capture("unknown", path)

    def test_names_decode_strict_ascii(self):
        self.assertEqual(install_capture.decode_name_hex("54657374"), "Test")
        for encoded in ("xyz", "ff"):
            with self.assertRaises(install_capture.ProtocolError):
                install_capture.decode_name_hex(encoded)


class FakePort:
    def __init__(self, fail_data=False):
        self.commands = []
        self.fail_data = fail_data

    def request(self, command, timeout=0):
        self.commands.append(command)
        if command.startswith("HNAM BEGIN"):
            return ["BEGIN"]
        if command.startswith("HNAM DATA"):
            if self.fail_data:
                raise install_capture.ProtocolError("test failure")
            fields = command.split()
            return ["DATA", str(int(fields[2]) + len(bytes.fromhex(fields[3])))]
        if command == "HNAM COMMIT":
            return ["COMMIT"]
        if command == "HNAM CANCEL":
            return ["CANCEL"]
        raise AssertionError(command)


class TransferTests(unittest.TestCase):
    def write_model(self, directory):
        path = pathlib.Path(directory) / "capture.nam"
        path.write_text(json.dumps(valid_a2()), encoding="utf-8")
        return path

    def test_install_addresses_requested_slot_and_commits(self):
        with tempfile.TemporaryDirectory() as directory:
            port = FakePort()
            install_capture.install(port, "a2_lite", self.write_model(directory), "C")
        self.assertTrue(port.commands[0].startswith("HNAM BEGIN C a2_weights_f32"))
        self.assertEqual(port.commands[-1], "HNAM COMMIT")
        data_commands = [command for command in port.commands if command.startswith("HNAM DATA")]
        self.assertGreater(len(data_commands), 1)

    def test_install_cancels_failed_transfer(self):
        with tempfile.TemporaryDirectory() as directory:
            port = FakePort(fail_data=True)
            with self.assertRaises(install_capture.ProtocolError):
                install_capture.install(port, "a2_lite", self.write_model(directory), "A")
        self.assertEqual(port.commands[-1], "HNAM CANCEL")


if __name__ == "__main__":
    unittest.main()
