"""Edge-TTS → ffmpeg → Opus frame generator for xiaozhi WebSocket protocol."""

import asyncio
import logging
import os

logger = logging.getLogger("stackchan-tts")

SAMPLE_RATE = 24000
CHANNELS = 1
FRAME_MS = 60
FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS // 1000  # 1440
FRAME_BYTES = FRAME_SAMPLES * 2  # s16le = 2880 bytes per frame

TTS_VOICE = os.environ.get("TTS_VOICE", "zh-CN-XiaoxiaoNeural")
TTS_RATE = os.environ.get("TTS_RATE", "+10%")
TTS_PITCH = os.environ.get("TTS_PITCH", "+5Hz")


async def text_to_opus_frames(text, voice=None, rate=None, pitch=None):
    import edge_tts
    import opuslib

    voice = voice or TTS_VOICE
    rate = rate or TTS_RATE
    pitch = pitch or TTS_PITCH

    communicate = edge_tts.Communicate(text, voice, rate=rate, pitch=pitch)
    mp3_chunks = []
    async for chunk in communicate.stream():
        if chunk["type"] == "audio":
            mp3_chunks.append(chunk["data"])
    mp3_data = b"".join(mp3_chunks)

    if not mp3_data:
        logger.warning("edge-tts returned empty audio for: %s", text[:50])
        return []

    proc = await asyncio.create_subprocess_exec(
        "ffmpeg", "-i", "pipe:0",
        "-f", "s16le", "-ar", str(SAMPLE_RATE), "-ac", str(CHANNELS),
        "-loglevel", "error", "pipe:1",
        stdin=asyncio.subprocess.PIPE,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    pcm_data, err = await proc.communicate(input=mp3_data)
    if proc.returncode != 0:
        logger.error("ffmpeg error: %s", err.decode(errors="replace"))
        return []

    enc = opuslib.Encoder(SAMPLE_RATE, CHANNELS, opuslib.APPLICATION_AUDIO)
    frames = []
    for i in range(0, len(pcm_data), FRAME_BYTES):
        chunk = pcm_data[i : i + FRAME_BYTES]
        if len(chunk) < FRAME_BYTES:
            chunk += b"\x00" * (FRAME_BYTES - len(chunk))
        frames.append(enc.encode(chunk, FRAME_SAMPLES))

    logger.info("TTS: %d opus frames for '%s'", len(frames), text[:30])
    return frames
