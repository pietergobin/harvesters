# Guide: Producer-Controlled Buffers

This is the recommended acquisition mode for a first integration: the
**Producer** allocates and owns every buffer's memory
(`DSAllocAndAnnounceBuffer`, GenTL spec §5.2.1–§5.2.2), so you never have to
worry about DMA-friendly alignment, page-locking, or any other
driver-specific memory requirement — the Producer already knows what it
needs and takes care of it for you. The alternative, Consumer-allocated
buffers (`announce_buffer`), is covered briefly at the end of this chapter.

The acquisition sequence below follows the GenTL "acquisition chain"
(spec chapter 5.2) step by step:

1. Query the required buffer size from the **stream**, not the device
   (`STREAM_INFO_PAYLOAD_SIZE`).
2. Allocate a handful of buffers via `alloc_and_announce_buffer()` and
   `queue()` them into the input pool.
3. Register for the `EVENT_NEW_BUFFER` notification.
4. `start_acquisition()`.
5. In a loop: wait for the event, read out the delivered `Buffer`, copy its
   payload, then `queue()` it again so the acquisition engine can reuse it.
6. `stop_acquisition()`, `revoke()` every buffer, and close everything down.

## A minimal `ImageAcquisition` helper

```python
"""Producer-controlled-buffer acquisition example for gentl.

Usage:
    python producer_controlled_acquisition.py /path/to/producer.cti
"""
from __future__ import annotations

import sys
from dataclasses import dataclass

import gentl


@dataclass
class Frame:
    data: bytes
    width: int
    height: int
    pixel_format: int
    timestamp: int
    frame_id: int
    incomplete: bool


class ImageAcquisition:
    """Grabs frames using GenTL's Producer-controlled buffer design.

    Every `Buffer` announced here was allocated *by the Producer* via
    `DSAllocAndAnnounceBuffer` -- we never touch raw pointers ourselves,
    we only ask the Producer to copy each buffer's payload out as
    `bytes` (`Buffer.get_data()`).
    """

    def __init__(
        self,
        cti_path: str,
        device_id: str | None = None,
        num_buffers: int = 4,
    ) -> None:
        self._producer = gentl.Producer(cti_path)
        self._producer.init_lib()

        self._system = self._producer.open_system()
        self._interface, self._device = self._find_device(device_id)

        stream_id = self._device.get_data_stream_id(0)
        self._stream = self._device.open_data_stream(stream_id)

        # Ask the *Data Stream* module how big a buffer needs to be. Per
        # GenTL 5.2.1, always check STREAM_INFO_DEFINES_PAYLOADSIZE first;
        # if it is False the payload size instead has to be read from the
        # remote device's "PayloadSize" GenApi feature (out of scope here --
        # most modern Producers report True).
        defines_payload_size = self._stream.get_info(
            gentl.STREAM_INFO_CMD.STREAM_INFO_DEFINES_PAYLOADSIZE
        )
        if not defines_payload_size:
            raise RuntimeError(
                "This Producer does not report the payload size; read "
                "the 'PayloadSize' feature from the remote device's "
                "GenApi node map instead (see device.get_port())."
            )
        payload_size = self._stream.get_info(
            gentl.STREAM_INFO_CMD.STREAM_INFO_PAYLOAD_SIZE
        )

        # Step 1-2: allocate + announce + queue Producer-controlled buffers.
        self._buffers: dict[int, gentl.Buffer] = {}
        for _ in range(num_buffers):
            buf = self._stream.alloc_and_announce_buffer(payload_size)
            buf.queue()
            self._buffers[buf.handle] = buf

        # Step 3: register for new-buffer notifications.
        self._new_buffer_event = self._stream.register_new_buffer_event()

    def _find_device(
        self, device_id: str | None
    ) -> tuple[gentl.Interface, gentl.Device]:
        self._system.update_interface_list(gentl.GENTL_INFINITE)
        for i in range(self._system.get_num_interfaces()):
            iface_id = self._system.get_interface_id(i)
            iface = self._system.open_interface(iface_id)
            iface.update_device_list(gentl.GENTL_INFINITE)
            if iface.get_num_devices() == 0:
                iface.close()
                continue
            did = device_id or iface.get_device_id(0)
            device = iface.open_device(did, gentl.DEVICE_ACCESS_FLAGS.EXCLUSIVE)
            return iface, device
        raise RuntimeError("No GenTL device found")

    def start(self) -> None:
        # Step 4: start the *host-side* acquisition engine. Starting the
        # remote device's own acquisition (the "AcquisitionStart" GenApi
        # feature) is a separate step performed through the device's GenApi
        # node map, not shown here.
        self._stream.start_acquisition(
            gentl.ACQ_START_FLAGS.DEFAULT, gentl.GENTL_INFINITE
        )

    def grab(self, timeout_ms: int = 5000) -> Frame:
        # Step 5: wait for EVENT_NEW_BUFFER, and look up which of our
        # announced Buffer objects it refers to.
        buffer_handle, _user_ptr = self._new_buffer_event.get_new_buffer_data(
            timeout_ms
        )
        buf = self._buffers[buffer_handle]

        cmd = gentl.BUFFER_INFO_CMD
        frame = Frame(
            data=buf.get_data(),
            width=buf.get_info(cmd.BUFFER_INFO_WIDTH),
            height=buf.get_info(cmd.BUFFER_INFO_HEIGHT),
            pixel_format=buf.get_info(cmd.BUFFER_INFO_PIXELFORMAT),
            timestamp=buf.get_info(cmd.BUFFER_INFO_TIMESTAMP),
            frame_id=buf.get_info(cmd.BUFFER_INFO_FRAMEID),
            incomplete=buf.get_info(cmd.BUFFER_INFO_IS_INCOMPLETE),
        )

        # Give the buffer back to the acquisition engine immediately -- do
        # this *after* get_data() copied out the payload we need.
        buf.queue()
        return frame

    def stop(self) -> None:
        self._stream.stop_acquisition(gentl.ACQ_STOP_FLAGS.DEFAULT)
        self._new_buffer_event.flush()

    def close(self) -> None:
        # Step 6: buffers must be revoked before the stream can be closed;
        # since the Producer owns the memory (alloc_and_announce_buffer),
        # revoke() here is only bookkeeping -- we must NOT free anything
        # ourselves.
        for buf in self._buffers.values():
            buf.revoke()
        self._buffers.clear()

        self._stream.close()
        self._device.close()
        self._interface.close()
        self._system.close()
        self._producer.close_lib()

    def __enter__(self) -> "ImageAcquisition":
        return self

    def __exit__(self, *exc_info) -> None:
        self.close()


def main() -> None:
    cti_path = sys.argv[1] if len(sys.argv) > 1 else (
        "/opt/ImpactAcquire/lib/x86_64/mvGenTLProducer.cti"
    )

    with ImageAcquisition(cti_path, num_buffers=4) as acquisition:
        acquisition.start()
        try:
            for i in range(10):
                frame = acquisition.grab(timeout_ms=5000)
                status = "INCOMPLETE" if frame.incomplete else "ok"
                print(
                    f"frame #{frame.frame_id} [{status}]: "
                    f"{frame.width}x{frame.height}, "
                    f"{len(frame.data)} bytes, t={frame.timestamp}"
                )
        finally:
            acquisition.stop()


if __name__ == "__main__":
    main()
```

