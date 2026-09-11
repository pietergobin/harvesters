"""Pure unit tests for the GenTL 1.5 module: no GenTL Producer needed.

These validate the module's own surface (constants, enums, exception type)
independent of any `.cti`, so they run in the leanest possible environment
and act as a fast first line of defense. They also pin down the boundaries
of the 1.5 spec: version constants and the presence of multi-part buffer
support, while asserting that features introduced later (GenTL 1.6) are
correctly absent.
"""
from __future__ import annotations

import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "build"))
import gentl15


def test_version_constants():
    assert gentl15.GENTL_MAJOR_VERSION == 1
    assert gentl15.GENTL_MINOR_VERSION == 5
    assert gentl15.GENTL_SUBMINOR_VERSION == 0


def test_infinite_and_invalid_handle_constants():
    # GenTL spec, numeric constants (chapter 6.7).
    assert gentl15.GENTL_INFINITE == 0xFFFFFFFFFFFFFFFF
    assert gentl15.GENTL_INVALID_HANDLE == 0


def test_gc_error_enum_matches_spec_values():
    # A handful of load-bearing values from GenTL spec Table 6-4; if these
    # ever drift it means the header/enum got out of sync.
    E = gentl15.GC_ERROR
    assert E.GC_ERR_SUCCESS == 0
    assert E.GC_ERR_ERROR == -1001
    assert E.GC_ERR_NOT_INITIALIZED == -1002
    assert E.GC_ERR_INVALID_ID == -1007
    assert E.GC_ERR_TIMEOUT == -1011
    assert E.GC_ERR_BUSY == -1022


def test_gc_err_ambiguous_is_a_16_added_after_15():
    # GC_ERR_AMBIGUOUS (-1023) was introduced in GenTL 1.6 (GENICAM
    # < 5.2); a doc-conformant 1.5 surface must not carry it.
    assert not hasattr(gentl15.GC_ERROR, "GC_ERR_AMBIGUOUS")


def test_error_enum_members_are_reexported_at_module_scope():
    assert gentl15.GC_ERR_SUCCESS is gentl15.GC_ERROR.GC_ERR_SUCCESS
    assert gentl15.GC_ERR_INVALID_ID is gentl15.GC_ERROR.GC_ERR_INVALID_ID


def test_gc_error_is_an_intenum_like_type():
    # `code` comparisons in user code (e.g. `e.code == gentl15.GC_ERR_TIMEOUT`)
    # rely on GC_ERROR behaving like an int.
    assert int(gentl15.GC_ERROR.GC_ERR_TIMEOUT) == -1011
    assert gentl15.GC_ERROR.GC_ERR_TIMEOUT == -1011


def test_16_only_stream_info_commands_are_absent():
    # STREAM_INFO_CMD additions from GenTL 1.6 must not leak into 1.5.
    assert not hasattr(gentl15.STREAM_INFO_CMD, "STREAM_INFO_FLOW_TABLE")
    assert not hasattr(
        gentl15.STREAM_INFO_CMD, "STREAM_INFO_GENDC_PREFETCH_DESCRIPTOR"
    )


def test_16_only_buffer_info_commands_are_absent():
    assert not hasattr(gentl15.BUFFER_INFO_CMD, "BUFFER_INFO_IS_COMPOSITE")


def test_16_only_buffer_part_info_commands_are_absent():
    assert not hasattr(
        gentl15.BUFFER_PART_INFO_CMD, "BUFFER_PART_INFO_REGION_ID"
    )
    assert not hasattr(
        gentl15.BUFFER_PART_INFO_CMD, "BUFFER_PART_INFO_DATA_PURPOSE_ID"
    )


def test_16_only_payload_types_are_absent():
    assert not hasattr(gentl15.PAYLOADTYPE_INFO_ID, "GENDC")


def test_15_multipart_payload_types_are_present():
    # PAYLOAD_TYPE_MULTI_PART (the identifier for multi-part buffers, the
    # headline GenTL 1.5 addition) must be available -- GenTL 1.5 spec,
    # chapter 7.2.5.
    P = gentl15.PAYLOADTYPE_INFO_ID
    assert P.MULTI_PART in set(P)
    for name in (
        "UNKNOWN",
        "IMAGE",
        "RAW_DATA",
        "FILE",
        "CHUNK_DATA",
        "JPEG",
        "JPEG2000",
        "H264",
        "CHUNK_ONLY",
        "DEVICE_SPECIFIC",
    ):
        assert getattr(P, name) in set(P)


def test_15_part_datatype_enums_are_present():
    # PARTDATATYPE_ID / BUFFER_PART_INFO_CMD are the GenTL 1.5 additions for
    # addressing individual parts of a multi-part buffer. CONFIDENCE_MAP is a
    # part datatype (value 9), not a payload type.
    P = gentl15.PARTDATATYPE_ID
    assert P.CONFIDENCE_MAP in set(P)
    assert len(list(P)) > 0
    assert len(list(gentl15.BUFFER_PART_INFO_CMD)) > 0


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
        "BUFFER_PART_INFO_CMD",
        "PAYLOADTYPE_INFO_ID",
        "PARTDATATYPE_ID",
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
    enum_cls = getattr(gentl15, enum_name)
    members = list(enum_cls)
    assert members, f"{enum_name} has no members"


def test_genexception_is_a_real_exception_subclass():
    assert issubclass(gentl15.GenTLException, Exception)


def test_producer_rejects_nonexistent_path(tmp_path):
    missing = tmp_path / "does-not-exist.cti"
    with pytest.raises(RuntimeError, match="does-not-exist.cti"):
        gentl15.Producer(str(missing))


def test_producer_rejects_non_library_file(tmp_path):
    # A file that exists but definitely isn't a shared library.
    not_a_lib = tmp_path / "not-a-library.cti"
    not_a_lib.write_text("this is not ELF/PE data")
    with pytest.raises(RuntimeError):
        gentl15.Producer(str(not_a_lib))