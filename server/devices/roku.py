from __future__ import annotations

import asyncio
import xml.etree.ElementTree as ET
from typing import Any

import httpx


class RokuDevice:
    """
    Roku / Roku TV controller using Roku ECP.

    Design goals:
    - /query/apps is fetched only once per process and cached.
    - Only physical TV inputs plus Plex and Paramount+ are exposed.
    - Physical inputs use the Roku's display names, not raw tvinput.* IDs.
    - Normal page loads after the first discovery do not query the Roku for apps.
    """

    KEY_COMMANDS = {
        "home": "Home",
        "back": "Back",
        "up": "Up",
        "down": "Down",
        "left": "Left",
        "right": "Right",
        "select": "Select",
        "ok": "Select",
        "play": "Play",
        "pause": "Play",
        "play_pause": "Play",
        "replay": "InstantReplay",
        "info": "Info",
        "options": "Info",
        "rev": "Rev",
        "rewind": "Rev",
        "fwd": "Fwd",
        "fast_forward": "Fwd",
        "volume_up": "VolumeUp",
        "volume_down": "VolumeDown",
        "mute": "VolumeMute",
        "power": "Power",
        "power_on": "PowerOn",
        "power_off": "PowerOff",
    }

    WANTED_APPS = {
        "plex": "Plex",
        "paramount": "Paramount+",
    }

    def __init__(self, device_id: str, name: str, config: dict):
        self.device_id = device_id
        self.name = name
        self.config = config

        self.host = str(config["host"])
        self.port = int(config.get("port", 8060))
        self.timeout = float(config.get("timeout", 3))

        self.base_url = f"http://{self.host}:{self.port}"
        self._client = httpx.AsyncClient(timeout=self.timeout)

        # label -> Roku launch ID
        self._launch_targets: dict[str, str] = {}

        # Discovery is deliberately lazy and happens at most once per process.
        self._discovery_attempted = False
        self._discovery_lock = asyncio.Lock()

    def describe(self) -> dict[str, Any]:
        return {
            "id": self.device_id,
            "name": self.name,
            "type": "roku",
        }

    async def connect(self) -> dict[str, Any]:
        """
        Lightweight connectivity check only.
        Does NOT enumerate apps.
        """
        response = await self._client.get(f"{self.base_url}/query/device-info")
        response.raise_for_status()
        return {"online": True}

    async def disconnect(self):
        await self._client.aclose()

    async def get_status(self) -> dict[str, Any]:
        """
        Keep status cheap: one device-info request, no app enumeration.
        """
        try:
            response = await self._client.get(f"{self.base_url}/query/device-info")
            response.raise_for_status()
            return {"online": True}
        except Exception as exc:
            return {
                "online": False,
                "error": str(exc),
            }

    @staticmethod
    def _clean_name(value: str) -> str:
        return " ".join((value or "").split()).strip()

    async def _discover_once(self):
        """
        One /query/apps request for the lifetime of this RokuDevice instance.

        Roku TV physical inputs are entries whose IDs begin with tvinput.
        The text value of each entry is the user-facing input name configured
        on the TV.

        We also keep only Plex and Paramount+ from the installed app list.
        """
        if self._discovery_attempted:
            return

        async with self._discovery_lock:
            if self._discovery_attempted:
                return

            # Mark this before the request so a failing/offline Roku does not
            # create repeated multi-second discovery stalls on every page load.
            self._discovery_attempted = True

            response = await self._client.get(f"{self.base_url}/query/apps")
            response.raise_for_status()

            root = ET.fromstring(response.text)

            inputs: list[tuple[str, str]] = []
            apps: dict[str, str] = {}

            for element in root.findall(".//app"):
                app_id = self._clean_name(element.attrib.get("id", ""))
                display_name = self._clean_name(element.text or "")

                if not app_id:
                    continue

                # Physical Roku TV inputs.
                if app_id.startswith("tvinput."):
                    label = display_name or app_id
                    inputs.append((label, app_id))
                    continue

                # Only retain the two apps wanted on the remote.
                normalized = display_name.casefold()

                if "plex" in normalized:
                    apps["Plex"] = app_id
                elif "paramount" in normalized:
                    apps["Paramount+"] = app_id

            # Preserve the Roku's input ordering, then append the two apps.
            self._launch_targets = dict(inputs)

            for label in ("Plex", "Paramount+"):
                if label in apps:
                    self._launch_targets[label] = apps[label]

    async def refresh_discovery(self):
        """
        Optional manual refresh for use later if an input is renamed or an app
        is installed/reinstalled. Nothing calls this during normal page loads.
        """
        async with self._discovery_lock:
            self._discovery_attempted = False
            self._launch_targets = {}

        await self._discover_once()

    async def get_available_inputs(self) -> list[str]:
        """
        Returns button labels expected by the existing generic Inputs UI.

        Example:
            ["Antenna TV", "Xbox", "PC", "Plex", "Paramount+"]

        Only the first call performs /query/apps. Later calls are memory-only.
        """
        await self._discover_once()
        return list(self._launch_targets.keys())

    async def get_input(self):
        """
        Intentionally performs NO Roku query.

        The existing FastAPI route asks for get_input() whenever it asks for
        the input list. Querying /query/active-app here would add latency to
        every refresh, so return None instead.
        """
        return None

    async def set_input(self, input_id: str):
        """
        input_id is actually the human-readable button label from the existing
        generic UI. Resolve it to the cached Roku launch ID and launch directly.
        """
        await self._discover_once()

        target = self._launch_targets.get(input_id)

        # Also accept a raw Roku launch ID for compatibility/debugging.
        if target is None and (
            input_id.startswith("tvinput.") or input_id.isdigit()
        ):
            target = input_id

        if target is None:
            raise ValueError(f"Unknown Roku input/app: {input_id}")

        response = await self._client.post(f"{self.base_url}/launch/{target}")
        response.raise_for_status()
        return {"launched": input_id, "id": target}

    async def send_command(self, command: str, value: Any | None = None):
        command = command.strip().lower()

        if command in {"input", "launch"}:
            if value is None:
                raise ValueError(f"{command} requires a value")
            return await self.set_input(str(value))

        if command == "keypress":
            if value is None:
                raise ValueError("keypress requires a Roku key name")
            roku_key = str(value)
        else:
            roku_key = self.KEY_COMMANDS.get(command)

        if not roku_key:
            raise ValueError(f"Unsupported Roku command: {command}")

        response = await self._client.post(
            f"{self.base_url}/keypress/{roku_key}"
        )
        response.raise_for_status()

        return {"command": command, "key": roku_key}
