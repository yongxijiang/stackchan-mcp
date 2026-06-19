"""
StackChan Relay — bridges Claude.ai MCP to ESP32 via command polling + WebSocket TTS.

Architecture:
  Claude.ai --MCP/SSE--> this server --HTTP poll + WebSocket--> ESP32 StackChan
"""

import asyncio
import json
import logging
import os
import time
import uuid

import uvicorn
from starlette.applications import Starlette
from starlette.responses import JSONResponse, Response
from starlette.routing import Route, WebSocketRoute
from starlette.websockets import WebSocket, WebSocketDisconnect

from mcp.server import Server
from mcp.server.sse import SseServerTransport
from mcp.types import Tool, TextContent

logger = logging.getLogger("stackchan-relay")

POLL_TOKEN = os.environ.get("POLL_TOKEN", "")
MCP_TOKEN = os.environ.get("MCP_TOKEN", "")
PORT = int(os.environ.get("PORT", "8011"))

# --- HTTP poll state (for non-audio commands) ---
_cmd = None
_cmd_lock = asyncio.Lock()
_delivered_id = None
_last_poll = 0.0

# --- WebSocket state (for TTS audio) ---
_esp32_ws = None
_esp32_session_id = None
_tts_queue = asyncio.Queue()

EXPRESSIONS = (
    "neutral happy sad angry surprised loving embarrassed thinking "
    "sleepy cool winking laughing silly confused shocked crying "
    "kissy confident relaxed funny delicious"
).split()

mcp_server = Server("stackchan-relay")
sse = SseServerTransport("/messages/")


@mcp_server.list_tools()
async def list_tools():
    expr_list = ", ".join(EXPRESSIONS)
    return [
        Tool(
            name="stackchan_speak",
            description="Let StackChan speak text aloud with TTS audio and display it on screen.",
            inputSchema={
                "type": "object",
                "properties": {
                    "text": {"type": "string", "description": "Text to speak"},
                    "expression": {
                        "type": "string",
                        "description": f"Optional face expression: {expr_list}",
                    },
                    "voice": {
                        "type": "string",
                        "description": "TTS voice name, e.g. zh-CN-YunxiNeural, zh-CN-XiaoxiaoNeural, zh-CN-YunyangNeural, zh-CN-XiaoyiNeural",
                    },
                    "rate": {
                        "type": "string",
                        "description": "Speech rate, e.g. +0%, +10%, +20%, -5%",
                    },
                    "pitch": {
                        "type": "string",
                        "description": "Voice pitch, e.g. +0Hz, +5Hz, -10Hz",
                    },
                },
                "required": ["text"],
            },
        ),
        Tool(
            name="stackchan_emote",
            description=f"Change StackChan's face expression: {expr_list}",
            inputSchema={
                "type": "object",
                "properties": {
                    "expression": {"type": "string", "description": "Expression name"},
                },
                "required": ["expression"],
            },
        ),
        Tool(
            name="stackchan_move_head",
            description="Rotate StackChan's head. yaw: left/right -30..30, pitch: up/down -20..20.",
            inputSchema={
                "type": "object",
                "properties": {
                    "yaw": {"type": "number", "description": "Left(-)/right(+) degrees"},
                    "pitch": {"type": "number", "description": "Down(-)/up(+) degrees"},
                },
            },
        ),
        Tool(
            name="stackchan_wiggle",
            description="StackChan shakes head side to side (happy/playful gesture).",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="stackchan_nuzzle",
            description="StackChan nuzzles (heart eyes + blush + head bob). Affectionate gesture.",
            inputSchema={"type": "object", "properties": {}},
        ),
        Tool(
            name="get_stackchan_status",
            description="Check if StackChan is online and responsive.",
            inputSchema={"type": "object", "properties": {}},
        ),
    ]


@mcp_server.call_tool()
async def call_tool(name: str, arguments: dict):
    global _cmd, _delivered_id

    if name == "get_stackchan_status":
        now = time.time()
        poll_online = _last_poll > 0 and (now - _last_poll) < 30
        ws_online = _esp32_ws is not None
        return [
            TextContent(
                type="text",
                text=json.dumps(
                    {
                        "online": poll_online or ws_online,
                        "websocket_connected": ws_online,
                        "tts_available": ws_online,
                        "seconds_since_last_poll": (
                            round(now - _last_poll) if _last_poll else None
                        ),
                    },
                    ensure_ascii=False,
                ),
            )
        ]

    if name == "stackchan_speak":
        text = arguments.get("text", "")
        expression = arguments.get("expression", "neutral")
        voice = arguments.get("voice")
        rate = arguments.get("rate")
        pitch = arguments.get("pitch")

        if _esp32_ws is not None:
            await _tts_queue.put({"text": text, "expression": expression,
                                  "voice": voice, "rate": rate, "pitch": pitch})
            return [TextContent(type="text", text=f"Speaking with TTS: {text}")]
        else:
            cmd = {
                "action": "speak",
                "params": arguments,
                "id": f"cmd_{int(time.time() * 1000)}",
            }
            async with _cmd_lock:
                _cmd = cmd
                _delivered_id = None
            return [TextContent(type="text", text=f"Text sent (no TTS — WebSocket not connected): {text}")]

    cmd = {
        "action": name.replace("stackchan_", ""),
        "params": arguments,
        "id": f"cmd_{int(time.time() * 1000)}",
    }
    async with _cmd_lock:
        _cmd = cmd
        _delivered_id = None
    logger.info("queued: %s", json.dumps(cmd, ensure_ascii=False))
    return [TextContent(type="text", text=f"Command sent: {cmd['action']}")]


# ---- WebSocket gateway (xiaozhi protocol) ----


