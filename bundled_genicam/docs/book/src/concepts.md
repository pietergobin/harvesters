# Core Concepts

## Object model

| Class | Wraps | Obtained from |
|---|---|---|
| `gentl.Producer` | the loaded `.cti` library | `gentl.Producer(path)` |
| `gentl.System` | `TL_HANDLE` | `Producer.open_system()` |
| `gentl.Interface` | `IF_HANDLE` | `System.open_interface(id)` |
| `gentl.Device` | `DEV_HANDLE` | `Interface.open_device(id)` |
| `gentl.DataStream` | `DS_HANDLE` | `Device.open_data_stream(id)` |
| `gentl.Buffer` | `BUFFER_HANDLE` | `DataStream.announce_buffer(...)` / `alloc_and_announce_buffer(...)` |
| `gentl.Port` | `PORT_HANDLE` | `Device.get_port()`, `Device.control_port()`, ... |
| `gentl.Event` | `EVENT_HANDLE` | `DataStream.register_new_buffer_event()` |

Every level except `Buffer`/`Event`/`Port` is a **context manager**: entering
it is a no-op (or, for `Producer`, calls `GCInitLib`), and exiting it calls
the matching GenTL `*Close` function for you (`TLClose`, `IFClose`,
`DevClose`, `DSClose`, `GCCloseLib`). Using `with` blocks is the recommended
way to guarantee cleanup even when an exception is raised mid-acquisition.

```python
with gentl.Producer(cti_path) as producer:
    with producer.open_system() as system:
        with system.open_interface(iface_id) as iface:
            with iface.open_device(device_id) as device:
                ...
```

## Enumeration lists never auto-refresh

Per the GenTL spec (chapter 3), the list of interfaces on a `System` and the
list of devices on an `Interface` are **snapshots**. They only change when
you explicitly call:

- `System.update_interface_list(timeout_ms)`
- `Interface.update_device_list(timeout_ms)`

before calling `get_num_interfaces()`/`get_interface_id(i)` or
`get_num_devices()`/`get_device_id(i)`. Forgetting this is the most common
source of "0 devices found" bugs.

## `get_info()` and the `INFO_DATATYPE` decoding

Almost every object exposes a `get_info(cmd)` method mirroring the GenTL
`*GetInfo` family of C functions (`TLGetInfo`, `IFGetInfo`, `DevGetInfo`,
`DSGetInfo`, `DSGetBufferInfo`, `GCGetPortInfo`, `EventGetInfo`, ...). These
functions report both a value *and* its `INFO_DATATYPE` (string, bool,
various integer widths, a raw buffer, ...). The Python wrapper does that
two-step "query size, then fetch" dance for you internally and returns the
most natural Python object for the reported type:

| `INFO_DATATYPE` | Python type |
|---|---|
| `STRING` | `str` |
| `STRINGLIST` | `list[str]` |
| `INT16`/`UINT16`/`INT32`/`UINT32`/`INT64`/`UINT64`/`SIZET`/`PTRDIFF`/`PTR` | `int` |
| `FLOAT64` | `float` |
| `BOOL8` | `bool` |
| `BUFFER` (or anything else) | `bytes` |

```python
vendor: str = producer.get_info(gentl.TL_INFO_CMD.TL_INFO_VENDOR)
is_grabbing: bool = stream.get_info(gentl.STREAM_INFO_CMD.STREAM_INFO_IS_GRABBING)
payload_size: int = stream.get_info(gentl.STREAM_INFO_CMD.STREAM_INFO_PAYLOAD_SIZE)
```

## Error handling

Every GenTL C function returns a `GC_ERROR` code. The wrapper checks it for
you and raises `gentl.GenTLException` (a subclass of `Exception`) on any
non-`GC_ERR_SUCCESS` result. See [Error Handling](./error-handling.md) for
details, including how to inspect the numeric `GC_ERROR` code.

## Buffer ownership: Consumer- vs Producer-allocated

`DataStream` supports both of GenTL's buffer announcement strategies
(GenTL spec, chapter 3.6):

- **Consumer-allocated** — you provide the memory (as any writable
  buffer-protocol object, e.g. `bytearray`) via
  `DataStream.announce_buffer(data, private_ptr)`. You are responsible for
  keeping the underlying buffer alive and free'd.
- **Producer-allocated ("Producer-controlled")** — the Producer allocates
  and owns the memory via `DataStream.alloc_and_announce_buffer(size,
  private_ptr)`. This is generally the *simplest and most portable* choice,
  since the Producer knows about any DMA/alignment/driver-specific
  requirements for the buffer memory. This is the mode covered in detail in
  the [next chapter](./producer-controlled-buffers.md).

A single `DataStream` must not mix the two strategies (GenTL spec, 5.2.2).