## Why this order matters

A few details in the example above are load-bearing, not stylistic:

- **Query `STREAM_INFO_PAYLOAD_SIZE` on the stream, not the device.**
  GenTL explicitly separates "what the Producer needs the buffer to be" from
  "what the remote device's GenApi `PayloadSize` feature reports" — they can
  differ if the Producer does any preprocessing (chapter 5.2.1, 5.5). Always
  check `STREAM_INFO_DEFINES_PAYLOADSIZE` first.
- **Queue every buffer before calling `start_acquisition()`.** Buffers must
  be in the *input pool* for the acquisition engine to use them (chapter
  5.2.3); an empty input pool stalls acquisition.
- **`register_new_buffer_event()` before `start_acquisition()`.** Events
  that fire before you've registered for them are silently discarded
  (chapter 4.2.3.1).
- **Re-`queue()` a buffer only after you're done reading it.** Once queued,
  the acquisition engine is free to overwrite it (chapter 5.1.3); call
  `buf.get_data()` (which copies the payload out) *before* `buf.queue()`.
- **`revoke()` every buffer before closing the stream.** A queued buffer
  cannot be revoked (chapter 5.2.9); this example never revokes without
  first calling `stop_acquisition()` + `flush()`, both of which move any
  in-flight buffers out of the queues.
- **Never `free()` a Producer-allocated buffer yourself.** Memory obtained
  via `alloc_and_announce_buffer()` is owned by the Producer and released
  automatically when you `revoke()` it (chapter 5.2.10). This is precisely
  the appeal of the Producer-controlled design: no manual memory management
  on the Python side at all.

## For comparison: Consumer-allocated buffers

If you need buffers backed by memory *you* control (e.g. a `numpy` array, a
pinned/page-locked allocation for a GPU pipeline, or memory shared with
another process), use `announce_buffer()` instead:

```python
payload_size = stream.get_info(gentl.STREAM_INFO_CMD.STREAM_INFO_PAYLOAD_SIZE)

buffers = []
for _ in range(4):
    memory = bytearray(payload_size)          # you own this memory
    buf = stream.announce_buffer(bytes(memory))
    buf.queue()
    buffers.append((buf, memory))
```

Do not mix `announce_buffer()` and `alloc_and_announce_buffer()` on the same
`DataStream` (GenTL spec §5.2.2) — pick one strategy per stream.
