"""Shared pytest fixtures for the ``gentl15`` test suite.

This is the GenTL 1.5 counterpart of ``tests/conftest.py``: it exercises the
``gentl15`` nanobind extension against the *Viky* GenTL Producer simulator
("world first game playing GenTL Producer"), which ships inside the official
``genicam`` PyPI package (a mandatory dependency of the ``harvesters``
project itself). Viky is a fully spec-conformant, hardware-free reference
Producer exposing three virtual color devices (``VikyTL_DEV_red`` /
``_green`` / ``_blue``). Although Viky implements the GenTL 1.6 interface,
producers are required to stay backward compatible with older clients, which
makes it a valid target for the 1.5-level ``gentl15`` binding.

Important gotcha these fixtures work around: ``GCInitLib``/``GCCloseLib``
and the System handle are **process-global** state inside the Producer
shared library, not per Python-object state (see GenTL spec, chapter 3.1
and 3.2: "the System module does no reference counting within a single
process"). We handle this by running every test in its own forked subprocess
via ``pytest-forked`` (configured globally in ``pyproject.toml``), which
also contains any native crash to a single failing test instead of aborting
the whole run.
"""
from __future__ import annotations

import glob
import os
import struct
import sys
import xml.etree.ElementTree as ET
from dataclasses import dataclass
from pathlib import Path

import pytest

# ---------------------------------------------------------------------------
# Make the freshly built extension importable without installing it.
# ---------------------------------------------------------------------------

_BUNDLED_GENICAM_DIR = Path(__file__).resolve().parent.parent
_DEFAULT_BUILD_DIR = _BUNDLED_GENICAM_DIR / "build"
_BUILD_DIR = Path(os.environ.get("GENTL_BUILD_DIR", _DEFAULT_BUILD_DIR))

if str(_BUILD_DIR) not in sys.path:
    sys.path.insert(0, str(_BUILD_DIR))

try:
    import gentl15  # noqa: E402  (import after sys.path manipulation)
except ImportError as exc:  # pragma: no cover - environment problem, not a test failure
    raise ImportError(
        "Could not import the 'gentl15' extension module. Build it first:\n"
        "    cd bundled_genicam && cmake -S . -B build && cmake --build build\n"
        f"(looked in: {_BUILD_DIR}, override with GENTL_BUILD_DIR env var)"
    ) from exc


# ---------------------------------------------------------------------------
# Locating the Viky simulator Producer
# ---------------------------------------------------------------------------

def _discover_viky_cti() -> str | None:
    """Locate viky.cti, honoring GENTL_TEST_CTI as an override."""
    override = os.environ.get("GENTL_TEST_CTI")
    if override:
        return override

    try:
        import genicam
    except ImportError:
        return None

    matches = glob.glob(
        os.path.join(os.path.dirname(genicam.__file__), "viky.cti")
    )
    return matches[0] if matches else None


@pytest.fixture(scope="session")
def cti_path() -> str:
    path = _discover_viky_cti()
    if not path:
        pytest.skip(
            "viky.cti not found; install the 'genicam' package or set "
            "GENTL_TEST_CTI to a valid GenTL Producer path."
        )
    return path


# ---------------------------------------------------------------------------
# GenTL module hierarchy fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def producer(cti_path):
    p = gentl15.Producer(cti_path)
    p.init_lib()
    try:
        yield p
    finally:
        p.close_lib()


@pytest.fixture
def system(producer):
    sysm = producer.open_system()
    sysm.update_interface_list(gentl15.GENTL_INFINITE)
    try:
        yield sysm
    finally:
        sysm.close()


@pytest.fixture
def interface(system):
    """The first Interface that has at least one Device attached."""
    for i in range(system.get_num_interfaces()):
        iface_id = system.get_interface_id(i)
        iface = system.open_interface(iface_id)
        iface.update_device_list(gentl15.GENTL_INFINITE)
        if iface.get_num_devices() > 0:
            try:
                yield iface
            finally:
                iface.close()
            return
        iface.close()
    pytest.skip("No interface with at least one device was found")


@pytest.fixture
def device_id(interface) -> str:
    return interface.get_device_id(0)


@pytest.fixture
def device(interface, device_id):
    dev = interface.open_device(device_id, gentl15.DEVICE_ACCESS_FLAGS.EXCLUSIVE)
    try:
        yield dev
    finally:
        dev.close()


@pytest.fixture
def stream_id(device) -> str:
    return device.get_data_stream_id(0)


