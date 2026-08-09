from abc import ABC, abstractmethod
from typing import Any


class RemoteDevice(ABC):
    def __init__(self, device_id: str, name: str, config: dict):
        self.device_id = device_id
        self.name = name
        self.config = config

    async def connect(self) -> dict:
        return await self.get_status()

    async def disconnect(self):
        return None

    @abstractmethod
    async def send_command(self, command: str, value: Any | None = None):
        raise NotImplementedError

    @abstractmethod
    async def get_status(self) -> dict:
        raise NotImplementedError

    async def get_available_inputs(self) -> list[dict]:
        return []

    async def get_input(self) -> str | None:
        return None

    async def set_input(self, input_id: str):
        raise ValueError(f"{self.name} does not support input selection")

    def describe(self) -> dict:
        return {
            "id": self.device_id,
            "name": self.name,
            "type": self.config.get("type"),
        }
