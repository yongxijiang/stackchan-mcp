"""HTTP capture server for receiving photos from ESP32.

ESP32's camera.Explain() POSTs multipart/form-data with:
- field 'question' (text)
- field 'file' (camera.jpg, JPEG image)

This server saves the JPEG and returns the file path so MCP client
can view the image via the Read tool.
"""

from __future__ import annotations

import json
import logging
import os
import time

from aiohttp import web

logger = logging.getLogger(__name__)

CAPTURE_DIR = os.path.expanduser("~/.stackchan/captures")
CAPTURE_TOKEN_KEY = web.AppKey("capture_token", str)
ESP32_MGR_KEY = web.AppKey("esp32_mgr", object)


def _is_authorized(auth_header: str, expected_token: str) -> bool:
    """Return whether the bearer auth header matches the expected token."""
    return auth_header == f"Bearer {expected_token}"


async def handle_capture(request: web.Request) -> web.Response:
    """Handle photo upload from ESP32."""
    expected_token = request.app[CAPTURE_TOKEN_KEY]
    if expected_token and not _is_authorized(
        request.headers.get("Authorization", ""), expected_token
    ):
        logger.warning("Capture upload auth rejected")
        return web.Response(
            text='{"error": "Unauthorized"}',
            status=401,
            content_type="application/json",
        )

    os.makedirs(CAPTURE_DIR, exist_ok=True)

    reader = await request.multipart()
    question = ""
    image_path = ""

    async for part in reader:
        if part.name == "question":
            question = (await part.read()).decode("utf-8")
        elif part.name == "file":
            timestamp = int(time.time() * 1000)
            filename = f"capture_{timestamp}.jpg"
            image_path = os.path.join(CAPTURE_DIR, filename)
            with open(image_path, "wb") as f:
                while True:
                    chunk = await part.read_chunk(8192)
                    if not chunk:
                        break
                    f.write(chunk)

    if image_path and os.path.exists(image_path):
        file_size = os.path.getsize(image_path)
        logger.info(
            "Captured photo: %s (%d bytes), question: %s",
            image_path,
            file_size,
            question,
        )
        result = json.dumps({
            "image_path": image_path,
            "size_bytes": file_size,
            "question": question,
        })
        return web.Response(text=result, content_type="application/json")

    return web.Response(
        text='{"error": "No image received"}',
        status=400,
        content_type="application/json",
    )


async def handle_control(request: web.Request) -> web.Response:
    """HTTP control endpoint — lets external callers invoke ESP32 tools.

    POST /control  {"tool": "self.set_head_angles", "arguments": {"yaw": 30}}
    Returns the ESP32 response JSON.
    """
    try:
        body = await request.json()
    except Exception:
        return web.Response(
            text='{"error": "invalid JSON"}',
            status=400,
            content_type="application/json",
        )

    tool = body.get("tool", "")
    arguments = body.get("arguments", {})
    if not tool:
        return web.Response(
            text='{"error": "missing tool field"}',
            status=400,
            content_type="application/json",
        )

    esp32_mgr = request.app.get(ESP32_MGR_KEY)
    if esp32_mgr is None:
        return web.Response(
            text='{"error": "gateway not initialised"}',
            status=503,
            content_type="application/json",
        )

    conn = esp32_mgr.connection
    if conn is None or not conn.connected:
        return web.Response(
            text='{"error": "ESP32 not connected"}',
            status=503,
            content_type="application/json",
        )

    logger.info("Control: %s(%s)", tool, json.dumps(arguments, ensure_ascii=False))
    result, error = await conn.call_tool(tool, arguments)
    if error:
        return web.Response(
            text=json.dumps({"error": error}, ensure_ascii=False),
            status=502,
            content_type="application/json",
        )
    return web.Response(
        text=json.dumps({"result": result}, ensure_ascii=False),
        content_type="application/json",
    )


async def handle_ota_stub(request: web.Request) -> web.Response:
    """Stub OTA/activation endpoint.

    When the ESP32 firmware's ``ota_url`` NVS key points at this server,
    the device contacts us instead of ``api.tenclass.net`` during boot.
    Returning an empty JSON object means:
    - no ``websocket`` section → NVS websocket.url is **not** overwritten
    - no ``activation`` section → boot proceeds immediately (no challenge)
    - no ``firmware`` section → no OTA upgrade triggered

    This lets the user-configured ``websocket.url`` in NVS survive reboots.
    """
    logger.info(
        "OTA stub: %s %s (User-Agent: %s)",
        request.method,
        request.path,
        request.headers.get("User-Agent", "?"),
    )
    return web.Response(
        text="{}",
        content_type="application/json",
    )


def create_capture_app(capture_token: str = "", esp32_mgr: object | None = None) -> web.Application:
    """Create the HTTP capture application."""
    app = web.Application()
    app[CAPTURE_TOKEN_KEY] = capture_token
    if esp32_mgr is not None:
        app[ESP32_MGR_KEY] = esp32_mgr
    app.router.add_post("/capture", handle_capture)
    app.router.add_post("/control", handle_control)
    # OTA stub: accept any path the firmware might hit during activation.
    # Routes are tried in registration order, so /capture and /control win;
    # everything else falls through to the stub.
    app.router.add_route("*", "/{path:.*}", handle_ota_stub)
    return app
