from __future__ import annotations

import asyncio
import hashlib
import json
import os
from datetime import datetime
from pathlib import Path
from typing import Any
from zoneinfo import ZoneInfo

from fastapi import FastAPI, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

from devices.registry import DeviceRegistry

BASE_DIR = Path(__file__).resolve().parent

# Persistent TrueNAS-backed ESP32 firmware directory.
#
# Recommended container mapping:
#   /mnt/App/UniversalRemote/Firmware:/data/firmware
#
FIRMWARE_DIR = Path(os.getenv("FIRMWARE_DIR", "/app/Firmware"))
FIRMWARE_FILE = os.getenv("FIRMWARE_FILE", "universal-remote.bin")
FIRMWARE_VERSION_FILE = os.getenv("FIRMWARE_VERSION_FILE", "version.txt")

SD_FIRMWARE_DIR = FIRMWARE_DIR / "sd"
PARENTAL_CONTROLS_FILE = Path(
    os.getenv("PARENTAL_CONTROLS_FILE", str(BASE_DIR / "parental_controls.json"))
)
PARENTAL_CONTROL_INTERVAL_SECONDS = max(5, int(os.getenv("PARENTAL_CONTROL_INTERVAL_SECONDS", "30")))
PARENTAL_CONTROL_TIMEZONE = os.getenv("PARENTAL_CONTROL_TIMEZONE", "America/Chicago")
pending_remote_command: str | None = None
parental_control_task: asyncio.Task | None = None
parental_controls_lock = asyncio.Lock()

app = FastAPI(title="Universal Remote", version="0.3.0")
app.mount("/static", StaticFiles(directory=BASE_DIR / "static"), name="static")

registry = DeviceRegistry.from_yaml(BASE_DIR / "config.yaml", base_dir=BASE_DIR)


class CommandRequest(BaseModel):
    device: str
    command: str
    value: Any | None = None


class TimeWindow(BaseModel):
    start: str
    end: str


class ParentalControlRule(BaseModel):
    enabled: bool = False
    schedule: dict[str, list[TimeWindow]] = Field(default_factory=dict)


class ParentalControlsDocument(BaseModel):
    devices: dict[str, ParentalControlRule] = Field(default_factory=dict)


DAYS = (
    "monday", "tuesday", "wednesday", "thursday",
    "friday", "saturday", "sunday",
)


def _default_parental_controls() -> dict[str, Any]:
    return {"devices": {}}


def _read_parental_controls() -> dict[str, Any]:
    if not PARENTAL_CONTROLS_FILE.exists():
        return _default_parental_controls()

    try:
        raw = json.loads(PARENTAL_CONTROLS_FILE.read_text(encoding="utf-8"))
        validated = ParentalControlsDocument.model_validate(raw)
        return validated.model_dump(mode="json")
    except Exception as exc:
        print(f"Parental controls file could not be read: {exc}")
        return _default_parental_controls()


def _write_parental_controls(data: dict[str, Any]) -> None:
    PARENTAL_CONTROLS_FILE.parent.mkdir(parents=True, exist_ok=True)
    temp_path = PARENTAL_CONTROLS_FILE.with_suffix(PARENTAL_CONTROLS_FILE.suffix + ".tmp")
    temp_path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    os.replace(temp_path, PARENTAL_CONTROLS_FILE)


def _parse_hhmm(value: str) -> int:
    try:
        hour_text, minute_text = value.split(":", 1)
        hour = int(hour_text)
        minute = int(minute_text)
    except (ValueError, AttributeError) as exc:
        raise ValueError(f"Invalid time: {value}") from exc

    if not (0 <= hour <= 23 and 0 <= minute <= 59):
        raise ValueError(f"Invalid time: {value}")

    return hour * 60 + minute


