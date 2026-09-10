"""Full acquisition test, Producer-controlled buffer design.

This is the automated counterpart of the "Guide: Producer-Controlled
Buffers" chapter in the user documentation: buffers are allocated *by the
Producer* via `alloc_and_announce_buffer()`, queued, and the acquisition
engine is driven end-to-end, including telling the *remote device* (via its
GenApi `AcquisitionStart`/`AcquisitionStop` commands) to actually stream --
`DataStream.start_acquisition()` alone only starts the host-side engine
(GenTL spec, chapter 5.2.5).
"""
from __future__ import annotations

import pytest

import gentl


NUM_BUFFERS = 4
NUM_FRAMES_TO_GRAB = 5


@pytest.fixture
def acquiring_stream(stream, remote_registers, payload_size):
    """A DataStream with Producer-allocated buffers queued and the whole
    acquisition chain (host + remote device) started; yields
    `(stream, buffers_by_handle, new_buffer_event)`. Always stopped and
    cleaned up on exit, even if the test raises."""
    buffers = []
    for _ in range(NUM_BUFFERS):
        buf = stream.alloc_and_announce_buffer(payload_size)
        buf.queue()
        buffers.append(buf)
    buffers_by_handle = {b.handle: b for b in buffers}

    event = stream.register_new_buffer_event()
    stream.start_acquisition(gentl.ACQ_START_FLAGS.DEFAULT, gentl.GENTL_INFINITE)
    remote_registers.execute_command("AcquisitionStart")

    try:
        yield stream, buffers_by_handle, event
    finally:
        remote_registers.execute_command("AcquisitionStop")
        stream.stop_acquisition(gentl.ACQ_STOP_FLAGS.DEFAULT)
        event.flush()
        # A buffer still sitting in the input pool/output queue cannot be
        # revoked (GenTL spec 5.2.9); discard whatever is left first.
        stream.flush_queue(gentl.ACQ_QUEUE_TYPE.ALL_DISCARD)
        for buf in buffers:
            buf.revoke()


def test_payload_size_is_reported_and_positive(payload_size):
    assert isinstance(payload_size, int)
    assert payload_size > 0


def test_grabs_expected_number_of_complete_frames(acquiring_stream):
    _stream, buffers_by_handle, event = acquiring_stream

    frame_ids = []
    for _ in range(NUM_FRAMES_TO_GRAB):
        buffer_handle, _user_ptr = event.get_new_buffer_data(timeout_ms=5000)
        buf = buffers_by_handle[buffer_handle]

        cmd = gentl.BUFFER_INFO_CMD
        assert buf.get_info(cmd.BUFFER_INFO_IS_INCOMPLETE) is False

        frame_ids.append(buf.get_info(cmd.BUFFER_INFO_FRAMEID))

        buf.queue()

    assert len(frame_ids) == NUM_FRAMES_TO_GRAB
    # Frame IDs must be strictly increasing (GenTL spec, BUFFER_INFO_FRAMEID).
    assert frame_ids == sorted(set(frame_ids))
    assert len(set(frame_ids)) == NUM_FRAMES_TO_GRAB


def test_frame_dimensions_match_remote_device_registers(
    acquiring_stream, remote_registers
):
    _stream, buffers_by_handle, event = acquiring_stream
    expected_width = remote_registers.read_integer("Width")
    expected_height = remote_registers.read_integer("Height")

    buffer_handle, _ = event.get_new_buffer_data(timeout_ms=5000)
    buf = buffers_by_handle[buffer_handle]

    cmd = gentl.BUFFER_INFO_CMD
    assert buf.get_info(cmd.BUFFER_INFO_WIDTH) == expected_width
    assert buf.get_info(cmd.BUFFER_INFO_HEIGHT) == expected_height
    buf.queue()


def test_get_data_returns_full_payload_as_bytes(acquiring_stream, payload_size):
    _stream, buffers_by_handle, event = acquiring_stream

    buffer_handle, _ = event.get_new_buffer_data(timeout_ms=5000)
    buf = buffers_by_handle[buffer_handle]

    data = buf.get_data()
    assert isinstance(data, bytes)
    assert len(data) == payload_size
    # Sanity check that the simulator actually put non-trivial data in
    # there (not an all-zero, never-written buffer).
    assert any(b != 0 for b in data)

    buf.queue()


def test_revoke_while_queued_raises_busy(stream, payload_size):
    buf = stream.alloc_and_announce_buffer(payload_size)
    buf.queue()
    try:
        with pytest.raises(gentl.GenTLException) as exc_info:
            buf.revoke()
        assert exc_info.value.code == gentl.GC_ERROR.GC_ERR_BUSY
    finally:
        stream.flush_queue(gentl.ACQ_QUEUE_TYPE.ALL_DISCARD)
        buf.revoke()


def test_get_new_buffer_data_times_out_without_acquisition(stream, payload_size):
    buf = stream.alloc_and_announce_buffer(payload_size)
    buf.queue()
    event = stream.register_new_buffer_event()
    try:
        # Acquisition was never started -- no buffer should ever arrive.
        with pytest.raises(gentl.GenTLException) as exc_info:
            event.get_new_buffer_data(timeout_ms=200)
        assert exc_info.value.code == gentl.GC_ERROR.GC_ERR_TIMEOUT
    finally:
        stream.flush_queue(gentl.ACQ_QUEUE_TYPE.ALL_DISCARD)
        buf.revoke()
