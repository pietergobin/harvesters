# Quick Start

This example loads a Producer, opens the first System → Interface → Device
it finds, and prints some basic information. It doesn't grab any images yet
— see the [next chapter](./producer-controlled-buffers.md) for that.

```python
import gentl

CTI_PATH = "/opt/ImpactAcquire/lib/x86_64/mvGenTLProducer.cti"

with gentl.Producer(CTI_PATH) as producer:
    print("Vendor:", producer.get_info(gentl.TL_INFO_CMD.TL_INFO_VENDOR))

    with producer.open_system() as system:
        system.update_interface_list(timeout_ms=1000)

        num_interfaces = system.get_num_interfaces()
        if num_interfaces == 0:
            raise RuntimeError("No interfaces found")

        interface_id = system.get_interface_id(0)
        print("Using interface:", interface_id)

        with system.open_interface(interface_id) as iface:
            iface.update_device_list(timeout_ms=1000)

            num_devices = iface.get_num_devices()
            if num_devices == 0:
                raise RuntimeError("No devices found on interface")

            device_id = iface.get_device_id(0)
            model = iface.get_device_info(
                device_id, gentl.DEVICE_INFO_CMD.DEVICE_INFO_MODEL
            )
            print(f"Opening device {device_id} ({model})")

            with iface.open_device(
                device_id, gentl.DEVICE_ACCESS_FLAGS.EXCLUSIVE
            ) as device:
                num_streams = device.get_num_data_streams()
                print("Data streams available:", num_streams)
```

Running this against a producer with no cameras attached (as in CI) still
exercises the whole enumeration path and prints something like:

```text
Vendor: Balluff
Using interface: 48:e1:50:e9:8a:b3_eth2
No devices found on interface
```

## Accessing GenApi feature nodes

`Device.get_port()` returns the `Port` for the **remote device's** register
map (`DevGetPort`), which is what you feed to a GenApi node-map
implementation (e.g. `genicam-python`, or Harvesters' own internals) in
order to read/write standard SFNC features such as `Width`, `PixelFormat`,
or `AcquisitionStart`:

```python
port = device.get_port()
raw_xml = port.read(address=0, size=...)  # typically driven by GCGetPortURL/URL info
```

In practice you will resolve the XML location via `Port.get_num_urls()` /
`Port.get_url_info(index, cmd)` as described in GenTL chapter 4.1.2, then
feed the resulting `Port` object into a full GenApi implementation. `gentl`
deliberately stays at the C-interface level and does not implement GenApi
itself — its job is only to get you a valid, GenTL-conformant `Port`.
