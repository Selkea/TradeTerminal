"""Wire protocol shared with the C++ terminal (see README / plan §IPC).

Frame:  u32 payload_len | u16 msg_type | u16 reserved   (little-endian)
CandleRecord (48 B): i64 ts_epoch_sec | f64 open,high,low,close,volume
TickRecord   (32 B): u32 sub_id | u32 sym_idx | i64 ts_ms | f64 price | f64 day_volume
"""
import json
import struct

HDR = struct.Struct("<IHH")
CANDLE = struct.Struct("<q5d")
TICK = struct.Struct("<IIqdd")

MAX_PAYLOAD = 16 * 1024 * 1024

HELLO = 0x01
REQ_CANDLES = 0x02
RESP_CANDLES = 0x03
SUB_QUOTES = 0x04
SUB_ACK = 0x05
TICKS = 0x06
UNSUB = 0x07
ERROR = 0x08
PING = 0x09
PONG = 0x0A
SHUTDOWN = 0x0F


def frame(msg_type: int, payload: bytes = b"") -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too large: {len(payload)}")
    return HDR.pack(len(payload), msg_type, 0) + payload


def jframe(msg_type: int, obj) -> bytes:
    return frame(msg_type, json.dumps(obj, separators=(",", ":")).encode("utf-8"))


def candles_response(meta: dict, rows) -> bytes:
    """RESP_CANDLES payload: u32 meta_len | meta JSON | count x CandleRecord."""
    meta = dict(meta, count=len(rows))
    mj = json.dumps(meta, separators=(",", ":")).encode("utf-8")
    body = bytearray(struct.pack("<I", len(mj)) + mj)
    for r in rows:
        body += CANDLE.pack(*r)
    return frame(RESP_CANDLES, bytes(body))


async def read_frame(reader):
    """Returns (msg_type, payload) or raises IncompleteReadError on EOF."""
    hdr = await reader.readexactly(HDR.size)
    length, msg_type, _ = HDR.unpack(hdr)
    if length > MAX_PAYLOAD:
        raise ValueError(f"oversized frame: {length}")
    payload = await reader.readexactly(length) if length else b""
    return msg_type, payload
