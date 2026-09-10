"""Integration tests for error handling and the module hierarchy's
open/close discipline, all against the Viky simulator.
"""
from __future__ import annotations

import pytest

import gentl


def test_get_info_before_init_lib_raises_not_initialized(cti_path):
    p = gentl.Producer(cti_path)
    # Deliberately not calling p.init_lib() here.
    with pytest.raises(gentl.GenTLException) as exc_info:
        p.get_info(gentl.TL_INFO_CMD.TL_INFO_VENDOR)
    assert exc_info.value.code == gentl.GC_ERROR.GC_ERR_NOT_INITIALIZED


def test_producer_reports_vendor_and_model(producer):
    vendor = producer.get_info(gentl.TL_INFO_CMD.TL_INFO_VENDOR)
    model = producer.get_info(gentl.TL_INFO_CMD.TL_INFO_MODEL)
    assert isinstance(vendor, str) and vendor
    assert model == "Viky"


def test_open_system_after_init_lib_works(producer):
    system = producer.open_system()
    try:
        assert isinstance(
            system.get_info(gentl.TL_INFO_CMD.TL_INFO_ID), str
        )
    finally:
        system.close()


def test_system_enumerates_at_least_one_interface(system):
    system.update_interface_list(gentl.GENTL_INFINITE)
    assert system.get_num_interfaces() >= 1


def test_open_interface_with_unknown_id_raises_invalid_id(system):
    with pytest.raises(gentl.GenTLException) as exc_info:
        system.open_interface("this-interface-does-not-exist")
    assert exc_info.value.code == gentl.GC_ERROR.GC_ERR_INVALID_ID


def test_interface_enumerates_viky_color_devices(interface):
    device_ids = {
        interface.get_device_id(i) for i in range(interface.get_num_devices())
    }
    assert device_ids == {
        "VikyTL_DEV_red",
        "VikyTL_DEV_green",
        "VikyTL_DEV_blue",
    }


def test_get_device_info_reports_vendor_and_model(interface, device_id):
    model = interface.get_device_info(
        device_id, gentl.DEVICE_INFO_CMD.DEVICE_INFO_MODEL
    )
    vendor = interface.get_device_info(
        device_id, gentl.DEVICE_INFO_CMD.DEVICE_INFO_VENDOR
    )
    assert model == "Viky"
    assert vendor


def test_opening_same_device_twice_raises(interface, device_id):
    first = interface.open_device(
        device_id, gentl.DEVICE_ACCESS_FLAGS.EXCLUSIVE
    )
    try:
        with pytest.raises(gentl.GenTLException) as exc_info:
            interface.open_device(
                device_id, gentl.DEVICE_ACCESS_FLAGS.EXCLUSIVE
            )
        # GenTL spec 3.4: a device already opened within the same process
        # must yield an error on the second attempt.
        assert exc_info.value.code == gentl.GC_ERROR.GC_ERR_RESOURCE_IN_USE
    finally:
        first.close()


def test_device_reopens_cleanly_after_close(interface, device_id):
    first = interface.open_device(
        device_id, gentl.DEVICE_ACCESS_FLAGS.EXCLUSIVE
    )
    first.close()

    # Proves DevClose() actually released the handle -- this would raise
    # GC_ERR_RESOURCE_IN_USE if it hadn't.
    second = interface.open_device(
        device_id, gentl.DEVICE_ACCESS_FLAGS.EXCLUSIVE
    )
    second.close()


def test_device_has_exactly_one_data_stream(device):
    assert device.get_num_data_streams() == 1


def test_data_stream_reports_stream_info(stream):
    assert stream.get_info(gentl.STREAM_INFO_CMD.STREAM_INFO_IS_GRABBING) is False
    assert isinstance(
        stream.get_info(gentl.STREAM_INFO_CMD.STREAM_INFO_ID), str
    )


def test_data_stream_does_not_define_payload_size(stream):
    # Documented gotcha (see the user guide): Viky reports the payload size
    # via the remote device's GenApi "PayloadSize" feature, not via
    # STREAM_INFO_PAYLOAD_SIZE.
    assert (
        stream.get_info(gentl.STREAM_INFO_CMD.STREAM_INFO_DEFINES_PAYLOADSIZE)
        is False
    )
    with pytest.raises(gentl.GenTLException) as exc_info:
        stream.get_info(gentl.STREAM_INFO_CMD.STREAM_INFO_PAYLOAD_SIZE)
    assert exc_info.value.code == gentl.GC_ERROR.GC_ERR_NOT_AVAILABLE