async def tts_consumer(ws: WebSocket, session_id: str):
    from tts_engine import text_to_opus_frames

    while True:
        req = await _tts_queue.get()
        text = req.get("text", "")
        expression = req.get("expression", "neutral")
        voice = req.get("voice")
        rate = req.get("rate")
        pitch = req.get("pitch")
        if not text:
            continue

        try:
            if expression:
                await ws.send_text(json.dumps({
                    "session_id": session_id,
                    "type": "llm",
                    "emotion": expression,
                }))

            await ws.send_text(json.dumps({
                "session_id": session_id,
                "type": "tts",
                "state": "start",
            }))
            await ws.send_text(json.dumps({
                "session_id": session_id,
                "type": "tts",
                "state": "sentence_start",
                "text": text,
            }))

            frames = await text_to_opus_frames(text, voice=voice, rate=rate, pitch=pitch)
            # Burst first 5 frames to fill ESP32 decode buffer, then pace
            burst = min(5, len(frames))
            for frame in frames[:burst]:
                await ws.send_bytes(frame)
            for frame in frames[burst:]:
                await asyncio.sleep(0.058)
                await ws.send_bytes(frame)

            await ws.send_text(json.dumps({
                "session_id": session_id,
                "type": "tts",
                "state": "stop",
            }))
            logger.info("TTS done: '%s' (%d frames)", text[:30], len(frames))

        except WebSocketDisconnect:
            logger.warning("TTS consumer: WebSocket disconnected")
            break
        except Exception as e:
            logger.error("TTS send error (skipping): %s", e)


async def handle_ws(websocket: WebSocket):
    global _esp32_ws, _esp32_session_id

    await websocket.accept()
    logger.info("WebSocket connected from %s", websocket.client)

    try:
        hello_raw = await asyncio.wait_for(websocket.receive_text(), timeout=15)
        hello = json.loads(hello_raw)
        logger.info("Client hello: %s", json.dumps(hello, ensure_ascii=False)[:200])

        session_id = f"sess_{uuid.uuid4().hex[:12]}"
        _esp32_session_id = session_id

        server_hello = {
            "type": "hello",
            "transport": "websocket",
            "session_id": session_id,
            "audio_params": {
                "sample_rate": 24000,
                "frame_duration": 60,
            },
        }
        await websocket.send_text(json.dumps(server_hello))
        logger.info("Server hello sent, session=%s", session_id)

        _esp32_ws = websocket

        consumer_task = asyncio.create_task(tts_consumer(websocket, session_id))

        try:
            while True:
                msg = await websocket.receive()
                if msg["type"] == "websocket.disconnect":
                    break
                elif "text" in msg:
                    data = json.loads(msg["text"])
                    msg_type = data.get("type", "")
                    if msg_type == "listen":
                        logger.info("ESP32 listen: %s", data.get("state"))
                    elif msg_type == "abort":
                        logger.info("ESP32 abort")
                    else:
                        logger.debug("ESP32 msg: %s", msg_type)
                elif "bytes" in msg:
                    pass
        finally:
            consumer_task.cancel()

    except WebSocketDisconnect:
        logger.info("WebSocket disconnected normally")
    except asyncio.TimeoutError:
        logger.warning("WebSocket hello timeout")
    except Exception as e:
        logger.error("WebSocket error: %s", e)
    finally:
        _esp32_ws = None
        _esp32_session_id = None
        logger.info("WebSocket session ended")


# ---- HTTP endpoints ----


async def handle_sse(request):
    token = request.query_params.get("token", "")
    if not MCP_TOKEN or token != MCP_TOKEN:
        return Response("Unauthorized", status_code=401)
    async with sse.connect_sse(
        request.scope, request.receive, request._send
    ) as (read_stream, write_stream):
        await mcp_server.run(
            read_stream,
            write_stream,
            mcp_server.create_initialization_options(),
        )


async def handle_messages(request):
    await sse.handle_post_message(
        request.scope, request.receive, request._send
    )


async def handle_poll(request):
    global _last_poll, _delivered_id
    token = request.query_params.get("token", "")
    if not POLL_TOKEN or token != POLL_TOKEN:
        return JSONResponse({"error": "unauthorized"}, status_code=401)

    _last_poll = time.time()

    async with _cmd_lock:
        cmd = _cmd
        if cmd and cmd.get("id") == _delivered_id:
            cmd = None
        elif cmd:
            _delivered_id = cmd["id"]

    return JSONResponse(cmd if cmd else {"action": "none"})


async def handle_ack(request):
    global _cmd
    token = request.query_params.get("token", "")
    if not POLL_TOKEN or token != POLL_TOKEN:
        return JSONResponse({"error": "unauthorized"}, status_code=401)
    try:
        body = await request.json()
    except Exception:
        body = {}
    async with _cmd_lock:
        if _cmd and _cmd.get("id") == body.get("id"):
            _cmd = None
    return JSONResponse({"ok": True})


async def handle_health(request):
    return JSONResponse({
        "status": "ok",
        "service": "stackchan-relay",
        "websocket_connected": _esp32_ws is not None,
        "tts_available": _esp32_ws is not None,
    })


app = Starlette(
    routes=[
        Route("/health", handle_health),
        Route("/sse", handle_sse),
        Route("/messages/", handle_messages, methods=["POST"]),
        Route("/poll", handle_poll),
        Route("/ack", handle_ack, methods=["POST"]),
        WebSocketRoute("/ws", handle_ws),
    ],
)

if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(name)s %(levelname)s %(message)s",
    )
    logger.info("Starting StackChan relay on port %d", PORT)
    uvicorn.run(app, host="127.0.0.1", port=PORT)
