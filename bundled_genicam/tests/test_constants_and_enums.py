"""Pure unit tests: no GenTL Producer needed at all.

These validate the module's own surface (constants, enums, exception type)
independent of any `.cti`, so they run in the leanest possible environment
and act as a fast first line of defense.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "build"))
import gentl


def test_version_constants():
    assert gentl.GENTL_MAJOR_VERSION == 1
    assert gentl.GENTL_MINOR_VERSION == 6
    assert isinstance(gentl.GENTL_SUBMINOR_VERSION, int)


def test_infinite_and_invalid_handle_constants():
    # GenTL spec, numeric constants (chapter 6.7).
    assert gentl.GENTL_INFINITE == 0xFFFFFFFFFFFFFFFF
    assert gentl.GENTL_INVALID_HANDLE == 0


def test_gc_error_enum_matches_spec_values():
    # A handful of load-bearing values from GenTL spec Table 6-4; if these
    # ever drift it means the header/enum got out of sync.
    E = gentl.GC_ERROR
    assert E.GC_ERR_SUCCESS == 0
    assert E.GC_ERR_ERROR == -1001
    assert E.GC_ERR_NOT_INITIALIZED == -1002
    assert E.GC_ERR_INVALID_ID == -1007
    assert E.GC_ERR_TIMEOUT == -1011
    assert E.GC_ERR_BUSY == -1022
    assert E.GC_ERR_AMBIGUOUS == -1023


def test_error_enum_members_are_reexported_at_module_scope():
    assert gentl.GC_ERR_SUCCESS is gentl.GC_ERROR.GC_ERR_SUCCESS
    assert gentl.GC_ERR_INVALID_ID is gentl.GC_ERROR.GC_ERR_INVALID_ID


def test_gc_error_is_an_intenum_like_type():
    # `code` comparisons in user code (e.g. `e.code == gentl.GC_ERR_TIMEOUT`)
    # rely on GC_ERROR behaving like an int.
    assert int(gentl.GC_ERROR.GC_ERR_TIMEOUT) == -1011
    assert gentl.GC_ERROR.GC_ERR_TIMEOUT == -1011


@pytest.mark.parametrize(
    "enum_name",
    [
        "INFO_DATATYPE",
        "TL_INFO_CMD",
        "INTERFACE_INFO_CMD",
        "DEVICE_ACCESS_FLAGS",
        "DEVICE_ACCESS_STATUS",
        "DEVICE_INFO_CMD",
        "ACQ_STOP_FLAGS",
        "ACQ_START_FLAGS",
        "ACQ_QUEUE_TYPE",
        "STREAM_INFO_CMD",
        "BUFFER_INFO_CMD",
        "PAYLOADTYPE_INFO_ID",
        "PIXELFORMAT_NAMESPACE_ID",
        "PORT_INFO_CMD",
        "URL_INFO_CMD",
        "URL_SCHEME_ID",
        "EVENT_TYPE",
        "EVENT_INFO_CMD",
        "EVENT_DATA_INFO_CMD",
    ],
)
def test_expected_enums_exist_and_are_nonempty(enum_name):
    enum_cls = getattr(gentl, enum_name)
    members = list(enum_cls)
    assert members, f"{enum_name} has no members"


def test_genexception_is_a_real_exception_subclass():
    assert issubclass(gentl.GenTLException, Exception)


def test_producer_rejects_nonexistent_path(tmp_path):
    missing = tmp_path / "does-not-exist.cti"
    with pytest.raises(RuntimeError, match="does-not-exist.cti"):
        gentl.Producer(str(missing))


def test_producer_rejects_non_library_file(tmp_path):
    # A file that exists but definitely isn't a shared library.
    not_a_lib = tmp_path / "not-a-library.cti"
    not_a_lib.write_text("this is not ELF/PE data")
    with pytest.raises(RuntimeError):
        gentl.Producer(str(not_a_lib))
