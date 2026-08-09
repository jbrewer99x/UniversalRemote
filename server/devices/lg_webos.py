import asyncio
import socket
from pathlib import Path
from typing import Any

from bscpylgtv import WebOsClient

from devices.base import RemoteDevice


class LGWebOSTV(RemoteDevice):
    """
    LG webOS network driver using bscpylgtv.

    The first successful connect() may cause the TV to show a pairing prompt.
    bscpylgtv stores the resulting client key in the configured SQLite file.
    """

    BUTTON_MAP = {
        "up": "UP",
        "down": "DOWN",
        "left": "LEFT",
        "right": "RIGHT",
        "ok": "ENTER",
        "back": "BACK",
        "home": "HOME",
        "play": "PLAY",
        "pause": "PAUSE",
    }

    def __init__(self, device_id: str, name: str, config: dict):
        super().__init__(device_id, name, config)
        self.host = str(config["host"])
        self.without_ssl = bool(config.get("without_ssl", True))
        self.mac = (config.get("mac") or "").strip()

        key_file = Path(config.get("key_file", "data/lg_pairing.sqlite"))
        base_dir = Path(config.get("_base_dir", "."))
        if not key_file.is_absolute():
            key_file = base_dir / key_file
        key_file.parent.mkdir(parents=True, exist_ok=True)
        self.key_file = key_file

        self._client = None
        self._client_lock = asyncio.Lock()

    async def _get_client(self) -> WebOsClient:
        if self._client is not None:
            return self._client

        async with self._client_lock:
            if self._client is None:
                self._client = await WebOsClient.create(
                    self.host,
                    key_file_path=str(self.key_file),
                    without_ssl=self.without_ssl,
                    ping_interval=5,
                    ping_timeout=10,
                    timeout_connect=2,
                    connect_retry_attempts=3,
                    states=[
                        "power",
                        "current_app",
                        "muted",
                        "volume",
                        "inputs",
                    ],
                )
        return self._client

    async def _ensure_connected(self) -> WebOsClient:
        client = await self._get_client()
        if not client.is_connected():
            await client.connect()
        return client

    async def connect(self) -> dict:
        client = await self._ensure_connected()
        return await self._status_from_client(client)

    async def disconnect(self):
        if self._client is not None and self._client.is_connected():
            await self._client.disconnect()

    async def send_command(self, command: str, value: Any | None = None):
        if command in {"power_on", "wake"}:
            self._wake()
            return {
                "online": False,
                "wake_sent": True,
                "message": "Wake-on-LAN packet sent.",
            }

        client = await self._ensure_connected()

        if command == "power":
            await client.power_off()
            return {"online": False, "power": False}

        if command == "power_off":
            await client.power_off()
            return {"online": False, "power": False}

        if command == "volume_up":
            await client.volume_up()
        elif command == "volume_down":
            await client.volume_down()
        elif command == "mute":
            muted = await client.get_muted()
            await client.set_mute(not bool(muted))
        elif command in self.BUTTON_MAP:
            await client.button(self.BUTTON_MAP[command])
        else:
            raise ValueError(f"Unsupported LG command: {command}")

        return await self._status_from_client(client)

    async def get_status(self) -> dict:
        client = await self._get_client()

        # Do not trigger a pairing prompt merely because the browser polls status.
        if not client.is_registered():
            return {
                "online": False,
                "paired": False,
                "power": None,
                "input": None,
                "input_label": None,
                "volume": None,
                "muted": None,
                "message": "Click Pair / Connect and approve the prompt on the TV.",
            }

        try:
            if not client.is_connected():
                await client.connect()
            return await self._status_from_client(client)
        except Exception as exc:
            return {
                "online": False,
                "paired": True,
                "power": False,
                "input": None,
                "input_label": None,
                "volume": None,
                "muted": None,
                "error": str(exc),
            }

    async def _status_from_client(self, client: WebOsClient) -> dict:
        current = await client.get_input()
        inputs = await self._read_inputs(client)

        power = None
        try:
            power_payload = await client.get_power_state()
            state = str(power_payload.get("state", "")).lower()
            power = state in {"active", "on"}
        except Exception:
            power = True if client.is_connected() else None

        volume = await client.get_volume()
        muted = await client.get_muted()

        return {
            "online": client.is_connected(),
            "paired": client.is_registered(),
            "power": power,
            "input": current,
            "input_label": self._input_label(inputs, current),
            "volume": volume,
            "muted": muted,
        }

    async def _read_inputs(self, client: WebOsClient) -> list[dict]:
        raw_inputs = await client.get_inputs()
        result = []

        for entry in raw_inputs or []:
            input_id = entry.get("id") or entry.get("appid")
            if not input_id:
                continue

            # webOS generally exposes a human-readable label/name alongside appId.
            label = (
                entry.get("label")
                or entry.get("name")
                or entry.get("deviceName")
                or input_id.replace("_", " ")
            )

            result.append({
                "id": input_id,
                "label": label,
                "connected": entry.get("connected"),
            })

        return result

    async def get_available_inputs(self) -> list[dict]:
        client = await self._get_client()
        if not client.is_registered():
            return []
        client = await self._ensure_connected()
        return await self._read_inputs(client)

    async def get_input(self) -> str | None:
        client = await self._get_client()
        if not client.is_registered():
            return None
        client = await self._ensure_connected()
        return await client.get_input()

    async def set_input(self, input_id: str):
        client = await self._ensure_connected()

        valid_inputs = {item["id"] for item in await self._read_inputs(client)}
        if valid_inputs and input_id not in valid_inputs:
            raise ValueError(
                f"Unknown LG input '{input_id}'. Available inputs: "
                + ", ".join(sorted(valid_inputs))
            )

        await client.set_input(input_id)

    def _input_label(self, inputs: list[dict], input_id: str | None) -> str | None:
        for item in inputs:
            if item["id"] == input_id:
                return item["label"]
        return input_id

    def _wake(self):
        if not self.mac:
            raise ValueError(
                "Power-on requires the TV MAC address. Add 'mac:' in config.yaml "
                "after enabling LG's network/mobile TV power-on setting."
            )

        mac = self.mac.replace(":", "").replace("-", "").replace(".", "")
        if len(mac) != 12:
            raise ValueError("Invalid MAC address in config.yaml")

        try:
            mac_bytes = bytes.fromhex(mac)
        except ValueError as exc:
            raise ValueError("Invalid MAC address in config.yaml") from exc

        packet = b"\xff" * 6 + mac_bytes * 16

        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
            sock.sendto(packet, ("255.255.255.255", 9))
