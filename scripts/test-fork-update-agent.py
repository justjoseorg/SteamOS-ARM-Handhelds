#!/usr/bin/env python3
"""Check update-family isolation and preservation of the 1.5 kernel."""

import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("configure", ROOT / "fork/configure-update-agent.py")
configure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(configure)
AGENT = ROOT / "steamos-overlay/usr/lib/steamos-arm/update-agent"


def load_agent(path):
    loader = importlib.machinery.SourceFileLoader("test_agent", str(path))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    agent = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(agent)
    return agent


class AgentTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.path = self.root / "usr/lib/steamos-arm/update-agent"
        self.path.parent.mkdir(parents=True)
        self.path.write_bytes(AGENT.read_bytes())
        self.metadata = self.root / "usr/share/steamos-arm/update/installed.json"
        self.metadata.parent.mkdir(parents=True)

    def setup_version(self, version):
        self.metadata.write_text(json.dumps({"version": version}))
        return configure.configure(self.root, "fork/repo")

    def test_modern_names_are_invisible_to_legacy_agents(self):
        legacy = load_agent(AGENT)
        self.assertIsNone(legacy.NAME.match("zaviya-arm-sm8550-1.5.1.sau"))
        self.assertIsNone(legacy.PART.match("zaviya-arm-sm8550-1.5.1.sau.001"))
        self.assertEqual(self.setup_version("1.5.0"), "zaviya-arm")
        modern = load_agent(self.path)
        self.assertEqual(modern.REPO, "fork/repo")
        self.assertIsNotNone(modern.NAME.match("zaviya-arm-sm8550-1.5.1.sau"))
        self.assertIsNone(modern.NAME.match("steamos-arm-sm8550-1.4.8.sau"))
        self.assertEqual(modern.RUN, legacy.RUN)
        self.assertEqual(modern.CACHE, legacy.CACHE)
        self.assertEqual(
            modern.split_sets(["zaviya-arm-sm8550-1.5.1.sau.001",
                               "zaviya-arm-sm8550-1.5.1.sau.002"], "sm8550"),
            {"zaviya-arm-sm8550-1.5.1.sau": [
                "zaviya-arm-sm8550-1.5.1.sau.001", "zaviya-arm-sm8550-1.5.1.sau.002"]})
        before = self.path.read_text()
        self.setup_version("1.5.0")
        self.assertEqual(self.path.read_text(), before)

    def test_legacy_device_stays_in_legacy_family(self):
        self.assertEqual(self.setup_version("1.4.8"), "steamos-arm")
        agent = load_agent(self.path)
        self.assertIsNotNone(agent.NAME.match("steamos-arm-sm8550-1.4.9.sau"))
        self.assertIsNone(agent.NAME.match("zaviya-arm-sm8550-1.5.1.sau"))

    def test_unknown_device_version_fails_without_modifying_agent(self):
        self.metadata.write_text("{}")
        before = self.path.read_bytes()
        with self.assertRaises(KeyError):
            configure.configure(self.root, "fork/repo")
        self.assertEqual(self.path.read_bytes(), before)

    def test_modern_hook_keeps_kernel_and_gyro_stack(self):
        assets = self.root / "assets"
        assets.mkdir()
        (assets / "inputplumber").write_text("new inputplumber")
        (self.root / "usr/bin").mkdir()
        kernel = self.root / "KERNEL"
        kernel.write_text("upstream 7.4 kernel")
        result = subprocess.run(
            ["bash", str(ROOT / "fork/hooks/10-gyro-sm8550.sh")],
            env={**os.environ, "SOC": "sm8550", "ROOTFS": str(self.root),
                 "FORK_ASSETS": str(assets), "FORK_UPSTREAM_KERNEL": "1",
                 "BOOT_KERNEL": str(kernel)}, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.root / "usr/bin/inputplumber").read_text(), "new inputplumber")
        self.assertEqual(kernel.read_text(), "upstream 7.4 kernel")


if __name__ == "__main__":
    unittest.main()
