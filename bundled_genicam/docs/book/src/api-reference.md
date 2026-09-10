# API Reference

This is a condensed map from Python method to the underlying GenTL C
function, for quick cross-referencing with the specification bundled at
`bundled_genicam/docs/GenICam_GenTL_1_6.pdf`. For full parameter docs, see
`gentl.pyi` (or `help(gentl.<Class>)`), which is generated directly from the
same docstrings shown here.

## `gentl.Producer`

| Method | GenTL function(s) |
|---|---|
| `Producer(path)` | loads the `.cti`, resolves entry points |
| `init_lib()` / `close_lib()` | `GCInitLib` / `GCCloseLib` |
| `get_info(cmd)` | `GCGetInfo` |
| `open_system()` | `TLOpen` |
| `with Producer(...) as p:` | brackets `GCInitLib()`/`GCCloseLib()` |

## `gentl.System`

| Method | GenTL function(s) |
|---|---|
| `close()` | `TLClose` |
| `update_interface_list(timeout_ms)` | `TLUpdateInterfaceList` |
| `get_num_interfaces()` | `TLGetNumInterfaces` |
| `get_interface_id(index)` | `TLGetInterfaceID` |
| `get_interface_info(id, cmd)` | `TLGetInterfaceInfo` |
| `open_interface(id)` | `TLOpenInterface` |
| `get_info(cmd)` | `TLGetInfo` |

## `gentl.Interface`

| Method | GenTL function(s) |
|---|---|
| `close()` | `IFClose` |
| `update_device_list(timeout_ms)` | `IFUpdateDeviceList` |
| `get_num_devices()` | `IFGetNumDevices` |
| `get_device_id(index)` | `IFGetDeviceID` |
| `get_device_info(id, cmd)` | `IFGetDeviceInfo` |
| `open_device(id, flags)` | `IFOpenDevice` |
| `get_info(cmd)` | `IFGetInfo` |

## `gentl.Device`

| Method | GenTL function(s) |
|---|---|
| `close()` | `DevClose` |
| `get_port()` | `DevGetPort` (remote device's own register map) |
| `control_port()` | the `Device` handle itself used as a `PORT_HANDLE` (local module register map) |
| `get_num_data_streams()` | `DevGetNumDataStreams` |
| `get_data_stream_id(index)` | `DevGetDataStreamID` |
| `open_data_stream(id)` | `DevOpenDataStream` |
| `get_info(cmd)` | `DevGetInfo` |

## `gentl.DataStream`

| Method | GenTL function(s) |
|---|---|
| `close()` | `DSClose` |
| `announce_buffer(data, private_ptr=0)` | `DSAnnounceBuffer` |
| `alloc_and_announce_buffer(size, private_ptr=0)` | `DSAllocAndAnnounceBuffer` |
| `flush_queue(op)` | `DSFlushQueue` |
| `start_acquisition(flags, num_to_acquire)` | `DSStartAcquisition` |
| `stop_acquisition(flags)` | `DSStopAcquisition` |
| `get_info(cmd)` | `DSGetInfo` |
| `get_buffer_by_index(index)` | `DSGetBufferID` |
| `register_new_buffer_event()` | `GCRegisterEvent(EVENT_NEW_BUFFER)` |

## `gentl.Buffer`

| Method | GenTL function(s) |
|---|---|
| `queue()` | `DSQueueBuffer` |
| `revoke()` | `DSRevokeBuffer` |
| `get_info(cmd)` | `DSGetBufferInfo` |
| `get_data()` | reads `BUFFER_INFO_BASE`/`BUFFER_INFO_SIZE` and copies out the payload as `bytes` |
| `.handle` | the raw `BUFFER_HANDLE`, as an `int` |

## `gentl.Port`

| Method | GenTL function(s) |
|---|---|
| `read(address, size)` | `GCReadPort` |
| `write(address, data)` | `GCWritePort` |
| `get_info(cmd)` | `GCGetPortInfo` |
| `get_num_urls()` | `GCGetNumPortURLs` |
| `get_url_info(index, cmd)` | `GCGetPortURLInfo` |

## `gentl.Event`

| Method | GenTL function(s) |
|---|---|
| `get_data(timeout_ms=0)` | `EventGetData` (raw payload as `bytes`) |
| `get_new_buffer_data(timeout_ms=0)` | `EventGetData`, decoded as `EVENT_NEW_BUFFER_DATA` → `(buffer_handle, user_pointer)` |
| `flush()` | `EventFlush` |
| `kill()` | `EventKill` |
| `get_info(cmd)` | `EventGetInfo` |

## Module-level constants

| Name | Meaning |
|---|---|
| `GENTL_MAJOR_VERSION`, `GENTL_MINOR_VERSION`, `GENTL_SUBMINOR_VERSION` | version of the GenTL spec this header/wrapper targets (currently 1.6.0) |
| `GENTL_INFINITE` | the GenTL "infinite timeout / unbounded count" sentinel (`0xFFFFFFFFFFFFFFFF`) |
| `GENTL_INVALID_HANDLE` | the GenTL "no handle" sentinel (`0`) |

## Enums

All of the following are exposed as `enum.IntEnum` subclasses (their members
also re-exported at module scope, e.g. both `gentl.GC_ERROR.GC_ERR_SUCCESS`
and `gentl.GC_ERR_SUCCESS` work):

`GC_ERROR`, `INFO_DATATYPE`, `TL_INFO_CMD`, `INTERFACE_INFO_CMD`,
`DEVICE_ACCESS_FLAGS`, `DEVICE_ACCESS_STATUS`, `DEVICE_INFO_CMD`,
`ACQ_STOP_FLAGS`, `ACQ_START_FLAGS`, `ACQ_QUEUE_TYPE`, `STREAM_INFO_CMD`,
`BUFFER_INFO_CMD`, `PAYLOADTYPE_INFO_ID`, `PIXELFORMAT_NAMESPACE_ID`,
`PORT_INFO_CMD`, `URL_INFO_CMD`, `URL_SCHEME_ID`, `EVENT_TYPE`,
`EVENT_INFO_CMD`, `EVENT_DATA_INFO_CMD`.

See the GenTL spec chapter 6.4 for the full semantics of each command/value.
