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

## Checking which GenTL version a Producer supports

Version negotiation in GenTL is **one-directional**: the *Consumer* queries
the *Producer*; a Producer never asks the Consumer what it was built against.
That is deliberate — Producers are required to be backward compatible, so
"which GenTL version does this `.cti` implement?" is the only question that
matters at runtime, and it is answered by the Producer's **System module**
via `get_info` (GenTL spec, `TL_INFO_GENTL_VER_MAJOR`/`MINOR`, GenTL v1.5):

```python
with gentl.Producer(cti_path) as producer:
    major = producer.get_info(gentl.TL_INFO_CMD.TL_INFO_GENTL_VER_MAJOR)
    minor = producer.get_info(gentl.TL_INFO_CMD.TL_INFO_GENTL_VER_MINOR)
    print(f"Producer complies with GenTL {major}.{minor}")
    print("Producer release:", producer.get_info(gentl.TL_INFO_CMD.TL_INFO_VERSION))
```

For example, the Viky reference Producer reports:

```
Producer complies with GenTL 1.6
Producer release: 4.0
```

### Don't confuse module constants with the Producer's version

The module-level constants `GENTL_MAJOR_VERSION`, `GENTL_MINOR_VERSION` and
`GENTL_SUBMINOR_VERSION` are **compile-time**: they record the version of the
GenTL *header* the binding was built against (for `gentl` that is 1.6, for
`gentl15` it is 1.5). They say nothing about any particular `.cti` you load.

The Producer's version, on the other hand, is discovered at **runtime** and
is independent of the binding. Because Producers must be backward
compatible, a version mismatch in either direction is normal and harmless —
a GenTL 1.6 Producer (like Viky) works fine through the `gentl15` module,
which tells you it was built against GenTL 1.5 while the Producer reports
1.6.

### SFNC alternative

The same information is also exposed as GenApi features on the System
module: `GenTLVersionMajor`/`GenTLVersionMinor` (see the GenICam GenTL SFNC
document, "System Information"). Reading those requires going through the
System's GenApi port, so the `TL_INFO_*` query above is the simpler, direct
route.

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
