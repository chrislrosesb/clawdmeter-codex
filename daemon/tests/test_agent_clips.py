import asyncio
import struct

from daemon import agent_clips


class FakeSession:
    def __init__(self, have=False):
        self.packets = []
        self.have = have

    async def write_packet(self, packet):
        self.packets.append(packet)
        return 2 if self.have and packet[1] == agent_clips.CLIP_START else 1


def test_shipped_clips_load_and_start_packet_is_45_bytes():
    clips = agent_clips.load_clips()
    assert {c.agent for c in clips} >= {"champion", "sales-marketing"}
    for c in clips:
        start = c.start_packet()
        assert len(start) == 45 and start[:2] == b"\xa6\x01"
        assert struct.unpack_from("<I", start, 6)[0] == len(c.body)


def test_sender_slices_and_skips_clips_the_device_has():
    clip = agent_clips.Clip("t", "champion", "Tori", 10, 8, 8, 1, b"x" * 2500)
    session = FakeSession()
    sender = agent_clips.ClipSender([clip], chunk=1000)
    asyncio.run(sender.step(session, budget=1500))
    assert not sender.done and len(session.packets) == 3          # start + 2 chunks
    asyncio.run(sender.step(session, budget=1500))
    assert sender.done and b"".join(p[10:] for p in session.packets[1:]) == clip.body

    have = FakeSession(have=True)
    sender = agent_clips.ClipSender([clip], chunk=1000)
    asyncio.run(sender.step(have, budget=10_000))
    assert sender.done and len(have.packets) == 1