def _rule_is_allowed(rule: dict[str, Any], now: datetime) -> bool:
    if not rule.get("enabled", False):
        return True

    schedule = rule.get("schedule", {})
    day_index = now.weekday()
    day_name = DAYS[day_index]
    previous_day_name = DAYS[(day_index - 1) % 7]
    minute_of_day = now.hour * 60 + now.minute

    # Normal windows and the starting portion of overnight windows.
    for window in schedule.get(day_name, []):
        start = _parse_hhmm(window["start"])
        end = _parse_hhmm(window["end"])

        if start == end:
            return True
        if start < end and start <= minute_of_day < end:
            return True
        if start > end and minute_of_day >= start:
            return True

    # After midnight, an overnight window belongs to the previous day.
    for window in schedule.get(previous_day_name, []):
        start = _parse_hhmm(window["start"])
        end = _parse_hhmm(window["end"])
        if start > end and minute_of_day < end:
            return True

    return False


async def _enforce_parental_controls_once() -> None:
    async with parental_controls_lock:
        document = _read_parental_controls()

    now = datetime.now(ZoneInfo(PARENTAL_CONTROL_TIMEZONE))

    for device_id, rule in document.get("devices", {}).items():
        if not rule.get("enabled", False) or _rule_is_allowed(rule, now):
            continue

        device = registry.devices.get(device_id)
        if not device:
            continue

        # If the driver can positively tell us the device is off, leave it alone.
        # Unknown power state still gets enforcement so streamers without a useful
        # TV power state (such as a Roku box on a dumb TV) remain controllable.
        try:
            status = await device.get_status()
            power = status.get("power") if isinstance(status, dict) else None
            if power is False or str(power).lower() == "off":
                continue
        except Exception:
            pass

        # HOME is the reliable fallback for streaming devices. POWER_OFF is
        # best-effort for devices that expose an explicit non-toggle off command.
        # HOME goes first so content stops even when POWER_OFF is unsupported.
        for action in ("home", "power_off"):
            try:
                await device.send_command(action, None)
            except Exception:
                pass


async def _parental_control_loop() -> None:
    while True:
        try:
            await _enforce_parental_controls_once()
        except asyncio.CancelledError:
            raise
        except Exception as exc:
            print(f"Parental control enforcement error: {exc}")
        await asyncio.sleep(PARENTAL_CONTROL_INTERVAL_SECONDS)


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
    volume_commands = {
        "volume_up",
        "volume_down",
        "mute",
    }

    target_device_id = (
        "roku"
        if request.command in volume_commands
        else request.device
    )

    device = registry.devices.get(target_device_id)

    if not device:
        raise HTTPException(
            status_code=404,
            detail=f"Unknown device: {target_device_id}",
        )

    try:
        result = await device.send_command(
            request.command,
            request.value,
        )
    except ValueError as exc:
        raise HTTPException(
            status_code=400,
            detail=str(exc),
        ) from exc
    except Exception as exc:
        raise HTTPException(
            status_code=502,
            detail=str(exc),
        ) from exc

    return {
        "ok": True,
        "device": target_device_id,
        "result": result,
    }



@app.get("/api/parental-controls")
async def get_parental_controls():
    async with parental_controls_lock:
        document = _read_parental_controls()

    known_devices = {item["id"]: item for item in registry.describe()}
    now = datetime.now(ZoneInfo(PARENTAL_CONTROL_TIMEZONE))

    result_devices = {}
    for device_id, device_info in known_devices.items():
        rule = document.get("devices", {}).get(device_id, {"enabled": False, "schedule": {}})
        result_devices[device_id] = {
            "name": device_info.get("name", device_id),
            "enabled": bool(rule.get("enabled", False)),
            "schedule": rule.get("schedule", {}),
            "allowed_now": _rule_is_allowed(rule, now),
        }

    return {
        "timezone": PARENTAL_CONTROL_TIMEZONE,
        "check_interval_seconds": PARENTAL_CONTROL_INTERVAL_SECONDS,
        "devices": result_devices,
    }


