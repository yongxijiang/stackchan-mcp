"""
StackChan Relay — bridges Claude.ai MCP to ESP32 via command polling.

Architecture:
  Claude.ai --MCP/SSE--> this server --HTTP poll--> ESP32 StackChan

Deploy on a VPS behind Cloudflare Tunnel (or any HTTPS reverse proxy).
"""

import asyncio
import json
import logging
import os
import time

import uvicorn
from starlette.applications import Starlette
from starlette.responses import JSONResponse, Response
from starlette.routing import Route

from mcp.server import Server
from mcp.server.sse import SseServerTransport
from mcp.types import Tool, TextContent

logger = logging.getLogger("stackchan-relay")

POLL_TOKEN = os.environ.get("POLL_TOKEN", "")
MCP_TOKEN = os.environ.get("MCP_TOKEN", "")
PORT = int(os.environ.get("PORT", "8011"))

_cmd = None
_cmd_lock = asyncio.Lock()
_delivered_id = None
_last_poll = 0.0

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
            description="Let StackChan display text with mouth animation.",
            inputSchema={
                "type": "object",
                "properties": {
                    "text": {"type": "string", "description": "Text to display/speak"},
                    "expression": {
                        "type": "string",
                        "description": f"Optional face expression: {expr_list}",
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
            description="Rotate StackChan's head. yaw: left/right -30..30, pitch: up/down -20..20. 0,0 centers.",
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
        online = _last_poll > 0 and (now - _last_poll) < 30
        return [
            TextContent(
                type="text",
                text=json.dumps(
                    {
                        "online": online,
                        "seconds_since_last_poll": (
                            round(now - _last_poll) if _last_poll else None
                        ),
                    },
                    ensure_ascii=False,
                ),
            )
        ]

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
    return JSONResponse({"status": "ok", "service": "stackchan-relay"})


app = Starlette(
    routes=[
        Route("/health", handle_health),
        Route("/sse", handle_sse),
        Route("/messages/", handle_messages, methods=["POST"]),
        Route("/poll", handle_poll),
        Route("/ack", handle_ack, methods=["POST"]),
    ],
)

if __name__ == "__main__":
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(name)s %(levelname)s %(message)s",
    )
    logger.info("Starting StackChan relay on port %d", PORT)
    uvicorn.run(app, host="127.0.0.1", port=PORT)
