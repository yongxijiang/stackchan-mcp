"""TTS -> Opus frame generator for xiaozhi WebSocket protocol.

Supports two backends:
  - edge-tts (Microsoft, free, via ffmpeg)
  - fish-audio (Fish Audio API, paid, direct PCM -- no ffmpeg needed)

Set TTS_ENGINE=fish and FISH_API_KEY to use Fish Audio.
"""

import asyncio
import logging
import os

logger = logging.getLogger("stackchan-tts")

SAMPLE_RATE = 16000
CHANNELS = 1
FRAME_MS = 60
FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS // 1000  # 960
FRAME_BYTES = FRAME_SAMPLES * 2  # s16le = 1920 bytes per frame

TTS_ENGINE = os.environ.get("TTS_ENGINE", "edge").lower()
TTS_VOICE = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
TTS_RATE = os.environ.get("TTS_RATE", "+10%")
TTS_PITCH = os.environ.get("TTS_PITCH", "+5Hz")
TTS_ROBOT = os.environ.get("TTS_ROBOT", "").lower() in ("true", "1", "yes")

FISH_API_KEY = os.environ.get("FISH_API_KEY", "")
FISH_VOICE_ID = os.environ.get("FISH_VOICE_ID", "")
FISH_API_BASE = os.environ.get("FISH_API_BASE", "https://api.fish.audio")


def _pcm_to_opus_frames(pcm_data):
    """Encode raw s16le PCM into 60ms Opus frames."""
    import opuslib

    enc = opuslib.Encoder(SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_AUDIO)
    frames = []
    for i in range(0, len(pcm_data), FRAME_BYTES):
        chunk = pcm_data[i : i + FRAME_BYTES]
        if len(chunk) < FRAME_BYTES:
            chunk += b"\x00" * (FRAME_BYTES - len(chunk))
        frames.append(enc.encode(chunk, FRAME_SAMPLES))
    return frames


async def _fish_audio_tts(text, voice_id=None):
    """Fish Audio API -> raw PCM s16le @ 16kHz (non-streaming, kept as fallback)."""
    import httpx

    voice_id = voice_id or FISH_VOICE_ID
    if not FISH_API_KEY:
        logger.error("FISH_API_KEY not set")
        return b""
    if not voice_id:
        logger.error("FISH_VOICE_ID not set")
        return b""

    async with httpx.AsyncClient(timeout=30.0) as client:
        resp = await client.post(
            f"{FISH_API_BASE}/v1/tts",
            headers={
                "Authorization": f"Bearer {FISH_API_KEY}",
                "Content-Type": "application/json",
            },
            json={
                "text": text,
                "reference_id": voice_id,
                "format": "pcm",
                "mp3_bitrate": 64,
                "opus_bitrate": -1000,
                "latency": "normal",
                "streaming": False,
                "sample_rate": SAMPLE_RATE,
            },
        )
        if resp.status_code != 200:
            logger.error("Fish Audio error %d: %s", resp.status_code, resp.text[:200])
            return b""
        return resp.content


async def stream_fish_opus_frames(text, voice_id=None):
    """Stream Opus frames from Fish Audio. Yields frames as PCM chunks arrive."""
    import httpx
    import opuslib

    voice_id = voice_id or FISH_VOICE_ID
    if not FISH_API_KEY:
        logger.error("FISH_API_KEY not set")
        return
    if not voice_id:
        logger.error("FISH_VOICE_ID not set")
        return

    enc = opuslib.Encoder(SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_AUDIO)
    pcm_buf = b""
    frame_count = 0

    async with httpx.AsyncClient(timeout=httpx.Timeout(10.0, read=120.0)) as client:
        async with client.stream(
            "POST",
            f"{FISH_API_BASE}/v1/tts",
            headers={
                "Authorization": f"Bearer {FISH_API_KEY}",
                "Content-Type": "application/json",
            },
            json={
                "text": text,
                "reference_id": voice_id,
                "format": "pcm",
                "latency": "normal",
                "streaming": True,
                "sample_rate": SAMPLE_RATE,
            },
        ) as resp:
            if resp.status_code != 200:
                err = await resp.aread()
                logger.error("Fish stream error %d: %s", resp.status_code, err.decode(errors="replace")[:200])
                return
            async for chunk in resp.aiter_bytes():
                pcm_buf += chunk
                while len(pcm_buf) >= FRAME_BYTES:
                    yield enc.encode(pcm_buf[:FRAME_BYTES], FRAME_SAMPLES)
                    pcm_buf = pcm_buf[FRAME_BYTES:]
                    frame_count += 1

    if pcm_buf:
        pcm_buf += b"\x00" * (FRAME_BYTES - len(pcm_buf))
        yield enc.encode(pcm_buf, FRAME_SAMPLES)
        frame_count += 1

    logger.info("TTS[fish-stream]: %d frames for '%s'", frame_count, text[:30])


async def _edge_tts(text, voice=None, rate=None, pitch=None, robot=None):
    """Edge-TTS -> ffmpeg -> raw PCM s16le @ 16kHz."""
    import edge_tts

    voice = voice or TTS_VOICE
    rate = rate or TTS_RATE
    pitch = pitch or TTS_PITCH
    if robot is None:
        robot = TTS_ROBOT

    communicate = edge_tts.Communicate(text, voice, rate=rate, pitch=pitch)
    mp3_chunks = []
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            mp3_chunks.append(chunk["data"])
    mp3_data = b"".join(mp3_chunks)

    if not mp3_data:
        logger.warning("edge-tts returned empty audio for: %s", text[:50])
        return b""

    ff_args = ["ffmpeg", "-i", "pipe:0"]
    if robot:
        ff_args += ["-af", "highpass=f=300,tremolo=f=5:d=0.12,aecho=0.8:0.85:4:0.2"]
    ff_args += ["-f", "s16le", "-ar", str(SAMPLE_RATE), "-ac", str(CHANNELS),
                "-loglevel", "error", "pipe:1"]

    proc = await asyncio.create_subprocess_exec(
        *ff_args,
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    pcm_data, err = await proc.communicate(input=mp3_data)
    if proc.returncode != 0:
        logger.error("ffmpeg error: %s", err.decode(errors="replace"))
        return b""
    return pcm_data


async def text_to_opus_frames(text, voice=None, rate=None, pitch=None, robot=None):
    if TTS_ENGINE == "fish":
        pcm_data = await _fish_audio_tts(text, voice_id=voice)
        engine_name = "fish"
    else:
        pcm_data = await _edge_tts(text, voice=voice, rate=rate, pitch=pitch, robot=robot)
        engine_name = "edge"

    if not pcm_data:
        return []

    frames = _pcm_to_opus_frames(pcm_data)
    logger.info("TTS[%s]: %d opus frames for '%s'", engine_name, len(frames), text[:30])
    return frames
