from pathlib import Path
import yaml

from devices.lg_webos import LGWebOSTV
from devices.mock_lg import MockLGTV
from devices.roku import RokuDevice
from devices.onkyo import OnkyoReceiver
from devices.bazzite import BazzitePC


DRIVERS = {
    "mock_lg": MockLGTV,
    "lg_webos": LGWebOSTV,
    "roku": RokuDevice,
    "onkyo": OnkyoReceiver,
    "bazzite": BazzitePC,
}


class DeviceRegistry:
    def __init__(self, devices: dict):
        self.devices = devices

    @classmethod
    def from_yaml(cls, path: str | Path, base_dir: Path | None = None):
        path = Path(path)
        config = yaml.safe_load(path.read_text(encoding="utf-8"))
        devices = {}

        if base_dir is None:
            base_dir = path.parent

        for device_id, raw_config in config.get("devices", {}).items():
            device_config = dict(raw_config)
            device_config["_base_dir"] = str(base_dir)

            device_type = device_config.get("type")
            driver_class = DRIVERS.get(device_type)

            if driver_class is None:
                raise RuntimeError(
                    f"No driver registered for device type: {device_type}"
                )

            devices[device_id] = driver_class(
                device_id=device_id,
                name=device_config.get("name", device_id),
                config=device_config,
            )

        return cls(devices)

    def describe(self):
        return [device.describe() for device in self.devices.values()]

    async def disconnect_all(self):
        for device in self.devices.values():
            try:
                await device.disconnect()
            except Exception:
                pass
