from __future__ import annotations

import asyncio
import json
import random
import socket
from typing import Any


class GoveeLANGroup:
    """Small Govee LAN-API driver that treats multiple lamps as one device."""

    def __init__(self, device_id: str, name: str, config: dict):
        self.device_id = device_id
        self.name = name
        self.config = config
        self.port = int(config.get("port", 4003))
        self.timeout = float(config.get("timeout", 1.0))
        self.members = list(config.get("members", []))

        if not self.members:
            raise ValueError(f"{device_id}: at least one Govee member is required")

        follow = config.get("power_follow", {}) or {}
        self.power_follow_enabled = bool(follow.get("enabled", False))
        self.power_follow_interval = float(follow.get("interval", 1.0))
        self.power_follow_leader = follow.get("leader")
        self._power_follow_task: asyncio.Task | None = None
        self._last_leader_power: bool | None = None

        self.crazy_mode_interval = 0.2
        self._crazy_mode_task: asyncio.Task | None = None

    def describe(self) -> dict[str, Any]:
        return {
            "id": self.device_id,
            "name": self.name,
            "type": "govee_lan_group",
        }

    async def connect(self) -> dict[str, Any]:
        # Govee LAN control is connectionless UDP; nothing to pair here.
        return await self.get_status()

    async def start(self) -> None:
        if not self.power_follow_enabled or self._power_follow_task is not None:
            return
        self._power_follow_task = asyncio.create_task(
            self._power_follow_loop(),
            name=f"{self.device_id}-power-follow",
        )

    async def disconnect(self) -> None:
        await self._stop_crazy_mode()

        if self._power_follow_task is not None:
            self._power_follow_task.cancel()
            try:
                await self._power_follow_task
            except asyncio.CancelledError:
                pass
            self._power_follow_task = None


    def _member_host(self, member: dict[str, Any]) -> str | None:
        return member.get("host")

    def _leader_member(self) -> dict[str, Any] | None:
        if self.power_follow_leader:
            for member in self.members:
                if (
                    member.get("host") == self.power_follow_leader
                    or member.get("name") == self.power_follow_leader
                ):
                    return member
        return self.members[0] if self.members else None

    def _query_power_sync(self, host: str) -> bool | None:
        payload = self._payload("devStatus", {})
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            sock.bind(("", 4002))
            sock.settimeout(self.timeout)
            sock.sendto(payload, (host, self.port))

            while True:
                data, addr = sock.recvfrom(4096)
                if addr[0] != host:
                    continue
                message = json.loads(data.decode("utf-8"))
                msg = message.get("msg", {})
                if msg.get("cmd") != "devStatus":
                    continue
                on_off = msg.get("data", {}).get("onOff")
                if on_off in (0, 1):
                    return bool(on_off)
                return None
        except (TimeoutError, socket.timeout, OSError, ValueError, json.JSONDecodeError):
            return None
        finally:
            sock.close()

    async def _query_power(self, host: str) -> bool | None:
        return await asyncio.to_thread(self._query_power_sync, host)

    async def _send_to_hosts(
        self, hosts: list[str], command: str, data: dict[str, Any]
    ) -> None:
        if not hosts:
            return

        payload = self._payload(command, data)

        def send_sync() -> None:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                sock.settimeout(self.timeout)
                for host in hosts:
                    sock.sendto(payload, (host, self.port))
            finally:
                sock.close()

        await asyncio.to_thread(send_sync)

    async def _power_follow_loop(self) -> None:
        leader = self._leader_member()
        if not leader or not leader.get("host"):
            return

        leader_host = leader["host"]
        follower_hosts = [
            member.get("host")
            for member in self.members
            if member is not leader and member.get("host")
        ]

        while True:
            try:
                power = await self._query_power(leader_host)
                if power is not None and power != self._last_leader_power:
                    await self._send_to_hosts(
                        follower_hosts,
                        "turn",
                        {"value": 1 if power else 0},
                    )
                    self._last_leader_power = power
            except asyncio.CancelledError:
                raise
            except Exception:
                # A missed LAN response should not kill the follower task.
                pass

            await asyncio.sleep(self.power_follow_interval)


    async def _send_host_color(self, host: str, r: int, g: int, b: int) -> None:
        await self._send_to_hosts(
            [host],
            "colorwc",
            {
                "color": {"r": r, "g": g, "b": b},
                "colorTemInKelvin": 0,
            },
        )

    @staticmethod
    def _random_rgb() -> tuple[int, int, int]:
        # Full-range RGB, while rejecting very dark colors that barely look like a change.
        while True:
            rgb = (
                random.randint(0, 255),
                random.randint(0, 255),
                random.randint(0, 255),
            )
            if max(rgb) >= 96:
                return rgb

    async def _crazy_mode_loop(self) -> None:
        hosts = [member.get("host") for member in self.members if member.get("host")]
        try:
            while True:
                colors: list[tuple[int, int, int]] = []
                for _ in hosts:
                    color = self._random_rgb()
                    while color in colors:
                        color = self._random_rgb()
                    colors.append(color)

                await asyncio.gather(*(
                    self._send_host_color(host, *color)
                    for host, color in zip(hosts, colors)
                ))
                await asyncio.sleep(self.crazy_mode_interval)
        except asyncio.CancelledError:
            raise

    async def _start_crazy_mode(self) -> None:
        if self._crazy_mode_task is not None and not self._crazy_mode_task.done():
            return
        self._crazy_mode_task = asyncio.create_task(
            self._crazy_mode_loop(),
            name=f"{self.device_id}-crazy-mode",
        )

    async def _stop_crazy_mode(self) -> None:
        if self._crazy_mode_task is None:
            return
        self._crazy_mode_task.cancel()
        try:
            await self._crazy_mode_task
        except asyncio.CancelledError:
            pass
        self._crazy_mode_task = None

    async def get_available_inputs(self) -> list:
        return []

    async def get_input(self):
        return None

    async def set_input(self, input_id: str):
        raise ValueError("Govee lights do not have inputs")

    def _payload(self, command: str, data: dict[str, Any]) -> bytes:
        return json.dumps({
            "msg": {
                "cmd": command,
                "data": data,
            }
        }, separators=(",", ":")).encode("utf-8")

    def _send_sync(self, command: str, data: dict[str, Any]) -> None:
        payload = self._payload(command, data)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            sock.settimeout(self.timeout)
            for member in self.members:
                host = member.get("host")
                if not host:
                    continue
                sock.sendto(payload, (host, self.port))
        finally:
            sock.close()

    async def _send(self, command: str, data: dict[str, Any]) -> None:
        await asyncio.to_thread(self._send_sync, command, data)

    @staticmethod
    def _int(value: Any, minimum: int, maximum: int, label: str) -> int:
        try:
            parsed = int(value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"{label} must be an integer") from exc
        if parsed < minimum or parsed > maximum:
            raise ValueError(f"{label} must be between {minimum} and {maximum}")
        return parsed

    async def send_command(self, command: str, value: Any | None = None):
        command = command.lower().strip()

        if command in {"on", "power_on"}:
            await self._send("turn", {"value": 1})
        elif command in {"off", "power_off"}:
            await self._send("turn", {"value": 0})
        elif command == "power":
            # Explicit value is required so a group cannot become desynchronized by toggling.
            if value in (True, 1, "1", "on", "ON", "true", "True"):
                await self._send("turn", {"value": 1})
            elif value in (False, 0, "0", "off", "OFF", "false", "False"):
                await self._send("turn", {"value": 0})
            else:
                raise ValueError("power requires an explicit on/off value")
        elif command == "brightness":
            brightness = self._int(value, 0, 100, "brightness")
            await self._send("brightness", {"value": brightness})
        elif command == "temperature":
            kelvin = self._int(value, 1000, 10000, "temperature")
            await self._send("colorwc", {
                "color": {"r": 0, "g": 0, "b": 0},
                "colorTemInKelvin": kelvin,
            })
        elif command == "crazy_on":
            await self._start_crazy_mode()
        elif command == "crazy_off":
            await self._stop_crazy_mode()
        elif command == "crazy_toggle":
            if self._crazy_mode_task is not None and not self._crazy_mode_task.done():
                await self._stop_crazy_mode()
            else:
                await self._start_crazy_mode()
        elif command == "color":
            if not isinstance(value, dict):
                raise ValueError("color requires {r, g, b}")
            r = self._int(value.get("r"), 0, 255, "red")
            g = self._int(value.get("g"), 0, 255, "green")
            b = self._int(value.get("b"), 0, 255, "blue")
            await self._send("colorwc", {
                "color": {"r": r, "g": g, "b": b},
                "colorTemInKelvin": 0,
            })
        else:
            raise ValueError(f"Unsupported Govee command: {command}")

        return {
            "command": command,
            "value": value,
            "members": [m.get("name", m.get("host")) for m in self.members],
            "crazy_mode": self._crazy_mode_task is not None and not self._crazy_mode_task.done(),
        }

    async def get_status(self) -> dict[str, Any]:
        # Commands are connectionless UDP and Govee does not ACK ordinary writes.
        # Keep status lightweight rather than opening competing listeners on UDP 4002.
        return {
            "online": True,
            "power": None,
            "input": None,
            "volume": None,
            "crazy_mode": self._crazy_mode_task is not None and not self._crazy_mode_task.done(),
            "members": [
                {
                    "name": m.get("name", m.get("host")),
                    "host": m.get("host"),
                    "model": m.get("model"),
                }
                for m in self.members
            ],
        }
