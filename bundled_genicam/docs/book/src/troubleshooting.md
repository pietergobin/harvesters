# Troubleshooting

## `RuntimeError: Failed to load GenTL Producer: ...`

The path passed to `gentl.Producer(path)` doesn't exist, isn't readable, or
isn't a valid shared library for your platform/architecture (e.g. a 32-bit
`.cti` loaded from a 64-bit Python, or vice versa). The underlying loader
error (`dlerror()`/`GetLastError()`) is included in the message — read it
carefully, it usually names the exact missing symbol or ABI mismatch.

## `num_devices` / `num_interfaces` is always `0`

Almost always one of:

- You forgot to call `update_interface_list()` / `update_device_list()`
  before `get_num_interfaces()` / `get_num_devices()` — these lists are
  snapshots that never refresh implicitly (GenTL spec, chapter 3).
- The interface really has no devices attached, or your OS user lacks
  permission to see them (common for USB3 Vision on Linux without the
  right udev rules, or GigE Vision behind a firewall/VLAN).
- You're enumerating the wrong interface (many Producers expose interfaces
  for network cards that have nothing to do with your camera, e.g. a
  Docker bridge or loopback interface — check
  `get_interface_info(id, gentl.INTERFACE_INFO_CMD.INTERFACE_INFO_DISPLAYNAME)`).

## `gentl.GenTLException: ... GC_ERR_ACCESS_DENIED`

Something else already has the device open. Common causes: another Python
process still holding the `Device`, a vendor GUI tool (e.g. a camera
viewer/IP-config tool) running in the background, or you requested
`DEVICE_ACCESS_FLAGS.EXCLUSIVE` on a device someone else opened with
`CONTROL`/`EXCLUSIVE` access already. Try `DEVICE_ACCESS_FLAGS.READONLY`
first to confirm the device is reachable at all.

## `Event.get_data()` / `get_new_buffer_data()` times out

- Confirm you called `start_acquisition()` *after*
  `register_new_buffer_event()` (chapter 4.2.3.1: events fired before
  registration are silently dropped).
- Confirm the *remote device* itself was told to start streaming (the
  `AcquisitionStart` GenApi feature) — `DataStream.start_acquisition()`
  only starts the **host-side** acquisition engine (GenTL spec §5.2.5); the
  camera also needs to be told to send data, which happens through its
  GenApi node map (`Device.get_port()`), not through this module.
- Confirm you queued at least one buffer (`buf.queue()`) — an empty input
  pool stalls acquisition indefinitely (chapter 5.1.2).

## Program hangs or crashes on exit

If you're not using `with` blocks, make sure you close things in the
correct order and don't skip a level: `Buffer.revoke()` (all of them) →
`DataStream.close()` → `Device.close()` → `Interface.close()` →
`System.close()` → `Producer.close_lib()`. A `Buffer` that is still queued
cannot be revoked (`GC_ERR_BUSY`); call `stream.stop_acquisition()` and
`event.flush()` first.

If you suspect the crash is inside the module itself rather than in
Producer/driver code, rebuild in `Debug` mode for a much more actionable
nanobind error message:

```bash
cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug -j$(nproc)
```

## Diagnosing native crashes without a debugger

If a native crash produces a core dump and your shell hangs for a long time
before reporting it, that's usually the OS writing out a (potentially huge)
core file, not an actual deadlock — disable core dumps while iterating:

```bash
ulimit -c 0
python your_script.py
```
