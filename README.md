# Universal Remote

Universal remote-control project with a TrueNAS/FastAPI backend, device drivers, web UI, and planned ESP32 handheld firmware.

## Repository layout

- `server/` — FastAPI backend, device drivers, web UI, and local device configuration example.
- `firmware/` — ESP32 source will live here. Compiled binaries are intentionally ignored.
- `deploy/truenas/` — TrueNAS deployment snippets and notes.
- `scripts/` — helper scripts such as firmware publishing.
- `docs/` — project/reference notes.

## First-time setup

Copy the example device configuration and edit it locally:

```bash
cp server/config.example.yaml server/config.yaml
```

`server/config.yaml` is deliberately ignored by Git because local network addresses, tokens, MAC addresses, and pairing references can become sensitive deployment data.

Copy `.env.example` to `.env` only if environment variables are needed:

```bash
cp .env.example .env
```

Never commit `.env`.

## Secrets and pairing state

The repository intentionally ignores:

- `.env` and environment-secret files
- `server/config.yaml`
- SQLite/database files
- LG webOS pairing state
- private keys/certificates
- compiled ESP32 firmware binaries

The LG pairing database from the existing installation should remain only on the TrueNAS persistent runtime storage.

## Running the server

From `server/`:

```bash
python -m uvicorn app:app --host 0.0.0.0 --port 8000
```

For TrueNAS, keep persistent runtime files and firmware outside the Git checkout and mount them into the container.

## Publishing ESP32 firmware

See `scripts/publish-firmware.sh` and `deploy/truenas/compose-ota-snippet.yaml`.
