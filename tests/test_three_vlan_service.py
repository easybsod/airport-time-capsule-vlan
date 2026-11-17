"""Exercise the real POSIX service with simulated hardware and short deadlines.

No root, network namespace, SRAB, real network configuration, or reboot.
Every detached process is confined to an owned process group and terminated.
"""
import os
from pathlib import Path
import signal
import subprocess
import tempfile
import time
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "scripts/airport-three-vlan-service.sh"


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.base = self.root / "flash"
        self.run = self.root / "ram"
        self.bin = self.root / "bin"
        for path in (self.base, self.run, self.bin):
            path.mkdir()
        self.groups = []
        self.script = self.base / "service.sh"
        text = SOURCE.read_text().replace("/mnt/Flash/airport-three-vlan", str(self.base))
        text = text.replace("/mnt/Memory/airport-three-vlan", str(self.run))
        text = text.replace("/mnt/Flash/airport-srab-lab/armed", str(self.root / "lab-armed"))
        for name in ("brconfig", "ifconfig", "sysctl", "reboot"):
            text = text.replace("/sbin/" + name, str(self.bin / name))
        text = text.replace("/usr/bin/netstat", str(self.bin / "netstat"))
        text = text.replace("/bin/sleep", str(self.bin / "sleep"))
        self.script.write_text(text)
        self.state = self.base / "hardware-state"
        self.state.write_text("baseline")
        (self.base / "enabled").touch()
        (self.base / "trial").touch()
        self.executable("sleep", "#!/usr/bin/env python3\nimport sys,time\ntime.sleep(float(sys.argv[1])*0.02)\n")
        self.executable("brconfig", """#!/bin/sh
[ ! -f 'BASE/fault-prerequisite' ] || exit 1
case $1 in
bridge0) printf '\tbcmeth0 flags=3\n\twlan0 flags=3\n\twlan2 flags=3\n';;
bridge1) printf '\tvlan0 flags=3\n\twlan1 flags=3\n\twlan3 flags=3\n';;
esac
""".replace("BASE", str(self.base)))
        self.executable("ifconfig", """#!/bin/sh
case $1 in
vlan0) echo 'vlan: 1003 parent interface: bcmeth0';;
bridge0) echo 'inet 192.0.2.10 netmask 0xffffff00';;
esac
""")
        self.executable("netstat", "#!/bin/sh\necho 'default 192.0.2.1 UGS 0 0 - bridge0'\n")
        self.executable("sysctl", "#!/bin/sh\necho 'kern.boottime = simulated boot'\n")
        self.executable("reboot", "#!/bin/sh\n[ -f 'BASE/disabled' ] || exit 99\ntouch 'BASE/reboot-requested'\n".replace("BASE", str(self.base)))
        tool = """#!/usr/bin/env python3
import pathlib,sys
b=pathlib.Path(BASE)
p=b/'hardware-state'
command=sys.argv[1]
if command=='abi':
 print('AIRPORT_THREE_VLAN_ABI=1 config=strict-8021q-v1 read=0xc0106d02 write=0x80106d03 size=16',file=sys.stderr)
 sys.exit(0)
if command=='verify': sys.exit(0 if p.read_text()=='active' else 1)
if command=='baseline': sys.exit(0 if p.read_text()=='baseline' else 1)
if command=='apply':
 with (b/'apply-count').open('a') as out: out.write('apply\\n')
 p.write_text('active')
 sys.exit(1 if (b/'fault-apply').exists() else 0)
if command=='restore':
 if (b/'fault-restore').exists(): sys.exit(1)
 p.write_text('baseline')
 sys.exit(0)
sys.exit(2)
""".replace("BASE", repr(str(self.base)))
        self.executable("switch-config", tool, self.base)

    def executable(self, name, contents, directory=None):
        path = (directory or self.bin) / name
        path.write_text(contents)
        path.chmod(0o700)

    def tearDown(self):
        for group in self.groups:
            try:
                os.killpg(group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        self.temp.cleanup()

    def command(self, action):
        return subprocess.run(["/bin/sh", str(self.script), action], capture_output=True, text=True, timeout=5)

    def start(self):
        process = subprocess.Popen(["/bin/sh", str(self.script), "start"], stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, start_new_session=True)
        self.groups.append(process.pid)
        stdout, stderr = process.communicate(timeout=5)
        self.assertEqual(process.returncode, 0, stdout + stderr)

    def until(self, predicate, timeout=9):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if predicate():
                return
            time.sleep(0.025)
        logs = (self.base / "events.log").read_text() if (self.base / "events.log").exists() else ""
        self.fail("condition not reached; " + logs)

    def acknowledged(self, action="ack"):
        self.until(lambda: (self.run / "ready").exists())
        result = self.command(action)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.until(lambda: not (self.run / "armed").exists())

    def test_trial_commit_reapply_and_automatic_next_boot(self):
        self.start()
        self.acknowledged("commit")
        self.assertFalse((self.base / "trial").exists())
        self.assertEqual(self.state.read_text(), "active")
        self.assertIn("MANAGER_LIVE_PID=", self.command("status").stdout)
        # Simulate an ACPd switch reset: the monitor must restore the split.
        self.state.write_text("baseline")
        self.until(lambda: self.state.read_text() == "active")
        self.until(lambda: not (self.run / "armed").exists())
        self.assertEqual((self.base / "apply-count").read_text().count("apply"), 2)
        # A real reboot clears RAM and resets the ASIC. Keep Flash only.
        pid = int((self.run / "manager.pid").read_text())
        os.kill(pid, signal.SIGTERM)
        time.sleep(0.1)
        for group in self.groups:
            try:
                os.killpg(group, signal.SIGKILL)
            except ProcessLookupError:
                pass
        import shutil
        shutil.rmtree(self.run)
        self.run.mkdir()
        self.state.write_text("baseline")
        self.start()
        self.until(lambda: (self.run / "last-good").exists())
        self.assertEqual(self.state.read_text(), "active")
        self.assertFalse((self.base / "disabled").exists())
        self.assertFalse((self.base / "reboot-requested").exists())

    def test_no_external_ack_rolls_back_and_disables(self):
        self.start()
        self.until(lambda: (self.run / "ready").exists())
        self.until(lambda: (self.base / "disabled").exists())
        self.until(lambda: self.state.read_text() == "baseline")
        self.until(lambda: not (self.run / "armed").exists())
        count = (self.base / "apply-count").read_text()
        self.assertEqual(self.command("start").returncode, 0)
        self.assertEqual((self.base / "apply-count").read_text(), count)
        self.assertFalse((self.base / "reboot-requested").exists())

    def test_apply_failure_restores_without_reboot(self):
        (self.base / "fault-apply").touch()
        self.start()
        self.until(lambda: (self.base / "disabled").exists())
        self.until(lambda: self.state.read_text() == "baseline")
        self.until(lambda: not (self.run / "armed").exists())
        self.assertFalse((self.base / "reboot-requested").exists())

    def test_failed_restore_has_one_shot_reboot_and_no_retry(self):
        (self.base / "fault-apply").touch()
        (self.base / "fault-restore").touch()
        self.start()
        self.until(lambda: (self.base / "reboot-requested").exists())
        self.assertTrue((self.base / "disabled").exists())
        count = (self.base / "apply-count").read_text()
        self.assertEqual(self.command("start").returncode, 0)
        self.assertEqual((self.base / "apply-count").read_text(), count)

    def test_missing_original_bridges_never_applies(self):
        (self.base / "fault-prerequisite").touch()
        self.start()
        self.until(lambda: (self.base / "disabled").exists())
        self.until(lambda: not (self.run / "armed").exists())
        self.assertFalse((self.base / "apply-count").exists())
        self.assertEqual(self.state.read_text(), "baseline")


if __name__ == "__main__":
    unittest.main()