@app.put("/api/parental-controls/{device_id}")
async def update_parental_controls(device_id: str, rule: ParentalControlRule):
    if device_id not in registry.devices:
        raise HTTPException(status_code=404, detail=f"Unknown device: {device_id}")

    normalized_schedule: dict[str, list[dict[str, str]]] = {}
    for day, windows in rule.schedule.items():
        day_key = day.lower()
        if day_key not in DAYS:
            raise HTTPException(status_code=400, detail=f"Invalid day: {day}")

        normalized_schedule[day_key] = []
        for window in windows:
            try:
                _parse_hhmm(window.start)
                _parse_hhmm(window.end)
            except ValueError as exc:
                raise HTTPException(status_code=400, detail=str(exc)) from exc
            normalized_schedule[day_key].append({"start": window.start, "end": window.end})

    async with parental_controls_lock:
        document = _read_parental_controls()
        document.setdefault("devices", {})[device_id] = {
            "enabled": rule.enabled,
            "schedule": normalized_schedule,
        }
        _write_parental_controls(document)

    await _enforce_parental_controls_once()
    return {"ok": True, "device": device_id}


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


def _sd_manifest_hash(files: list[dict[str, Any]]) -> str:
    """
    Build a deterministic hash for the complete SD content manifest.

    The hash changes when a managed file is added, removed, renamed, resized,
    or its contents change.
    """
    digest = hashlib.sha256()

    for item in files:
        digest.update(item["path"].encode("utf-8"))
        digest.update(b"\0")
        digest.update(str(item["size"]).encode("ascii"))
        digest.update(b"\0")
        digest.update(item["sha256"].encode("ascii"))
        digest.update(b"\n")

    return digest.hexdigest()


@app.get("/api/firmware/manifest")
def firmware_manifest():
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


@app.on_event("startup")
async def startup_devices():
    global parental_control_task
    await registry.start_all()
    parental_control_task = asyncio.create_task(_parental_control_loop())


@app.on_event("shutdown")
async def shutdown_devices():
    global parental_control_task
    if parental_control_task:
        parental_control_task.cancel()
        try:
            await parental_control_task
        except asyncio.CancelledError:
            pass
        parental_control_task = None
    await registry.disconnect_all()


@app.get("/api/firmware/sd/manifest")
def sd_firmware_manifest():
    """
    Dynamically build a manifest from everything under Firmware/sd/.

    No manually maintained manifest/version file is required. The returned
    manifest_sha256 is deterministic and lets the ESP32 skip all per-file SD
    hashing when nothing on the server has changed.
    """

    # Missing/unmounted server storage is NOT treated as an authoritative empty
    # manifest. Returning an error prevents clients from deleting local content.
    if not SD_FIRMWARE_DIR.exists() or not SD_FIRMWARE_DIR.is_dir():
        raise HTTPException(
            status_code=503,
            detail="SD firmware directory is unavailable",
        )

    files: list[dict[str, Any]] = []

    for path in sorted(SD_FIRMWARE_DIR.rglob("*")):
        if not path.is_file():
            continue

        relative = path.relative_to(SD_FIRMWARE_DIR)
        relative_string = relative.as_posix()
        stat = path.stat()

        files.append({
            "path": f"/content/{relative_string}",
            "url": f"/api/firmware/sd/download/{relative_string}",
            "size": stat.st_size,
            "sha256": _sha256(path),
        })

    return {
        "valid": True,
        "manifest_sha256": _sd_manifest_hash(files),
        "files": files,
    }


@app.get("/api/firmware/sd/download/{file_path:path}")
async def sd_firmware_download(file_path: str):
    """
    Download a file from Firmware/sd/ while preventing path traversal.
    """

    root = SD_FIRMWARE_DIR.resolve()

    requested = (
        SD_FIRMWARE_DIR / file_path
    ).resolve()

    try:
        requested.relative_to(root)
    except ValueError:
        raise HTTPException(
            status_code=400,
            detail="Invalid SD file path",
        )

    if (
        not requested.exists() or
        not requested.is_file()
    ):
        raise HTTPException(
            status_code=404,
            detail="SD file not found",
        )

    return FileResponse(
        requested,
        media_type="application/octet-stream",
        filename=requested.name,
        headers={
            "Cache-Control": "no-store",
        },
    )
@app.post("/api/remote/find")
async def find_remote():
    global pending_remote_command

    pending_remote_command = "find_remote"

    return {
        "ok": True,
        "command": pending_remote_command,
    }
@app.get("/api/remote/command")
async def get_remote_command():
    global pending_remote_command

    command = pending_remote_command
    pending_remote_command = None

    return {
        "command": command
    }