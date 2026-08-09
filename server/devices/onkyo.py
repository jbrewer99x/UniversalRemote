from __future__ import annotations

import asyncio
import struct


class OnkyoReceiver:
    """Onkyo/Integra eISCP network-control driver."""

    DEFAULT_INPUTS = {
        "vcr_dvr": "00",
        "cbl_sat": "01",
        "game": "02",
        "aux": "03",
        "pc": "05",
        "bd_dvd": "10",
        "tv_cd": "23",
        "phono": "22",
        "net": "2B",
        "usb": "29",
        "bluetooth": "2E",
    }

    def __init__(self, device_id: str, name: str, config: dict):
        self.device_id = device_id
        self.name = name
        self.config = config
        self.host = str(config.get("host", "")).strip()
        self.port = int(config.get("port", 60128))
        self.timeout = float(config.get("timeout", 3.0))

        self.inputs = dict(self.DEFAULT_INPUTS)
        self.inputs.update(config.get("inputs", {}) or {})

    def describe(self):
        return {
            "id": self.device_id,
            "name": self.name,
            "type": "onkyo",
            "host": self.host,
            "commands": [
                "power_on",
                "power_off",
                "volume_up",
                "volume_down",
                "mute",
                "unmute",
                "mute_toggle",
                "input",
                "raw",
            ],
        }

    async def connect(self):
        self._require_host()
        power = await self._query("PWRQSTN")
        return {
            "online": True,
            "power": self._decode_power(power),
        }

    async def disconnect(self):
        # This driver uses short-lived TCP connections, so there is no
        # persistent connection to tear down.
        return None

    async def get_status(self):
        try:
            power_raw, volume_raw, mute_raw, input_raw = await asyncio.gather(
                self._query("PWRQSTN"),
                self._query("MVLQSTN"),
                self._query("AMTQSTN"),
                self._query("SLIQSTN"),
            )

            input_code = self._parameter(input_raw, "SLI")
            return {
                "online": True,
                "power": self._decode_power(power_raw),
                "volume": self._decode_volume(volume_raw),
                "mute": self._decode_mute(mute_raw),
                "input": {
                    "id": self._input_name(input_code),
                    "code": input_code,
                },
            }
        except Exception as exc:
            return {
                "online": False,
                "error": str(exc),
            }

    async def send_command(self, command: str, value=None):
        command = command.strip().lower()

        commands = {
            "power_on": "PWR01",
            "power_off": "PWR00",
            "volume_up": "MVLUP",
            "volume_down": "MVLDOWN",
            "mute": "AMT01",
            "unmute": "AMT00",
            "mute_toggle": "AMTTG",
        }

        if command in commands:
            response = await self._query(commands[command])
            return {
                "command": command,
                "response": response,
            }

        if command == "input":
            if value is None:
                raise ValueError("Onkyo 'input' requires an input name or code")
            code = self._resolve_input(str(value))
            response = await self._query(f"SLI{code}")
            return {
                "command": command,
                "input": self._input_name(code),
                "code": code,
                "response": response,
            }

        if command == "raw":
            if value is None:
                raise ValueError("Onkyo 'raw' requires an ISCP command as value")
            raw = str(value).strip().upper()
            if not raw:
                raise ValueError("Onkyo raw command cannot be empty")
            response = await self._query(raw)
            return {
                "command": command,
                "sent": raw,
                "response": response,
            }

        raise ValueError(f"Unsupported Onkyo command: {command}")

    async def get_available_inputs(self):
        return [
            {"id": name, "name": name.replace("_", " ").title(), "code": code}
            for name, code in self.inputs.items()
        ]

    async def get_input(self):
        response = await self._query("SLIQSTN")
        code = self._parameter(response, "SLI")
        return {
            "id": self._input_name(code),
            "code": code,
        }

    async def set_input(self, input_id: str):
        code = self._resolve_input(input_id)
        await self._query(f"SLI{code}")

    async def _query(self, command: str) -> str:
        self._require_host()

        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.port),
                timeout=self.timeout,
            )
        except Exception as exc:
            raise RuntimeError(
                f"Could not connect to Onkyo at {self.host}:{self.port}: {exc}"
            ) from exc

        try:
            writer.write(self._build_packet(command))
            await asyncio.wait_for(writer.drain(), timeout=self.timeout)

            header = await asyncio.wait_for(
                reader.readexactly(16),
                timeout=self.timeout,
            )
            magic, header_size, data_size, version = struct.unpack(
                ">4sIIB3x", header
            )

            if magic != b"ISCP":
                raise RuntimeError("Invalid eISCP response header")
            if header_size != 16:
                raise RuntimeError(f"Unexpected eISCP header size: {header_size}")
            if version != 1:
                raise RuntimeError(f"Unsupported eISCP version: {version}")

            payload = await asyncio.wait_for(
                reader.readexactly(data_size),
                timeout=self.timeout,
            )

            return payload.decode("ascii", errors="replace").rstrip("\x1a\r\n")
        except asyncio.IncompleteReadError as exc:
            raise RuntimeError("Onkyo closed the connection before replying") from exc
        except asyncio.TimeoutError as exc:
            raise RuntimeError("Timed out waiting for Onkyo response") from exc
        finally:
            writer.close()
            try:
                await writer.wait_closed()
            except Exception:
                pass

    @staticmethod
    def _build_packet(command: str) -> bytes:
        payload = f"!1{command}\r".encode("ascii")
        header = struct.pack(
            ">4sIIB3x",
            b"ISCP",
            16,
            len(payload),
            1,
        )
        return header + payload

    @staticmethod
    def _parameter(response: str, command: str) -> str:
        prefix = f"!1{command}"
        if not response.startswith(prefix):
            return response
        return response[len(prefix):]

    def _decode_power(self, response: str):
        value = self._parameter(response, "PWR")
        if value == "01":
            return "on"
        if value == "00":
            return "off"
        return value

    def _decode_mute(self, response: str):
        value = self._parameter(response, "AMT")
        if value == "01":
            return True
        if value == "00":
            return False
        return value

    def _decode_volume(self, response: str):
        value = self._parameter(response, "MVL")
        try:
            raw = int(value, 16)
            # Most Onkyo receivers encode main-zone volume in 0.5 dB steps.
            return {
                "raw": value,
                "display": raw / 2.0,
            }
        except ValueError:
            return {
                "raw": value,
                "display": None,
            }

    def _resolve_input(self, value: str) -> str:
        key = value.strip().lower().replace(" ", "_").replace("/", "_")
        if key in self.inputs:
            return str(self.inputs[key]).upper()

        raw = value.strip().upper()
        if len(raw) == 2 and all(c in "0123456789ABCDEF" for c in raw):
            return raw

        raise ValueError(
            f"Unknown Onkyo input '{value}'. "
            f"Known inputs: {', '.join(sorted(self.inputs))}"
        )

    def _input_name(self, code: str) -> str:
        code = code.upper()
        for name, configured_code in self.inputs.items():
            if str(configured_code).upper() == code:
                return name
        return code

    def _require_host(self):
        if not self.host:
            raise RuntimeError(
                f"Onkyo host is not configured for device '{self.device_id}'"
            )
