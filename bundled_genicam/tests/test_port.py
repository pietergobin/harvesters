"""Tests for the `Port` wrapper: URL discovery and raw register access.

These exercise `Device.get_port()` (the *remote device's* register map)
directly through the GenTL C interface, independent of any GenApi
implementation -- see `conftest.RegisterMap` for the minimal amount of XML
parsing used elsewhere in this suite to turn that into feature reads.
"""
from __future__ import annotations

import xml.etree.ElementTree as ET

import conftest
import gentl


def test_device_exposes_exactly_one_description_url(device):
    port = device.get_port()
    assert port.get_num_urls() == 1


def test_description_url_is_a_local_register_map_reference(device):
    port = device.get_port()
    url = port.get_url_info(0, gentl.URL_INFO_CMD.URL_INFO_URL)
    assert url.startswith("local:")

    scheme = port.get_url_info(0, gentl.URL_INFO_CMD.URL_INFO_SCHEME)
    assert scheme == gentl.URL_SCHEME_ID.LOCAL


def test_description_xml_is_well_formed_and_describes_viky(device):
    port = device.get_port()
    url = port.get_url_info(0, gentl.URL_INFO_CMD.URL_INFO_URL)
    address, length = conftest._parse_local_url(url)

    xml_bytes = port.read(address, length)
    assert len(xml_bytes) == length

    root = conftest._strip_namespaces(ET.fromstring(xml_bytes))
    assert root.tag == "RegisterDescription"
    assert root.attrib["ModelName"] == "Viky"


def test_port_info_reports_read_access(device):
    port = device.get_port()
    assert port.get_info(gentl.PORT_INFO_CMD.PORT_INFO_ACCESS_READ) is True


def test_control_port_is_a_distinct_local_module_port(device):
    remote_port = device.get_port()
    local_port = device.control_port()

    remote_module = remote_port.get_info(gentl.PORT_INFO_CMD.PORT_INFO_MODULE)
    local_module = local_port.get_info(gentl.PORT_INFO_CMD.PORT_INFO_MODULE)

    assert remote_module == "Device"
    assert local_module != remote_module


def test_register_map_resolves_payload_size_feature(remote_registers):
    value = remote_registers.read_integer("PayloadSize")
    assert isinstance(value, int)
    assert value > 0