@pytest.fixture
def stream(device, stream_id):
    s = device.open_data_stream(stream_id)
    try:
        yield s
    finally:
        # Defensive teardown: if a test forgot to stop/flush, make sure a
        # still-queued buffer doesn't prevent DSClose from doing its job.
        try:
            s.flush_queue(gentl15.ACQ_QUEUE_TYPE.ALL_DISCARD)
        except gentl15.GenTLException:
            pass
        s.close()


# ---------------------------------------------------------------------------
# A tiny, self-contained GenApi-register reader/writer.
#
# `gentl15` deliberately stops at the GenTL C interface and does not
# implement a GenApi node-map (that's a separate library's job -- see the
# "Accessing GenApi feature nodes" section of the user guide). To exercise
# real image acquisition in these tests we still need to read a couple of
# standard SFNC features (PayloadSize, Width, Height) and execute two
# commands (AcquisitionStart/Stop) on the remote device, so this helper does
# the minimal amount of GenApi XML parsing needed for that -- resolving a
# feature name to its backing register via <pValue>, purely through
# `Port.read()`/`Port.write()` and Python's standard library.
# ---------------------------------------------------------------------------

def _strip_namespaces(elem: ET.Element) -> ET.Element:
    for el in elem.iter():
        if "}" in el.tag:
            el.tag = el.tag.split("}", 1)[1]
    return elem


def _parse_local_url(url: str) -> tuple[int, int]:
    # "local:[///]filename.ext;address;length[?SchemaVersion=x.x.x]"
    # (GenTL spec, chapter 4.1.2.1)
    assert url.startswith("local:"), f"unsupported URL scheme: {url}"
    body = url[len("local:"):].lstrip("/")
    _filename, address, length = body.split("?")[0].split(";")
    return int(address, 16), int(length, 16)


@dataclass
class RegisterMap:
    """Resolves SFNC feature names to registers on a GenTL `Port`, and
    reads/writes/executes them -- just enough GenApi to drive acquisition
    in tests without depending on a full GenApi implementation."""

    port: "gentl15.Port"
    root: ET.Element

    @classmethod
    def from_port(cls, port: "gentl15.Port") -> "RegisterMap":
        num_urls = port.get_num_urls()
        assert num_urls > 0, "Port exposes no GenApi description URLs"
        url = port.get_url_info(0, gentl15.URL_INFO_CMD.URL_INFO_URL)
        address, length = _parse_local_url(url)
        xml_bytes = port.read(address, length)
        root = _strip_namespaces(ET.fromstring(xml_bytes))
        return cls(port=port, root=root)

    def _reg_node(self, feature_name: str) -> ET.Element:
        feature = self.root.find(f".//*[@Name='{feature_name}']")
        assert feature is not None, f"unknown feature: {feature_name}"
        pvalue = feature.find("pValue")
        assert pvalue is not None, f"{feature_name} has no <pValue>"
        reg = self.root.find(f".//*[@Name='{pvalue.text}']")
        assert reg is not None, f"unknown register: {pvalue.text}"
        return reg

    def _addr_len_fmt(self, reg: ET.Element) -> tuple[int, int, str]:
        address = int(reg.find("Address").text, 16)
        length = int(reg.find("Length").text)
        endianess = reg.find("Endianess").text
        sign = reg.find("Sign").text if reg.find("Sign") is not None else "Unsigned"
        byte_order = "<" if endianess == "LittleEndian" else ">"
        size_code = {1: "B", 2: "H", 4: "I", 8: "Q"}[length]
        if sign == "Signed":
            size_code = size_code.lower()
        return address, length, byte_order + size_code

    def read_integer(self, feature_name: str) -> int:
        reg = self._reg_node(feature_name)
        address, length, fmt = self._addr_len_fmt(reg)
        return struct.unpack(fmt, self.port.read(address, length))[0]

    def write_integer(self, feature_name: str, value: int) -> None:
        reg = self._reg_node(feature_name)
        address, length, fmt = self._addr_len_fmt(reg)
        self.port.write(address, struct.pack(fmt, value))

    def execute_command(self, feature_name: str) -> None:
        feature = self.root.find(f".//*[@Name='{feature_name}']")
        assert feature is not None, f"unknown feature: {feature_name}"
        command_value = int(feature.find("CommandValue").text)
        self.write_integer(feature_name, command_value)


@pytest.fixture
def remote_registers(device) -> RegisterMap:
    return RegisterMap.from_port(device.get_port())


@pytest.fixture
def payload_size(remote_registers) -> int:
    return remote_registers.read_integer("PayloadSize")