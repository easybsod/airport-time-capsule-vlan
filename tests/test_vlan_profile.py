"""Offline tests of topology-driven values; never open a network device."""
from copy import deepcopy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("vlan_profile", ROOT / "scripts/plan-vlan-profile.py")
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def load(name):
    return json.loads((ROOT / "examples" / name).read_text())


class VlanProfileTests(unittest.TestCase):
    def setUp(self):
        self.profile = load("two-access-vlans.json")
        self.baseline = load("synthetic-baseline.json")

    def test_multiple_access_ports_and_derived_masks(self):
        plan = MODULE.make_plan(self.profile, self.baseline)
        tables = {v["vid"]: v for v in plan["target"]["vlans"]}
        ports = {p["port"]: p for p in plan["target"]["ports"]}
        self.assertEqual(tables[200]["entry"], 0x00200209)
        self.assertEqual(tables[300]["entry"], 0x0020040A)
        self.assertEqual(tables[18]["entry"], 0x0022190C)
        self.assertEqual(ports[0]["pvlan_mask"], 0x009)
        self.assertEqual(ports[1]["pvlan_mask"], 0x00A)
        self.assertEqual(ports[8]["pvlan_mask"], 0x10C)
        self.assertEqual(plan["cpu_tagged_vids"], [1003])
        self.assertFalse(plan["deployable"])
        self.assertFalse(plan["hardware_tested"])
        self.assertTrue(plan["changes"])
        self.assertEqual(plan["baseline_kind"], "synthetic")
        self.assertEqual(plan["unmanaged_ports"], [5])
        self.assertNotIn("preserved_unmanaged_ports", plan)
        self.assertTrue(any("global policies" in item for item in plan["limitations"]))

    def test_cpu_and_external_trunk_membership(self):
        plan = MODULE.make_plan(load("cpu-tagged-vlan.json"), self.baseline)
        tables = {v["vid"]: v for v in plan["target"]["vlans"]}
        self.assertEqual(tables[400]["entry"], 0x0020010A)
        self.assertEqual(plan["cpu_tagged_vids"], [400, 1003])
        self.assertEqual(tables[400]["untagged"], 0)

    def test_vid_and_trunk_are_not_fixed(self):
        profile = deepcopy(self.profile)
        profile["vlans"][1] = {"vid": 237, "members": [0, 1], "untagged": [0]}
        profile["vlans"][2] = {"vid": 801, "members": [1, 3], "untagged": [3]}
        profile["ports"][0]["pvid"] = 237
        profile["ports"][1]["pvid"] = 801
        profile["ports"][3]["pvid"] = 801
        profile["vlans"][0]["vid"] = 21
        profile["vlans"] = profile["vlans"][:3]
        for port in profile["ports"]:
            if port["pvid"] == 18:
                port["pvid"] = 21
        plan = MODULE.make_plan(profile)
        tables = {v["vid"]: v for v in plan["target"]["vlans"]}
        self.assertEqual(set(tables), {21, 237, 801})
        self.assertEqual(tables[237]["members"], 3)
        self.assertEqual(tables[801]["untagged"], 8)
        self.assertIsNone(plan["changes"])

    def test_register_selector_is_calculated_per_port(self):
        plan = MODULE.make_plan(self.profile)
        registers = {r["name"]: r for r in plan["target"]["registers"]}
        self.assertEqual(registers["port0_pvid"]["selector"], "0x34100001")
        self.assertEqual(registers["port1_mask"]["selector"], "0x31020001")
        self.assertEqual(registers["port8_pvid"]["selector"], "0x34200001")

    def test_original_validated_case_values_are_reproducible(self):
        profile = deepcopy(self.profile)
        profile["vlans"] = [
            {"vid": 18, "members": [0, 1, 3, 8], "untagged": [0, 1, 3, 8]},
            {"vid": 1002, "members": [2, 3], "untagged": [2]},
            {"vid": 1003, "members": [3, 8], "untagged": []},
        ]
        profile["ports"] = [{"port": p, "pvid": 1002 if p == 2 else 18,
                             "pvlan_mask": 0xC if p == 2 else 0x10F} for p in sorted(MODULE.PORTS)]
        plan = MODULE.make_plan(profile)
        self.assertEqual({v["vid"]: v["entry"] for v in plan["target"]["vlans"]},
                         {18: 0x0022170B, 1002: 0x0020080C, 1003: 0x00200108})

    def test_missing_baseline_vid_does_not_assume_zero(self):
        self.baseline["vlans"] = [v for v in self.baseline["vlans"] if v["vid"] != 200]
        with self.assertRaisesRegex(ValueError, "zero is never assumed"):
            MODULE.make_plan(self.profile, self.baseline)

    def test_nonzero_baseline_vid_requires_explicit_retention_or_removal(self):
        self.baseline["vlans"].append({"vid": 777, "entry": 0x00200108})
        with self.assertRaisesRegex(ValueError, "explicitly removed"):
            MODULE.make_plan(self.profile, self.baseline)
        self.profile["remove_vids"] = [777]
        plan = MODULE.make_plan(self.profile, self.baseline)
        change = next(c for c in plan["changes"] if c.get("vid") == 777)
        self.assertEqual((change["before"], change["after"]), (0x00200108, 0))

    def test_invalid_vid_port_and_nonmember_untag_are_rejected(self):
        for value in (0, 4095, True, -1):
            with self.subTest(vid=value):
                profile = deepcopy(self.profile)
                profile["vlans"][1]["vid"] = value
                with self.assertRaises(ValueError):
                    MODULE.make_plan(profile)
        for members, untag in (([0, 4], [0]), ([0, 0], [0]), ([0, 3], [1])):
            with self.subTest(members=members, untagged=untag):
                profile = deepcopy(self.profile)
                profile["vlans"][1].update(members=members, untagged=untag)
                with self.assertRaises(ValueError):
                    MODULE.make_plan(profile)

    def test_missing_port_or_pvid_membership_is_rejected(self):
        self.profile["ports"].pop()
        with self.assertRaisesRegex(ValueError, "all managed"):
            MODULE.make_plan(self.profile)
        self.profile = load("two-access-vlans.json")
        self.profile["ports"][0]["pvid"] = 300
        with self.assertRaisesRegex(ValueError, "PVID"):
            MODULE.make_plan(self.profile)

    def test_mask_and_configuration_typos_are_rejected(self):
        self.profile["ports"][0]["pvlan_mask"] = 1
        with self.assertRaisesRegex(ValueError, "all VLAN peers"):
            MODULE.make_plan(self.profile)
        self.profile = load("two-access-vlans.json")
        self.profile["vlans"][0]["untagged_ports"] = [2]
        with self.assertRaisesRegex(ValueError, "unknown keys"):
            MODULE.make_plan(self.profile)

    def test_egress_asymmetry_is_reported_without_claiming_tag_admission(self):
        self.profile["vlans"][1]["untagged"] = []
        plan = MODULE.make_plan(self.profile)
        self.assertTrue(any("port 0" in warning for warning in plan["warnings"]))
        self.assertFalse(plan["hardware_tested"])

    def test_cli_emits_only_offline_json(self):
        result = subprocess.run([sys.executable, str(ROOT / "scripts/plan-vlan-profile.py"),
                                 str(ROOT / "examples/cpu-tagged-vlan.json")],
                                text=True, capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        plan = json.loads(result.stdout)
        self.assertEqual(plan["kind"], "offline_vlan_design")
        self.assertFalse(plan["deployable"])

    def test_duplicate_profile_or_baseline_keys_fail_without_output(self):
        with tempfile.TemporaryDirectory() as directory:
            input_path = Path(directory) / "input.json"
            output_path = Path(directory) / "output.json"
            cases = [
                (json.dumps(self.profile).replace('"pvid": 200', '"pvid": 300, "pvid": 200'), False),
                (json.dumps(self.baseline).replace('"control4": "0xc4"',
                                                  '"control4": "0x40", "control4": "0xc4"'), True),
            ]
            for text, is_baseline in cases:
                with self.subTest(baseline=is_baseline):
                    input_path.write_text(text)
                    command = [sys.executable, str(ROOT / "scripts/plan-vlan-profile.py")]
                    if is_baseline:
                        command += [str(ROOT / "examples/two-access-vlans.json"), "--baseline", str(input_path)]
                    else:
                        command += [str(input_path)]
                    result = subprocess.run(command + ["--output", str(output_path)],
                                            text=True, capture_output=True, timeout=5)
                    self.assertEqual(result.returncode, 2)
                    self.assertIn("duplicate JSON key", result.stderr)
                    self.assertEqual(result.stdout, "")
                    self.assertFalse(output_path.exists())

    def test_nonstandard_json_constants_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "profile.json"
            for constant in ("NaN", "Infinity", "-Infinity"):
                with self.subTest(constant=constant):
                    path.write_text('{"unused": ' + constant + '}')
                    with self.assertRaisesRegex(ValueError, "nonstandard JSON constant"):
                        MODULE.read_json(path)


if __name__ == "__main__":
    unittest.main()
