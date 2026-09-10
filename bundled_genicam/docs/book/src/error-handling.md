# Error Handling

Every wrapped GenTL C function is checked internally; any return value other
than `GC_ERR_SUCCESS` raises `gentl.GenTLException`, a subclass of the
built-in `Exception`.

```python
try:
    system.open_interface("does-not-exist")
except gentl.GenTLException as e:
    print(e)          # human-readable message, including GCGetLastError() text
    print(e.code)     # the numeric GC_ERROR, e.g. gentl.GC_ERROR.GC_ERR_INVALID_ID
```

```text
TLOpenInterface failed with GC_ERR_INVALID_ID (-1007): does-not-exist is not
a recognized interface name. Valid interface names: 48:e1:50:e9:8a:b3_eth2,
96:a4:f7:ab:12:17_docker0
```

`e.code` compares equal to the `gentl.GC_ERROR` enum members (it *is* one),
so you can branch on specific error conditions:

```python
try:
    buf = stream.get_buffer_by_index(0)
except gentl.GenTLException as e:
    if e.code == gentl.GC_ERROR.GC_ERR_INVALID_INDEX:
        print("stream has no buffers yet")
    else:
        raise
```

## Where messages come from

The exception message is built from two pieces:

1. The C function name that failed and the symbolic name/value of the
   `GC_ERROR` it returned (this part is always present, even for Producers
   that don't implement `GCGetLastError`).
2. If the Producer implements `GCGetLastError` (GenTL spec §6.3.1.3), its
   free-form, vendor-specific description of the failure — this is usually
   the more useful part, as seen in the example above.

## Common error codes you are likely to see

| `GC_ERROR` | Typical cause |
|---|---|
| `GC_ERR_INVALID_ID` | Passed an interface/device/stream ID that doesn't exist (often because you forgot to call `update_interface_list()`/`update_device_list()` first, or the list is stale). |
| `GC_ERR_RESOURCE_IN_USE` | Tried to open something (`GCInitLib`, `TLOpen`, a device, ...) a second time without closing it first. |
| `GC_ERR_ACCESS_DENIED` | Device already opened exclusively by another process/host, or you don't have permission to access the interface. |
| `GC_ERR_INVALID_BUFFER` / `GC_ERR_BUFFER_TOO_SMALL` | Fewer/smaller buffers announced than the stream requires — check `STREAM_INFO_BUF_ANNOUNCE_MIN` and `STREAM_INFO_PAYLOAD_SIZE`. |
| `GC_ERR_TIMEOUT` | `Event.get_data()`/`get_new_buffer_data()` waited longer than `timeout_ms` without a buffer arriving — check the remote device is actually streaming. |
| `GC_ERR_NOT_IMPLEMENTED` | The Producer doesn't implement that particular optional GenTL feature/command (e.g. some `*_INFO_CMD` values, or `GCGetNumPortURLs`). |

## Cleanup is exception-safe

Because every wrapper object is a context manager, wrapping the acquisition
loop in a `try`/`finally` (or nested `with` blocks, as in the
[Producer-controlled buffers guide](./producer-controlled-buffers.md)) is
enough to guarantee `DSStopAcquisition`/`DSClose`/`IFClose`/`TLClose`/
`GCCloseLib` are still called if a frame grab raises mid-loop:

```python
with gentl.Producer(cti_path) as producer:
    with producer.open_system() as system:
        ...
        # if anything below raises, __exit__ on every open `with` block
        # still runs, in reverse order, closing every handle cleanly.
```
