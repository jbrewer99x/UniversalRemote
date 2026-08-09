from typing import Any
from devices.base import RemoteDevice


class MockLGTV(RemoteDevice):
    def __init__(self, device_id: str, name: str, config: dict):
        super().__init__(device_id, name, config)
        raw_inputs = config.get(
            "inputs",
            ["HDMI 1", "HDMI 2", "HDMI 3", "Live TV"],
        )
        self.inputs = [
            {"id": name.replace(" ", "_").upper(), "label": name}
            for name in raw_inputs
        ]
        self.power = True
        self.current_input = self.inputs[0]["id"] if self.inputs else None
        self.volume = 20
        self.muted = False

    async def send_command(self, command: str, value: Any | None = None):
        if command == "power":
            self.power = not self.power
        elif command == "power_on":
            self.power = True
        elif command == "power_off":
            self.power = False
        elif command == "volume_up":
            self.volume = min(100, self.volume + 1)
        elif command == "volume_down":
            self.volume = max(0, self.volume - 1)
        elif command == "mute":
            self.muted = not self.muted
        elif command in {
            "up", "down", "left", "right", "ok",
            "back", "home", "play", "pause"
        }:
            pass
        else:
            raise ValueError(f"Unsupported command: {command}")

        return await self.get_status()

    async def get_status(self) -> dict:
        return {
            "online": True,
            "paired": True,
            "power": self.power,
            "input": self.current_input,
            "input_label": self._label_for(self.current_input),
            "volume": self.volume,
            "muted": self.muted,
        }

    async def get_available_inputs(self) -> list[dict]:
        return self.inputs

    async def get_input(self) -> str | None:
        return self.current_input

    async def set_input(self, input_id: str):
        valid = {entry["id"] for entry in self.inputs}
        if input_id not in valid:
            raise ValueError(
                f"Unknown input '{input_id}'. Available inputs: {', '.join(sorted(valid))}"
            )
        self.current_input = input_id

    def _label_for(self, input_id: str | None) -> str | None:
        for item in self.inputs:
            if item["id"] == input_id:
                return item["label"]
        return input_id
