from __future__ import annotations

import httpx


class BazzitePC:
    """
    Client driver for a small HTTP control agent running on the Bazzite PC.

    The companion agent is intentionally separate from the TrueNAS service.
    This avoids storing SSH credentials and gives us one stable API regardless
    of whether Bazzite is using KDE, GNOME, Steam Gaming Mode, etc.
    """

    def __init__(self, device_id: str, name: str, config: dict):
        self.device_id = device_id
        self.name = name
        self.config = config
        self.host = str(config.get("host", "")).strip()
        self.port = int(config.get("port", 8765))
        self.scheme = str(config.get("scheme", "http")).strip()
        self.timeout = float(config.get("timeout", 3.0))
        self.token = str(config.get("token", "")).strip()
        self._client: httpx.AsyncClient | None = None

    @property
    def base_url(self) -> str:
        return f"{self.scheme}://{self.host}:{self.port}"

    def describe(self):
        return {
            "id": self.device_id,
            "name": self.name,
            "type": "bazzite",
            "host": self.host,
            "commands": [
                "up",
                "down",
                "left",
                "right",
                "select",
                "back",
                "home",
                "play_pause",
                "next",
                "previous",
                "volume_up",
                "volume_down",
                "mute",
                "launch",
                "sleep",
                "shutdown",
                "custom",
            ],
        }

    async def connect(self):
        return await self.get_status()

    async def disconnect(self):
        if self._client is not None:
            await self._client.aclose()
            self._client = None

    async def get_status(self):
        try:
            data = await self._get("/status")
            if isinstance(data, dict):
                data.setdefault("online", True)
                return data
            return {
                "online": True,
                "status": data,
            }
        except Exception as exc:
            return {
                "online": False,
                "error": str(exc),
            }

    async def send_command(self, command: str, value=None):
        command = command.strip().lower()

        payload = {
            "command": command,
            "value": value,
        }

        result = await self._post("/command", payload)
        return {
            "command": command,
            "value": value,
            "agent": result,
        }

    async def get_available_inputs(self):
        """
        Treat launchable applications as 'inputs' so the existing generic
        Universal Remote UI/API can list and select them.
        """
        try:
            result = await self._get("/apps")
        except Exception:
            return []

        if isinstance(result, dict):
            return result.get("apps", [])
        if isinstance(result, list):
            return result
        return []

    async def get_input(self):
        try:
            result = await self._get("/active")
            return result
        except Exception:
            return None

    async def set_input(self, input_id: str):
        await self._post(
            "/command",
            {
                "command": "launch",
                "value": input_id,
            },
        )

    async def _ensure_client(self):
        if self._client is None:
            headers = {}
            if self.token:
                headers["Authorization"] = f"Bearer {self.token}"

            self._client = httpx.AsyncClient(
                timeout=self.timeout,
                headers=headers,
            )
        return self._client

    async def _get(self, path: str):
        self._require_host()
        client = await self._ensure_client()

        try:
            response = await client.get(self.base_url + path)
            response.raise_for_status()
            return self._decode_response(response)
        except httpx.HTTPError as exc:
            raise RuntimeError(f"Bazzite agent request failed: {exc}") from exc

    async def _post(self, path: str, payload: dict):
        self._require_host()
        client = await self._ensure_client()

        try:
            response = await client.post(self.base_url + path, json=payload)
            response.raise_for_status()
            return self._decode_response(response)
        except httpx.HTTPError as exc:
            raise RuntimeError(f"Bazzite agent request failed: {exc}") from exc

    @staticmethod
    def _decode_response(response: httpx.Response):
        if not response.content:
            return {"ok": True}

        content_type = response.headers.get("content-type", "")
        if "application/json" in content_type:
            return response.json()

        return {
            "ok": True,
            "text": response.text,
        }

    def _require_host(self):
        if not self.host:
            raise RuntimeError(
                f"Bazzite host is not configured for device '{self.device_id}'"
            )
