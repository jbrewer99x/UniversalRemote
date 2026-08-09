from __future__ import annotations

import hashlib
import os
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from devices.registry import DeviceRegistry

BASE_DIR = Path(__file__).resolve().parent

# Persistent TrueNAS-backed ESP32 firmware directory.
#
# Recommended container mapping:
#   /mnt/App/UniversalRemote/Firmware:/data/firmware
#
FIRMWARE_DIR = Path(os.getenv("FIRMWARE_DIR", "/data/firmware"))
FIRMWARE_FILE = os.getenv("FIRMWARE_FILE", "universal-remote.bin")
FIRMWARE_VERSION_FILE = os.getenv("FIRMWARE_VERSION_FILE", "version.txt")

app = FastAPI(title="Universal Remote", version="0.3.0")
app.mount("/static", StaticFiles(directory=BASE_DIR / "static"), name="static")

registry = DeviceRegistry.from_yaml(BASE_DIR / "config.yaml", base_dir=BASE_DIR)


class CommandRequest(BaseModel):
    device: str
    command: str
    value: Any | None = None


@app.get("/")
async def index():
    return FileResponse(BASE_DIR / "static" / "index.html")


@app.get("/api/devices")
async def list_devices():
    return registry.describe()


@app.get("/api/status")
async def status():
    result = {}

    for device_id, device in registry.devices.items():
        try:
            result[device_id] = await device.get_status()
        except Exception as exc:
            result[device_id] = {
                "online": False,
                "error": str(exc),
            }

    return result


@app.post("/api/devices/{device_id}/connect")
async def connect_device(device_id: str):
    device = registry.devices.get(device_id)

    if not device:
        raise HTTPException(
            status_code=404,
            detail=f"Unknown device: {device_id}",
        )

    try:
        return {"ok": True, "status": await device.connect()}
    except Exception as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc


@app.post("/api/command")
async def command(request: CommandRequest):
    device = registry.devices.get(request.device)

    if not device:
        raise HTTPException(
            status_code=404,
            detail=f"Unknown device: {request.device}",
        )

    try:
        result = await device.send_command(
            request.command,
            request.value,
        )
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    return {"ok": True, "result": result}


@app.get("/api/devices/{device_id}/inputs")
async def inputs(device_id: str):
    device = registry.devices.get(device_id)

    if not device:
        raise HTTPException(
            status_code=404,
            detail=f"Unknown device: {device_id}",
        )

    try:
        return {
            "device": device_id,
            "inputs": await device.get_available_inputs(),
            "current": await device.get_input(),
        }
    except Exception as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc


@app.post("/api/devices/{device_id}/inputs/{input_id}")
async def set_input(device_id: str, input_id: str):
    device = registry.devices.get(device_id)

    if not device:
        raise HTTPException(
            status_code=404,
            detail=f"Unknown device: {device_id}",
        )

    try:
        await device.set_input(input_id)

        return {
            "ok": True,
            "device": device_id,
            "current": await device.get_input(),
        }
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc


def _firmware_path() -> Path:
    return FIRMWARE_DIR / FIRMWARE_FILE


def _version_path() -> Path:
    return FIRMWARE_DIR / FIRMWARE_VERSION_FILE


def _read_firmware_version() -> str:
    path = _version_path()

    if not path.exists():
        return "0.0.0"

    value = path.read_text(encoding="utf-8").strip()

    return value or "0.0.0"


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()

    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)

    return digest.hexdigest()


@app.get("/api/firmware/manifest")
async def firmware_manifest():
    """
    Lightweight ESP32 OTA manifest.

    Expected ESP32 behavior:
      1. GET this endpoint.
      2. Compare `version` with its compiled/current version.
      3. If newer, download `firmware_url`.
      4. Verify `sha256`.
      5. Write to the inactive OTA partition.
      6. Reboot only after verification succeeds.
    """

    firmware = _firmware_path()

    if not firmware.exists() or not firmware.is_file():
        raise HTTPException(
            status_code=404,
            detail="No ESP32 firmware has been published",
        )

    stat = firmware.stat()

    return {
        "product": "universal-remote-esp32",
        "version": _read_firmware_version(),
        "filename": firmware.name,
        "size": stat.st_size,
        "sha256": _sha256(firmware),
        "firmware_url": "/api/firmware/download",
    }


@app.get("/api/firmware/download")
async def firmware_download():
    firmware = _firmware_path()

    if not firmware.exists() or not firmware.is_file():
        raise HTTPException(
            status_code=404,
            detail="No ESP32 firmware has been published",
        )

    return FileResponse(
        firmware,
        media_type="application/octet-stream",
        filename=firmware.name,
        headers={
            "Cache-Control": "no-store",
            "X-Firmware-Version": _read_firmware_version(),
        },
    )


@app.get("/api/firmware/status")
async def firmware_status():
    """
    Human-friendly endpoint for checking the OTA repository in a browser.
    """

    firmware = _firmware_path()

    return {
        "ready": firmware.exists() and firmware.is_file(),
        "directory": str(FIRMWARE_DIR),
        "filename": FIRMWARE_FILE,
        "version": _read_firmware_version(),
    }

@app.get("/api/devices/{device_id}/status")
async def device_status(device_id: str):
    device = registry.devices.get(device_id)

    if not device:
        raise HTTPException(
            status_code=404,
            detail=f"Unknown device: {device_id}",
        )

    try:
        return await device.get_status()
    except Exception as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

@app.on_event("shutdown")
async def shutdown_devices():
    await registry.disconnect_all()
