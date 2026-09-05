"""Run: python -m unittest discover -s server/tests -v (server deps installed).
Loads real routes with an empty registry; never reads config.yaml or contacts devices.
"""
import asyncio
import hashlib
import importlib.util
import sys
import tempfile
import threading
import types
import unittest
from pathlib import Path
from unittest.mock import patch

import httpx


class EmptyRegistry:
    devices = {}

    @classmethod
    def from_yaml(cls, *args, **kwargs):
        return cls()

    def describe(self):
        return []


def load_app():
    registry = types.ModuleType("devices.registry")
    registry.DeviceRegistry = EmptyRegistry
    spec = importlib.util.spec_from_file_location(
        "manifest_test_app", Path(__file__).resolve().parents[1] / "app.py"
    )
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    with patch.dict(sys.modules, {"devices.registry": registry}):
        spec.loader.exec_module(module)
    return module


class ManifestTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.module = load_app()
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name)
        self.module.FIRMWARE_DIR = root
        self.module.SD_FIRMWARE_DIR = root / "sd"
        (root / "sd").mkdir()
        (root / self.module.FIRMWARE_FILE).write_bytes(b"test firmware")
        (root / self.module.FIRMWARE_VERSION_FILE).write_text("1.2.3")
        (root / "sd" / "sound.wav").write_bytes(b"test audio")
        self.client = httpx.AsyncClient(
            transport=httpx.ASGITransport(app=self.module.app), base_url="http://test"
        )
        self.addAsyncCleanup(self.client.aclose)

    async def test_firmware_manifest_contract(self):
        response = await self.client.get("/api/firmware/manifest")
        self.assertEqual(response.status_code, 200)
        data = response.json()
        self.assertEqual(data["version"], "1.2.3")
        self.assertEqual(data["size"], len(b"test firmware"))
        self.assertEqual(data["sha256"], hashlib.sha256(b"test firmware").hexdigest())
        self.assertEqual(data["firmware_url"], "/api/firmware/download")

    async def test_sd_manifest_contract_and_stability(self):
        first = (await self.client.get("/api/firmware/sd/manifest")).json()
        second = (await self.client.get("/api/firmware/sd/manifest")).json()
        self.assertTrue(first["valid"])
        self.assertEqual(first, second)
        self.assertEqual(first["files"], [{
            "path": "/content/sound.wav",
            "url": "/api/firmware/sd/download/sound.wav",
            "size": len(b"test audio"),
            "sha256": hashlib.sha256(b"test audio").hexdigest(),
        }])
        (self.module.SD_FIRMWARE_DIR / "sound.wav").write_bytes(b"changed")
        changed = (await self.client.get("/api/firmware/sd/manifest")).json()
        self.assertNotEqual(first["manifest_sha256"], changed["manifest_sha256"])

    async def test_missing_storage_is_not_authoritative_empty_manifest(self):
        self.module.SD_FIRMWARE_DIR /= "unmounted"
        response = await self.client.get("/api/firmware/sd/manifest")
        self.assertEqual(response.status_code, 503)
        self.assertNotIn("files", response.json())

    async def test_command_poll_works_while_each_manifest_is_hashing(self):
        for route in ("/api/firmware/manifest", "/api/firmware/sd/manifest"):
            with self.subTest(route=route):
                started, release = threading.Event(), threading.Event()
                original = self.module._sha256

                def slow_hash(path):
                    started.set()
                    if not release.wait(3):
                        raise RuntimeError("Hashing blocked the event loop")
                    return original(path)

                with patch.object(self.module, "_sha256", slow_hash):
                    request = asyncio.create_task(self.client.get(route))
                    try:
                        self.assertTrue(await asyncio.to_thread(started.wait, 2))
                        queued = await asyncio.wait_for(
                            self.client.post("/api/remote/find"), 1
                        )
                        polled = await asyncio.wait_for(
                            self.client.get("/api/remote/command"), 1
                        )
                        self.assertEqual(queued.status_code, 200)
                        self.assertEqual(polled.json(), {"command": "find_remote"})
                        self.assertFalse(request.done())
                    finally:
                        release.set()
                        response = await request
                    self.assertEqual(response.status_code, 200)


if __name__ == "__main__":
    unittest.main()
