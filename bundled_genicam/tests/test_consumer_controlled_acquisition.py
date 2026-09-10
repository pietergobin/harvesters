"""Acquisition test using Consumer-allocated buffers (`announce_buffer`),
plus the safety guarantees around it: rejecting immutable `bytes`, and
keeping the backing memory alive/locked for the buffer's lifetime.
"""
from __future__ import annotations

import pytest

import gentl


NUM_BUFFERS = 4


@pytest.fixture
def consumer_buffers(stream, payload_size):
    memories = [bytearray(payload_size) for _ in range(NUM_BUFFERS)]
    buffers = [stream.announce_buffer(m) for m in memories]
    for buf in buffers:
        buf.queue()
    try:
        yield buffers, memories
    finally:
        stream.flush_queue(gentl.ACQ_QUEUE_TYPE.ALL_DISCARD)
        for buf in buffers:
            buf.revoke()


def test_announce_buffer_accepts_bytearray(consumer_buffers):
    buffers, _memories = consumer_buffers
    assert len(buffers) == NUM_BUFFERS
    for buf in buffers:
        assert isinstance(buf.handle, int)


def test_announce_buffer_rejects_immutable_bytes(stream, payload_size):
    with pytest.raises(BufferError):
        stream.announce_buffer(bytes(payload_size))


def test_full_acquisition_with_consumer_allocated_buffers(
    stream, remote_registers, payload_size, consumer_buffers
):
    buffers, _memories = consumer_buffers
    buffers_by_handle = {b.handle: b for b in buffers}

    event = stream.register_new_buffer_event()
    stream.start_acquisition(gentl.ACQ_START_FLAGS.DEFAULT, gentl.GENTL_INFINITE)
    remote_registers.execute_command("AcquisitionStart")
    try:
        buffer_handle, _user_ptr = event.get_new_buffer_data(timeout_ms=5000)
        buf = buffers_by_handle[buffer_handle]

        cmd = gentl.BUFFER_INFO_CMD
        assert buf.get_info(cmd.BUFFER_INFO_IS_INCOMPLETE) is False
        assert len(buf.get_data()) == payload_size

        buf.queue()
    finally:
        remote_registers.execute_command("AcquisitionStop")
        stream.stop_acquisition(gentl.ACQ_STOP_FLAGS.DEFAULT)
        event.flush()
