// gentl15.cpp -- nanobind bindings that dynamically load a GenTL Producer
// (".cti") library and expose its C interface (GenTL 1.5) as a fully typed
// Python module.
//
// A GenTL Producer is *not* linked against at build time. It is a plugin
// discovered/loaded at runtime (see chapter 6.1.1 of the GenTL spec), so
// this wrapper resolves every entry point via dlopen/dlsym (or
// LoadLibrary/GetProcAddress on Windows) and exposes thin, Pythonic,
// exception-raising wrappers around them.
//
// This module targets GenTL 1.5 (the version shipped in GenICam Package
// v3.0.2).  For the GenTL 1.6 variant see the companion `gentl` module.

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/shared_ptr.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

// The v1.5 header's C++ types (enum tags like GC_ERROR_LIST, structs, caller
// types, ...) share their names with the GenTL 1.6 header used by the
// companion `gentl` module. nanobind keeps a process-wide type registry keyed
// by C++ type identity (RTTI name), so if both modules were compiled against
// identically-named C++ types, loading one after the other would abort with
// "nanobind: type 'GC_ERROR' was already registered!". Wrapping the header in
// this dedicated namespace gives every GenTL 1.5 type a distinct mangled name
// (gentl15::GenTL::*) while keeping the Python-visible names exactly as the
// spec dictates.
namespace gentl15 {

// The header defines its own fixed width types unless GC_USER_DEFINED_TYPES
// is set, which is exactly what we want here.
#include "headers/GenTL_v1_5.h"

namespace nb = nanobind;
using namespace nb::literals;
using namespace GenTL;

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

namespace {

std::string error_name(GC_ERROR err) {
    switch (err) {
        case GC_ERR_SUCCESS: return "GC_ERR_SUCCESS";
        case GC_ERR_ERROR: return "GC_ERR_ERROR";
        case GC_ERR_NOT_INITIALIZED: return "GC_ERR_NOT_INITIALIZED";
        case GC_ERR_NOT_IMPLEMENTED: return "GC_ERR_NOT_IMPLEMENTED";
        case GC_ERR_RESOURCE_IN_USE: return "GC_ERR_RESOURCE_IN_USE";
        case GC_ERR_ACCESS_DENIED: return "GC_ERR_ACCESS_DENIED";
        case GC_ERR_INVALID_HANDLE: return "GC_ERR_INVALID_HANDLE";
        case GC_ERR_INVALID_ID: return "GC_ERR_INVALID_ID";
        case GC_ERR_NO_DATA: return "GC_ERR_NO_DATA";
        case GC_ERR_INVALID_PARAMETER: return "GC_ERR_INVALID_PARAMETER";
        case GC_ERR_IO: return "GC_ERR_IO";
        case GC_ERR_TIMEOUT: return "GC_ERR_TIMEOUT";
        case GC_ERR_ABORT: return "GC_ERR_ABORT";
        case GC_ERR_INVALID_BUFFER: return "GC_ERR_INVALID_BUFFER";
        case GC_ERR_NOT_AVAILABLE: return "GC_ERR_NOT_AVAILABLE";
        case GC_ERR_INVALID_ADDRESS: return "GC_ERR_INVALID_ADDRESS";
        case GC_ERR_BUFFER_TOO_SMALL: return "GC_ERR_BUFFER_TOO_SMALL";
        case GC_ERR_INVALID_INDEX: return "GC_ERR_INVALID_INDEX";
        case GC_ERR_PARSING_CHUNK_DATA: return "GC_ERR_PARSING_CHUNK_DATA";
        case GC_ERR_INVALID_VALUE: return "GC_ERR_INVALID_VALUE";
        case GC_ERR_RESOURCE_EXHAUSTED: return "GC_ERR_RESOURCE_EXHAUSTED";
        case GC_ERR_OUT_OF_MEMORY: return "GC_ERR_OUT_OF_MEMORY";
        case GC_ERR_BUSY: return "GC_ERR_BUSY";
        default:
            if (err <= GC_ERR_CUSTOM_ID) return "GC_ERR_CUSTOM_ID";
            return "GC_ERR_UNKNOWN";
    }
}

// Platform-neutral dynamic library handle.
class SharedLibrary {
public:
    explicit SharedLibrary(const std::string &path) {
#if defined(_WIN32)
        handle_ = static_cast<void *>(LoadLibraryA(path.c_str()));
        if (!handle_)
            throw std::runtime_error("Failed to load GenTL Producer: " + path);
#else
        handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!handle_) {
            const char *err = dlerror();
            throw std::runtime_error(
                std::string("Failed to load GenTL Producer: ") + path +
                " (" + (err ? err : "unknown error") + ")");
        }
#endif
    }

    ~SharedLibrary() {
        if (handle_) {
#if defined(_WIN32)
            FreeLibrary(static_cast<HMODULE>(handle_));
#else
            dlclose(handle_);
#endif
        }
    }

    SharedLibrary(const SharedLibrary &) = delete;
    SharedLibrary &operator=(const SharedLibrary &) = delete;

    void *symbol(const char *name) const {
#if defined(_WIN32)
        return reinterpret_cast<void *>(
            GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
        return dlsym(handle_, name);
#endif
    }

private:
    void *handle_ = nullptr;
};

} // namespace

// ---------------------------------------------------------------------------
// GenTLException -- raised for every non-success GC_ERROR
// ---------------------------------------------------------------------------

struct GenTLException : std::runtime_error {
    GC_ERROR code;
    GenTLException(GC_ERROR c, const std::string &msg)
        : std::runtime_error(msg), code(c) {}
};

// ---------------------------------------------------------------------------
// FunctionTable -- resolves every GenTL entry point from the loaded library
// ---------------------------------------------------------------------------

struct FunctionTable {
    std::unique_ptr<SharedLibrary> lib;

    PGCGetInfo GCGetInfo_ = nullptr;
    PGCGetLastError GCGetLastError_ = nullptr;
    PGCInitLib GCInitLib_ = nullptr;
    PGCCloseLib GCCloseLib_ = nullptr;
    PGCReadPort GCReadPort_ = nullptr;
    PGCWritePort GCWritePort_ = nullptr;
    PGCGetPortURL GCGetPortURL_ = nullptr;
    PGCGetPortInfo GCGetPortInfo_ = nullptr;
    PGCRegisterEvent GCRegisterEvent_ = nullptr;
    PGCUnregisterEvent GCUnregisterEvent_ = nullptr;
    PEventGetData EventGetData_ = nullptr;
    PEventGetDataInfo EventGetDataInfo_ = nullptr;
    PEventGetInfo EventGetInfo_ = nullptr;
    PEventFlush EventFlush_ = nullptr;
    PEventKill EventKill_ = nullptr;
    PTLOpen TLOpen_ = nullptr;
    PTLClose TLClose_ = nullptr;
    PTLGetInfo TLGetInfo_ = nullptr;
    PTLGetNumInterfaces TLGetNumInterfaces_ = nullptr;
    PTLGetInterfaceID TLGetInterfaceID_ = nullptr;
    PTLGetInterfaceInfo TLGetInterfaceInfo_ = nullptr;
    PTLOpenInterface TLOpenInterface_ = nullptr;
    PTLUpdateInterfaceList TLUpdateInterfaceList_ = nullptr;
    PIFClose IFClose_ = nullptr;
    PIFGetInfo IFGetInfo_ = nullptr;
    PIFGetNumDevices IFGetNumDevices_ = nullptr;
    PIFGetDeviceID IFGetDeviceID_ = nullptr;
    PIFUpdateDeviceList IFUpdateDeviceList_ = nullptr;
    PIFGetDeviceInfo IFGetDeviceInfo_ = nullptr;
    PIFOpenDevice IFOpenDevice_ = nullptr;
    PDevGetPort DevGetPort_ = nullptr;
    PDevGetNumDataStreams DevGetNumDataStreams_ = nullptr;
    PDevGetDataStreamID DevGetDataStreamID_ = nullptr;
    PDevOpenDataStream DevOpenDataStream_ = nullptr;
    PDevGetInfo DevGetInfo_ = nullptr;
    PDevClose DevClose_ = nullptr;
    PDSAnnounceBuffer DSAnnounceBuffer_ = nullptr;
    PDSAllocAndAnnounceBuffer DSAllocAndAnnounceBuffer_ = nullptr;
    PDSFlushQueue DSFlushQueue_ = nullptr;
    PDSStartAcquisition DSStartAcquisition_ = nullptr;
    PDSStopAcquisition DSStopAcquisition_ = nullptr;
    PDSGetInfo DSGetInfo_ = nullptr;
    PDSGetBufferID DSGetBufferID_ = nullptr;
    PDSClose DSClose_ = nullptr;
    PDSRevokeBuffer DSRevokeBuffer_ = nullptr;
    PDSQueueBuffer DSQueueBuffer_ = nullptr;
    PDSGetBufferInfo DSGetBufferInfo_ = nullptr;
    PGCGetNumPortURLs GCGetNumPortURLs_ = nullptr;
    PGCGetPortURLInfo GCGetPortURLInfo_ = nullptr;
    PDSGetBufferChunkData DSGetBufferChunkData_ = nullptr;
    PIFGetParentTL IFGetParentTL_ = nullptr;
    PDevGetParentIF DevGetParentIF_ = nullptr;
    PDSGetParentDev DSGetParentDev_ = nullptr;
    PDSGetNumBufferParts DSGetNumBufferParts_ = nullptr;
    PDSGetBufferPartInfo DSGetBufferPartInfo_ = nullptr;

    bool lib_initialized = false;

    explicit FunctionTable(const std::string &path) {
        lib = std::make_unique<SharedLibrary>(path);

#define LOAD(name) name##_ = reinterpret_cast<P##name>(lib->symbol(#name))
        LOAD(GCGetInfo);
        LOAD(GCGetLastError);
        LOAD(GCInitLib);
        LOAD(GCCloseLib);
        LOAD(GCReadPort);
        LOAD(GCWritePort);
        LOAD(GCGetPortURL);
        LOAD(GCGetPortInfo);
        LOAD(GCRegisterEvent);
        LOAD(GCUnregisterEvent);
        LOAD(EventGetData);
        LOAD(EventGetDataInfo);
        LOAD(EventGetInfo);
        LOAD(EventFlush);
        LOAD(EventKill);
        LOAD(TLOpen);
        LOAD(TLClose);
        LOAD(TLGetInfo);
        LOAD(TLGetNumInterfaces);
        LOAD(TLGetInterfaceID);
        LOAD(TLGetInterfaceInfo);
        LOAD(TLOpenInterface);
        LOAD(TLUpdateInterfaceList);
        LOAD(IFClose);
        LOAD(IFGetInfo);
        LOAD(IFGetNumDevices);
        LOAD(IFGetDeviceID);
        LOAD(IFUpdateDeviceList);
        LOAD(IFGetDeviceInfo);
        LOAD(IFOpenDevice);
        LOAD(DevGetPort);
        LOAD(DevGetNumDataStreams);
        LOAD(DevGetDataStreamID);
        LOAD(DevOpenDataStream);
        LOAD(DevGetInfo);
        LOAD(DevClose);
        LOAD(DSAnnounceBuffer);
        LOAD(DSAllocAndAnnounceBuffer);
        LOAD(DSFlushQueue);
        LOAD(DSStartAcquisition);
        LOAD(DSStopAcquisition);
        LOAD(DSGetInfo);
        LOAD(DSGetBufferID);
        LOAD(DSClose);
        LOAD(DSRevokeBuffer);
        LOAD(DSQueueBuffer);
        LOAD(DSGetBufferInfo);
        LOAD(GCGetNumPortURLs);
        LOAD(GCGetPortURLInfo);
        LOAD(DSGetBufferChunkData);
        LOAD(IFGetParentTL);
        LOAD(DevGetParentIF);
        LOAD(DSGetParentDev);
        LOAD(DSGetNumBufferParts);
        LOAD(DSGetBufferPartInfo);
#undef LOAD

        if (!GCInitLib_ || !GCCloseLib_ || !TLOpen_ || !TLClose_)
            throw std::runtime_error(
                "The provided library does not look like a valid GenTL "
                "Producer (mandatory entry points are missing)");
    }

    ~FunctionTable() {
        if (lib_initialized && GCCloseLib_)
            GCCloseLib_();
    }
};

using FunctionTablePtr = std::shared_ptr<FunctionTable>;

namespace {

[[noreturn]] void raise(const FunctionTablePtr &ft, GC_ERROR err,
                         const char *what) {
    std::string message = std::string(what) + " failed with " +
                           error_name(err) + " (" + std::to_string(err) + ")";
    if (ft && ft->GCGetLastError_) {
        GC_ERROR last_code = GC_ERR_SUCCESS;
        size_t size = 0;
        if (ft->GCGetLastError_(&last_code, nullptr, &size) == GC_ERR_SUCCESS &&
            size > 0) {
            std::vector<char> text(size, 0);
            size_t sz2 = size;
            if (ft->GCGetLastError_(&last_code, text.data(), &sz2) ==
                GC_ERR_SUCCESS) {
                message += ": ";
                message += text.data();
            }
        }
    }
    throw GenTLException(err, message);
}

inline void check(const FunctionTablePtr &ft, GC_ERROR err, const char *what) {
    if (err != GC_ERR_SUCCESS)
        raise(ft, err, what);
}

// Converts a raw GenTL info buffer to the most natural Python object given
// its reported INFO_DATATYPE.
nb::object decode_info(INFO_DATATYPE type, const std::vector<uint8_t> &buf) {
    switch (type) {
        case INFO_DATATYPE_STRING: {
            const char *s = reinterpret_cast<const char *>(buf.data());
            size_t len = strnlen(s, buf.size());
            return nb::str(s, len);
        }
        case INFO_DATATYPE_STRINGLIST: {
            nb::list result;
            const char *p = reinterpret_cast<const char *>(buf.data());
            const char *end = p + buf.size();
            while (p < end && *p) {
                size_t len = strnlen(p, static_cast<size_t>(end - p));
                result.append(nb::str(p, len));
                p += len + 1;
            }
            return result;
        }
        case INFO_DATATYPE_INT16:
            return nb::int_(*reinterpret_cast<const int16_t *>(buf.data()));
        case INFO_DATATYPE_UINT16:
            return nb::int_(*reinterpret_cast<const uint16_t *>(buf.data()));
        case INFO_DATATYPE_INT32:
            return nb::int_(*reinterpret_cast<const int32_t *>(buf.data()));
        case INFO_DATATYPE_UINT32:
            return nb::int_(*reinterpret_cast<const uint32_t *>(buf.data()));
        case INFO_DATATYPE_INT64:
            return nb::int_(*reinterpret_cast<const int64_t *>(buf.data()));
        case INFO_DATATYPE_UINT64:
            return nb::int_(*reinterpret_cast<const uint64_t *>(buf.data()));
        case INFO_DATATYPE_FLOAT64:
            return nb::float_(*reinterpret_cast<const double *>(buf.data()));
        case INFO_DATATYPE_PTR:
            return nb::int_(reinterpret_cast<uintptr_t>(
                *reinterpret_cast<void *const *>(buf.data())));
        case INFO_DATATYPE_BOOL8:
            return nb::bool_(*reinterpret_cast<const bool8_t *>(buf.data()) !=
                              0);
        case INFO_DATATYPE_SIZET:
            return nb::int_(*reinterpret_cast<const size_t *>(buf.data()));
        case INFO_DATATYPE_PTRDIFF:
            return nb::int_(*reinterpret_cast<const ptrdiff_t *>(buf.data()));
        case INFO_DATATYPE_BUFFER:
        default:
            return nb::bytes(reinterpret_cast<const char *>(buf.data()),
                              buf.size());
    }
}

// Generic "query-with-growing-buffer" helper matching the ubiquitous GenTL
// pattern:
//   fn(..., INFO_DATATYPE *piType, void *pBuffer, size_t *piSize)
// First call negotiates the size (pBuffer == nullptr), second call fills it.
template <typename Fn>
nb::object query_info(const FunctionTablePtr &ft, Fn &&fn, const char *what) {
    INFO_DATATYPE type = INFO_DATATYPE_UNKNOWN;
    size_t size = 0;
    GC_ERROR err = fn(&type, nullptr, &size);
    check(ft, err, what);
    std::vector<uint8_t> buf(size, 0);
    if (size > 0) {
        size_t sz2 = size;
        err = fn(&type, buf.data(), &sz2);
        check(ft, err, what);
        buf.resize(sz2);
    }
    return decode_info(type, buf);
}

// Same idea but for plain string-only queries (no INFO_DATATYPE), e.g.
// TLGetInterfaceID / IFGetDeviceID / DevGetDataStreamID.
template <typename Fn>
std::string query_string(const FunctionTablePtr &ft, Fn &&fn,
                          const char *what) {
    size_t size = 0;
    GC_ERROR err = fn(nullptr, &size);
    check(ft, err, what);
    std::string buf(size, '\0');
    if (size > 0) {
        size_t sz2 = size;
        err = fn(buf.data(), &sz2);
        check(ft, err, what);
        if (sz2 > 0 && buf[sz2 - 1] == '\0')
            buf.resize(sz2 - 1);
        else
            buf.resize(sz2);
    }
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// Port -- wraps a PORT_HANDLE (module or remote device register interface)
// ---------------------------------------------------------------------------

class PyPort {
public:
    PyPort(FunctionTablePtr ft, PORT_HANDLE handle)
        : ft_(std::move(ft)), handle_(handle) {}

    nb::bytes read(uint64_t address, size_t size) const {
        std::vector<uint8_t> buf(size, 0);
        size_t sz = size;
        GC_ERROR err = ft_->GCReadPort_(handle_, address, buf.data(), &sz);
        check(ft_, err, "GCReadPort");
        return nb::bytes(reinterpret_cast<const char *>(buf.data()), sz);
    }

    size_t write(uint64_t address, nb::bytes data) const {
        size_t sz = data.size();
        GC_ERROR err =
            ft_->GCWritePort_(handle_, address, data.c_str(), &sz);
        check(ft_, err, "GCWritePort");
        return sz;
    }

    nb::object get_info(PORT_INFO_CMD_LIST cmd) const {
        return query_info(
            ft_,
            [&](INFO_DATATYPE *t, void *b, size_t *s) {
                return ft_->GCGetPortInfo_(handle_, cmd, t, b, s);
            },
            "GCGetPortInfo");
    }

    uint32_t get_num_urls() const {
        uint32_t n = 0;
        if (!ft_->GCGetNumPortURLs_)
            throw GenTLException(GC_ERR_NOT_IMPLEMENTED,
                                  "GCGetNumPortURLs is not implemented by "
                                  "this Producer");
        GC_ERROR err = ft_->GCGetNumPortURLs_(handle_, &n);
        check(ft_, err, "GCGetNumPortURLs");
        return n;
    }

    nb::object get_url_info(uint32_t index, URL_INFO_CMD_LIST cmd) const {
        if (!ft_->GCGetPortURLInfo_)
            throw GenTLException(GC_ERR_NOT_IMPLEMENTED,
                                  "GCGetPortURLInfo is not implemented by "
                                  "this Producer");
        return query_info(
            ft_,
            [&](INFO_DATATYPE *t, void *b, size_t *s) {
                return ft_->GCGetPortURLInfo_(handle_, index, cmd, t, b, s);
            },
            "GCGetPortURLInfo");
    }

    PORT_HANDLE handle() const { return handle_; }

private:
    FunctionTablePtr ft_;
    PORT_HANDLE handle_;
};

// ---------------------------------------------------------------------------
// Buffer -- wraps a BUFFER_HANDLE
// ---------------------------------------------------------------------------

class PyDataStream; // fwd

class PyBuffer {
public:
    // Plain constructor used for Producer-allocated buffers
    // (alloc_and_announce_buffer()): the Producer owns and releases the
    // memory itself (GenTL spec, 5.2.10), so there is nothing for us to
    // keep alive.
    PyBuffer(FunctionTablePtr ft, DS_HANDLE ds, BUFFER_HANDLE handle)
        : ft_(std::move(ft)), ds_(ds), handle_(handle) {}

    // Constructor used for Consumer-allocated buffers (announce_buffer()):
    // `keepalive` is the Python object backing the memory, and `view` is
    // the *locked, writable* buffer-protocol view into it (obtained via
    // PyObject_GetBuffer(..., PyBUF_WRITABLE)). Both must stay alive for as
    // long as the Producer might still write into that memory, i.e. until
    // this Buffer is revoked or destroyed -- otherwise Python could
    // mutate/relocate/garbage-collect memory the acquisition engine is
    // actively writing into.
    PyBuffer(FunctionTablePtr ft, DS_HANDLE ds, BUFFER_HANDLE handle,
              nb::object keepalive, Py_buffer view)
        : ft_(std::move(ft)), ds_(ds), handle_(handle),
          keepalive_(std::move(keepalive)), view_(view), has_view_(true) {}

    ~PyBuffer() { release_view(); }

    PyBuffer(const PyBuffer &) = delete;
    PyBuffer &operator=(const PyBuffer &) = delete;

    void queue() const {
        GC_ERROR err = ft_->DSQueueBuffer_(ds_, handle_);
        check(ft_, err, "DSQueueBuffer");
    }

    std::pair<uintptr_t, uintptr_t> revoke() {
        void *buf = nullptr;
        void *priv = nullptr;
        GC_ERROR err = ft_->DSRevokeBuffer_(ds_, handle_, &buf, &priv);
        check(ft_, err, "DSRevokeBuffer");
        // Safe to release our reference on the backing object now that the
        // Producer has forgotten about this memory.
        release_view();
        keepalive_ = nb::object();
        return {reinterpret_cast<uintptr_t>(buf),
                reinterpret_cast<uintptr_t>(priv)};
    }


    nb::object get_info(BUFFER_INFO_CMD_LIST cmd) const {
        return query_info(
            ft_,
            [&](INFO_DATATYPE *t, void *b, size_t *s) {
                return ft_->DSGetBufferInfo_(ds_, handle_, cmd, t, b, s);
            },
            "DSGetBufferInfo");
    }

    nb::bytes get_data() const {
        auto base = nb::cast<uint64_t>(
            get_info(BUFFER_INFO_BASE));
        auto size = nb::cast<uint64_t>(get_info(BUFFER_INFO_SIZE));
        const char *ptr = reinterpret_cast<const char *>(
            static_cast<uintptr_t>(base));
        return nb::bytes(ptr, static_cast<size_t>(size));
    }

    uintptr_t handle_value() const {
        return reinterpret_cast<uintptr_t>(handle_);
    }

    BUFFER_HANDLE handle() const { return handle_; }

private:
    void release_view() {
        if (has_view_) {
            PyBuffer_Release(&view_);
            has_view_ = false;
        }
    }

    FunctionTablePtr ft_;
    DS_HANDLE ds_;
    BUFFER_HANDLE handle_;
    nb::object keepalive_;
    Py_buffer view_{};
    bool has_view_ = false;
};

// ---------------------------------------------------------------------------
// Event -- wraps an EVENT_HANDLE
// ---------------------------------------------------------------------------

class PyEvent {
public:
    PyEvent(FunctionTablePtr ft, EVENTSRC_HANDLE src, EVENT_TYPE type,
            EVENT_HANDLE handle)
        : ft_(std::move(ft)), src_(src), type_(type), handle_(handle) {}

    ~PyEvent() {
        if (handle_ && ft_->GCUnregisterEvent_)
            ft_->GCUnregisterEvent_(src_, type_);
    }

    PyEvent(const PyEvent &) = delete;
    PyEvent &operator=(const PyEvent &) = delete;

    nb::bytes get_data(uint64_t timeout_ms) const {
        size_t size = 0;
        GC_ERROR err = ft_->EventGetData_(handle_, nullptr, &size, 0);
        if (err != GC_ERR_SUCCESS && err != GC_ERR_BUFFER_TOO_SMALL)
            check(ft_, err, "EventGetData");
        std::vector<uint8_t> buf(size, 0);
        size_t sz2 = size;
        err = ft_->EventGetData_(handle_, buf.data(), &sz2, timeout_ms);
        check(ft_, err, "EventGetData");
        return nb::bytes(reinterpret_cast<const char *>(buf.data()), sz2);
    }

    // Convenience decoder for events registered with EVENT_NEW_BUFFER: reads
    // and unpacks the EVENT_NEW_BUFFER_DATA payload (GenTL spec, 6.5.2.1) so
    // callers don't have to hand-decode raw bytes. Returns
    // (buffer_handle, user_pointer), both matching Buffer.handle /
    // the private_ptr passed to (alloc_and_)announce_buffer.
    std::pair<uintptr_t, uintptr_t> get_new_buffer_data(
        uint64_t timeout_ms) const {
        EVENT_NEW_BUFFER_DATA data{};
        size_t size = sizeof(data);
        GC_ERROR err =
            ft_->EventGetData_(handle_, &data, &size, timeout_ms);
        check(ft_, err, "EventGetData");
        return {reinterpret_cast<uintptr_t>(data.BufferHandle),
                reinterpret_cast<uintptr_t>(data.pUserPointer)};
    }

    void flush() const {
        GC_ERROR err = ft_->EventFlush_(handle_);
        check(ft_, err, "EventFlush");
    }

    void kill() const {
        GC_ERROR err = ft_->EventKill_(handle_);
        check(ft_, err, "EventKill");
    }

    nb::object get_info(EVENT_INFO_CMD_LIST cmd) const {
        return query_info(
            ft_,
            [&](INFO_DATATYPE *t, void *b, size_t *s) {
                return ft_->EventGetInfo_(handle_, cmd, t, b, s);
            },
            "EventGetInfo");
    }

private:
    FunctionTablePtr ft_;
    EVENTSRC_HANDLE src_;
    EVENT_TYPE type_;
    EVENT_HANDLE handle_;
};

// ---------------------------------------------------------------------------
// DataStream -- wraps a DS_HANDLE
// ---------------------------------------------------------------------------

class PyDataStream {
public:
    // Normal constructor: this object *owns* the DS_HANDLE (obtained via
    // DevOpenDataStream()) and will DSClose() it on close()/destruction.
    PyDataStream(FunctionTablePtr ft, DEV_HANDLE dev, DS_HANDLE handle)
        : ft_(std::move(ft)), dev_(dev), handle_(handle) {}

    // "Borrowed handle" constructor: used to attach to a DS_HANDLE that was
    // opened -- and is owned -- by a *different* GenTL binding already
    // running in this process (see module-level attach_data_stream()).
    // close()/destruction here only forgets the handle, it never calls
    // DSClose() on it -- that remains the owning binding's responsibility.
    PyDataStream(FunctionTablePtr ft, DS_HANDLE handle, bool /*borrowed_tag*/)
        : ft_(std::move(ft)), dev_(GENTL_INVALID_HANDLE), handle_(handle),
          owns_handle_(false) {}

    ~PyDataStream() { close_impl(); }

    PyDataStream(const PyDataStream &) = delete;
    PyDataStream &operator=(const PyDataStream &) = delete;


    void close() { close_impl(); }

    std::shared_ptr<PyBuffer> announce_buffer(nb::object data,
                                                uintptr_t private_ptr) {
        // `data` must support the (writable) Python buffer protocol, e.g.
        // `bytearray`, `numpy.ndarray`, or `array.array` -- plain
        // immutable `bytes` are rejected since the acquisition engine
        // writes into this memory and CPython `bytes` objects must never
        // be mutated after creation.
        Py_buffer view{};
        if (PyObject_GetBuffer(data.ptr(), &view, PyBUF_WRITABLE | PyBUF_SIMPLE) != 0)
            throw nb::python_error();

        BUFFER_HANDLE h = GENTL_INVALID_HANDLE;
        GC_ERROR err = ft_->DSAnnounceBuffer_(
            handle_, view.buf, static_cast<size_t>(view.len),
            reinterpret_cast<void *>(private_ptr), &h);
        if (err != GC_ERR_SUCCESS) {
            PyBuffer_Release(&view);
            check(ft_, err, "DSAnnounceBuffer");
        }
        // Keep `data` (and its locked buffer view) alive on the returned
        // Buffer for as long as the Producer may write into it -- see
        // PyBuffer's constructor doc comment.
        return std::make_shared<PyBuffer>(ft_, handle_, h, std::move(data),
                                           view);
    }

    std::shared_ptr<PyBuffer> alloc_and_announce_buffer(
        size_t size, uintptr_t private_ptr) {
        BUFFER_HANDLE h = GENTL_INVALID_HANDLE;
        GC_ERROR err = ft_->DSAllocAndAnnounceBuffer_(
            handle_, size, reinterpret_cast<void *>(private_ptr), &h);
        check(ft_, err, "DSAllocAndAnnounceBuffer");
        return std::make_shared<PyBuffer>(ft_, handle_, h);
    }

    void flush_queue(ACQ_QUEUE_TYPE_LIST op) const {
        GC_ERROR err = ft_->DSFlushQueue_(handle_, op);
        check(ft_, err, "DSFlushQueue");
    }

    void start_acquisition(ACQ_START_FLAGS_LIST flags, uint64_t num_to_acquire) {
        GC_ERROR err =
            ft_->DSStartAcquisition_(handle_, flags, num_to_acquire);
        check(ft_, err, "DSStartAcquisition");
    }

    void stop_acquisition(ACQ_STOP_FLAGS_LIST flags) {
        GC_ERROR err = ft_->DSStopAcquisition_(handle_, flags);
        check(ft_, err, "DSStopAcquisition");
    }

    nb::object get_info(STREAM_INFO_CMD_LIST cmd) const {
        return query_info(
            ft_,
            [&](INFO_DATATYPE *t, void *b, size_t *s) {
                return ft_->DSGetInfo_(handle_, cmd, t, b, s);
            },
            "DSGetInfo");
    }

    std::shared_ptr<PyBuffer> get_buffer_by_index(uint32_t index) {
        BUFFER_HANDLE h = GENTL_INVALID_HANDLE;
        GC_ERROR err = ft_->DSGetBufferID_(handle_, index, &h);
        check(ft_, err, "DSGetBufferID");
        return std::make_shared<PyBuffer>(ft_, handle_, h);
    }

    std::shared_ptr<PyEvent> register_new_buffer_event() {
        EVENT_HANDLE h = GENTL_INVALID_HANDLE;
        GC_ERROR err =
            ft_->GCRegisterEvent_(handle_, EVENT_NEW_BUFFER, &h);
        check(ft_, err, "GCRegisterEvent(EVENT_NEW_BUFFER)");
        return std::make_shared<PyEvent>(ft_, handle_, EVENT_NEW_BUFFER, h);
    }

    DS_HANDLE handle() const { return handle_; }

private:
    void close_impl() {
        if (owns_handle_ && handle_ && ft_ && ft_->DSClose_) {
            ft_->DSClose_(handle_);
        }
        handle_ = GENTL_INVALID_HANDLE;
    }

    FunctionTablePtr ft_;
    DEV_HANDLE dev_;
    DS_HANDLE handle_;
    bool owns_handle_ = true;
};

// Attaches to a DS_HANDLE that a *different* GenTL binding in this same
// process already opened (e.g. genicam.gentl's official SWIG binding),
// purely so its buffers can be managed with Producer-controlled allocation
// (DSAllocAndAnnounceBuffer), which that other binding may not expose.
//
// This does not call DevOpenDataStream()/TLOpen()/GCInitLib() at all -- it
// only dlopen()'s `cti_path` (harmlessly bumping the refcount of the
// already-loaded shared library and resolving the handful of DS*/GC*
// function pointers this module needs) and wraps the caller-supplied raw
// handle. The caller remains responsible for the DataStream's lifetime
// (opening/closing it, starting/stopping acquisition, registering for
// events, etc.) through whichever binding actually owns it; this object
// must not outlive that ownership.
//
// `handle` must be the exact numeric DS_HANDLE (pointer value) of an
// already-open GenTL Data Stream module in this process, e.g. obtained from
// genicam.gentl via `int(data_stream.module._handle)`.
std::shared_ptr<PyDataStream> attach_data_stream(const std::string &cti_path,
                                                  uintptr_t handle) {
    auto ft = std::make_shared<FunctionTable>(cti_path);
    return std::make_shared<PyDataStream>(
        ft, reinterpret_cast<DS_HANDLE>(handle), true);
}


// ---------------------------------------------------------------------------
// Device -- wraps a DEV_HANDLE
// ---------------------------------------------------------------------------

class PyDevice {
public:
    PyDevice(FunctionTablePtr ft, IF_HANDLE iface, DEV_HANDLE handle)
        : ft_(std::move(ft)), iface_(iface), handle_(handle) {}

    ~PyDevice() { close_impl(); }

    PyDevice(const PyDevice &) = delete;
    PyDevice &operator=(const PyDevice &) = delete;

    void close() { close_impl(); }

    std::shared_ptr<PyPort> get_port() {
        PORT_HANDLE h = GENTL_INVALID_HANDLE;
        GC_ERROR err = ft_->DevGetPort_(handle_, &h);
        check(ft_, err, "DevGetPort");
        return std::make_shared<PyPort>(ft_, h);
    }

    std::shared_ptr<PyPort> control_port() {
        // The device handle itself is a valid PORT_HANDLE for local-module
        // (control) register access.
        return std::make_shared<PyPort>(ft_, handle_);
    }

    uint32_t get_num_data_streams() const {
        uint32_t n = 0;
        GC_ERROR err = ft_->DevGetNumDataStreams_(handle_, &n);
        check(ft_, err, "DevGetNumDataStreams");
        return n;
    }

    std::string get_data_stream_id(uint32_t index) const {
        return query_string(
            ft_,
            [&](char *b, size_t *s) {
                return ft_->DevGetDataStreamID_(handle_, index, b, s);
            },
            "DevGetDataStreamID");
    }

    std::shared_ptr<PyDataStream> open_data_stream(const std::string &id) {
        DS_HANDLE h = GENTL_INVALID_HANDLE;
        GC_ERROR err =
            ft_->DevOpenDataStream_(handle_, id.c_str(), &h);
        check(ft_, err, "DevOpenDataStream");
        return std::make_shared<PyDataStream>(ft_, handle_, h);
    }

    nb::object get_info(DEVICE_INFO_CMD_LIST cmd) const {
        return query_info(
            ft_,
            [&](INFO_DATATYPE *t, void *b, size_t *s) {
                return ft_->DevGetInfo_(handle_, cmd, t, b, s);
            },
            "DevGetInfo");
    }

    DEV_HANDLE handle() const { return handle_; }

private:
    void close_impl() {
        if (handle_ && ft_ && ft_->DevClose_) {
            ft_->DevClose_(handle_);
            handle_ = GENTL_INVALID_HANDLE;
        }
    }

    FunctionTablePtr ft_;
    IF_HANDLE iface_;
    DEV_HANDLE handle_;
};

// ---------------------------------------------------------------------------
// Interface -- wraps an IF_HANDLE
// ---------------------------------------------------------------------------

class PyInterface {
public:
    PyInterface(FunctionTablePtr ft, TL_HANDLE tl, IF_HANDLE handle)
        : ft_(std::move(ft)), tl_(tl), handle_(handle) {}

    ~PyInterface() { close_impl(); }

    PyInterface(const PyInterface &) = delete;
    PyInterface &operator=(const PyInterface &) = delete;

    void close() { close_impl(); }

    bool update_device_list(uint64_t timeout_ms) const {
        bool8_t changed = 0;
        GC_ERROR err =
            ft_->IFUpdateDeviceList_(handle_, &changed, timeout_ms);
        check(ft_, err, "IFUpdateDeviceList");
        return changed != 0;
    }

    uint32_t get_num_devices() const {
        uint32_t n = 0;
        GC_ERROR err = ft_->IFGetNumDevices_(handle_, &n);
        check(ft_, err, "IFGetNumDevices");
        return n;
    }

    std::string get_device_id(uint32_t index) const {
        return query_string(
            ft_,
            [&](char *b, size_t *s) {
                return ft_->IFGetDeviceID_(handle_, index, b, s);
            },
            "IFGetDeviceID");
    }

    nb::object get_device_info(const std::string &device_id,
                                DEVICE_INFO_CMD_LIST cmd) const {
        return query_info(
            ft_,
            [&](INFO_DATATYPE *t, void *b, size_t *s) {
                return ft_->IFGetDeviceInfo_(handle_, device_id.c_str(), cmd,
                                              t, b, s);
            },
            "IFGetDeviceInfo");
    }

    std::shared_ptr<PyDevice> open_device(
        const std::string &device_id,
        DEVICE_ACCESS_FLAGS_LIST flags = DEVICE_ACCESS_EXCLUSIVE) {
        DEV_HANDLE h = GENTL_INVALID_HANDLE;
        GC_ERROR err =
            ft_->IFOpenDevice_(handle_, device_id.c_str(), flags, &h);
        check(ft_, err, "IFOpenDevice");
        return std::make_shared<PyDevice>(ft_, handle_, h);
    }

    nb::object get_info(INTERFACE_INFO_CMD_LIST cmd) const {
        return query_info(
            ft_,
            [&](INFO_DATATYPE *t, void *b, size_t *s) {
                return ft_->IFGetInfo_(handle_, cmd, t, b, s);
            },
            "IFGetInfo");
    }

    IF_HANDLE handle() const { return handle_; }

private:
    void close_impl() {
        if (handle_ && ft_ && ft_->IFClose_) {
            ft_->IFClose_(handle_);
            handle_ = GENTL_INVALID_HANDLE;
        }
    }

    FunctionTablePtr ft_;
    TL_HANDLE tl_;
    IF_HANDLE handle_;
};

// ---------------------------------------------------------------------------
// System -- wraps a TL_HANDLE
// ---------------------------------------------------------------------------

class PySystem {
public:
    PySystem(FunctionTablePtr ft, TL_HANDLE handle)
        : ft_(std::move(ft)), handle_(handle) {}

    ~PySystem() { close_impl(); }

    PySystem(const PySystem &) = delete;
    PySystem &operator=(const PySystem &) = delete;

    void close() { close_impl(); }

    bool update_interface_list(uint64_t timeout_ms) const {
        bool8_t changed = 0;
        GC_ERROR err =
            ft_->TLUpdateInterfaceList_(handle_, &changed, timeout_ms);
        check(ft_, err, "TLUpdateInterfaceList");
        return changed != 0;
    }

    uint32_t get_num_interfaces() const {
        uint32_t n = 0;
        GC_ERROR err = ft_->TLGetNumInterfaces_(handle_, &n);
        check(ft_, err, "TLGetNumInterfaces");
        return n;
    }

    std::string get_interface_id(uint32_t index) const {
        return query_string(
            ft_,
            [&](char *b, size_t *s) {
                return ft_->TLGetInterfaceID_(handle_, index, b, s);
            },
            "TLGetInterfaceID");
    }

    nb::object get_interface_info(const std::string &iface_id,
                                    INTERFACE_INFO_CMD_LIST cmd) const {
        return query_info(
            ft_,
            [&](INFO_DATATYPE *t, void *b, size_t *s) {
                return ft_->TLGetInterfaceInfo_(handle_, iface_id.c_str(),
                                                 cmd, t, b, s);
            },
            "TLGetInterfaceInfo");
    }

    std::shared_ptr<PyInterface> open_interface(const std::string &iface_id) {
        IF_HANDLE h = GENTL_INVALID_HANDLE;
        GC_ERROR err =
            ft_->TLOpenInterface_(handle_, iface_id.c_str(), &h);
        check(ft_, err, "TLOpenInterface");
        return std::make_shared<PyInterface>(ft_, handle_, h);
    }

    nb::object get_info(TL_INFO_CMD_LIST cmd) const {
        return query_info(
            ft_,
            [&](INFO_DATATYPE *t, void *b, size_t *s) {
                return ft_->TLGetInfo_(handle_, cmd, t, b, s);
            },
            "TLGetInfo");
    }

    TL_HANDLE handle() const { return handle_; }

private:
    void close_impl() {
        if (handle_ && ft_ && ft_->TLClose_) {
            ft_->TLClose_(handle_);
            handle_ = GENTL_INVALID_HANDLE;
        }
    }

    FunctionTablePtr ft_;
    TL_HANDLE handle_;
};

// ---------------------------------------------------------------------------
// Producer -- the module's main entry point. Loads a .cti library, manages
// GCInitLib/GCCloseLib bracketing and offers TLOpen().
// ---------------------------------------------------------------------------

class PyProducer {
public:
    explicit PyProducer(const std::string &path) {
        ft_ = std::make_shared<FunctionTable>(path);
    }

    ~PyProducer() {
        if (ft_ && ft_->lib_initialized && ft_->GCCloseLib_) {
            ft_->GCCloseLib_();
            ft_->lib_initialized = false;
        }
    }

    PyProducer(const PyProducer &) = delete;
    PyProducer &operator=(const PyProducer &) = delete;

    void init_lib() {
        GC_ERROR err = ft_->GCInitLib_();
        check(ft_, err, "GCInitLib");
        ft_->lib_initialized = true;
    }

    void close_lib() {
        GC_ERROR err = ft_->GCCloseLib_();
        check(ft_, err, "GCCloseLib");
        ft_->lib_initialized = false;
    }

    nb::object get_info(TL_INFO_CMD_LIST cmd) const {
        if (!ft_->GCGetInfo_)
            throw GenTLException(GC_ERR_NOT_IMPLEMENTED,
                                  "GCGetInfo is not implemented by this "
                                  "Producer");
        return query_info(
            ft_,
            [&](INFO_DATATYPE *t, void *b, size_t *s) {
                return ft_->GCGetInfo_(cmd, t, b, s);
            },
            "GCGetInfo");
    }

    std::shared_ptr<PySystem> open_system() {
        TL_HANDLE h = GENTL_INVALID_HANDLE;
        GC_ERROR err = ft_->TLOpen_(&h);
        check(ft_, err, "TLOpen");
        return std::make_shared<PySystem>(ft_, h);
    }

    // Enter/exit so `with Producer(path) as p:` works and brackets
    // GCInitLib/GCCloseLib automatically (see binding below for the
    // reference-policy rationale).
    void exit(nb::object, nb::object, nb::object) { close_lib(); }

private:
    FunctionTablePtr ft_;
};

// ---------------------------------------------------------------------------
// nanobind module definition
// ---------------------------------------------------------------------------

void register_module(nb::module_ &m) {
    m.doc() = "Typed Python bindings for the GenICam GenTL 1.5 Producer C "
               "interface (GenICam Package v3.0.2). A GenTL Producer (.cti) "
               "is loaded dynamically at runtime via "
               "gentl15.Producer(path).";

    m.attr("GENTL_MAJOR_VERSION") = GenTLMajorVersion;
    m.attr("GENTL_MINOR_VERSION") = GenTLMinorVersion;
    m.attr("GENTL_SUBMINOR_VERSION") = GenTLSubMinorVersion;
    // "Infinite" timeout / unbounded acquisition count, and the sentinel
    // value used throughout the API for "no handle" -- exposed so callers
    // don't have to hardcode the GenTL spec's magic numbers themselves.
    m.attr("GENTL_INFINITE") = static_cast<uint64_t>(GENTL_INFINITE);
    m.attr("GENTL_INVALID_HANDLE") = static_cast<uintptr_t>(0);

    static nb::exception<GenTLException> exc(m, "GenTLException");
    // The default translator installed by nb::exception<> above only sets a
    // plain string message. Register a second (higher priority) translator
    // that additionally exposes the numeric GC_ERROR as `.code` on the
    // raised Python exception instance.
    nb::register_exception_translator(
        [](const std::exception_ptr &p, void *payload) {
            auto *exc_type = static_cast<PyObject *>(payload);
            try {
                std::rethrow_exception(p);
            } catch (const GenTLException &e) {
                PyObject *args = Py_BuildValue("(s)", e.what());
                if (!args)
                    return;
                PyObject *inst = PyObject_CallObject(exc_type, args);
                Py_DECREF(args);
                if (!inst)
                    return;
                PyObject *code = PyLong_FromLong(static_cast<long>(e.code));
                if (code) {
                    PyObject_SetAttrString(inst, "code", code);
                    Py_DECREF(code);
                }
                PyErr_SetObject(exc_type, inst);
                Py_DECREF(inst);
            }
        },
        exc.ptr());


    // ---- Error codes -----------------------------------------------------
    nb::enum_<GC_ERROR_LIST>(m, "GC_ERROR", nb::is_arithmetic())
        .value("GC_ERR_SUCCESS", GC_ERR_SUCCESS)
        .value("GC_ERR_ERROR", GC_ERR_ERROR)
        .value("GC_ERR_NOT_INITIALIZED", GC_ERR_NOT_INITIALIZED)
        .value("GC_ERR_NOT_IMPLEMENTED", GC_ERR_NOT_IMPLEMENTED)
        .value("GC_ERR_RESOURCE_IN_USE", GC_ERR_RESOURCE_IN_USE)
        .value("GC_ERR_ACCESS_DENIED", GC_ERR_ACCESS_DENIED)
        .value("GC_ERR_INVALID_HANDLE", GC_ERR_INVALID_HANDLE)
        .value("GC_ERR_INVALID_ID", GC_ERR_INVALID_ID)
        .value("GC_ERR_NO_DATA", GC_ERR_NO_DATA)
        .value("GC_ERR_INVALID_PARAMETER", GC_ERR_INVALID_PARAMETER)
        .value("GC_ERR_IO", GC_ERR_IO)
        .value("GC_ERR_TIMEOUT", GC_ERR_TIMEOUT)
        .value("GC_ERR_ABORT", GC_ERR_ABORT)
        .value("GC_ERR_INVALID_BUFFER", GC_ERR_INVALID_BUFFER)
        .value("GC_ERR_NOT_AVAILABLE", GC_ERR_NOT_AVAILABLE)
        .value("GC_ERR_INVALID_ADDRESS", GC_ERR_INVALID_ADDRESS)
        .value("GC_ERR_BUFFER_TOO_SMALL", GC_ERR_BUFFER_TOO_SMALL)
        .value("GC_ERR_INVALID_INDEX", GC_ERR_INVALID_INDEX)
        .value("GC_ERR_PARSING_CHUNK_DATA", GC_ERR_PARSING_CHUNK_DATA)
        .value("GC_ERR_INVALID_VALUE", GC_ERR_INVALID_VALUE)
        .value("GC_ERR_RESOURCE_EXHAUSTED", GC_ERR_RESOURCE_EXHAUSTED)
        .value("GC_ERR_OUT_OF_MEMORY", GC_ERR_OUT_OF_MEMORY)
        .value("GC_ERR_BUSY", GC_ERR_BUSY)
        .value("GC_ERR_CUSTOM_ID", GC_ERR_CUSTOM_ID)
        .export_values();

    // ---- INFO_DATATYPE -----------------------------------------------------
    nb::enum_<INFO_DATATYPE_LIST>(m, "INFO_DATATYPE", nb::is_arithmetic())
        .value("UNKNOWN", INFO_DATATYPE_UNKNOWN)
        .value("STRING", INFO_DATATYPE_STRING)
        .value("STRINGLIST", INFO_DATATYPE_STRINGLIST)
        .value("INT16", INFO_DATATYPE_INT16)
        .value("UINT16", INFO_DATATYPE_UINT16)
        .value("INT32", INFO_DATATYPE_INT32)
        .value("UINT32", INFO_DATATYPE_UINT32)
        .value("INT64", INFO_DATATYPE_INT64)
        .value("UINT64", INFO_DATATYPE_UINT64)
        .value("FLOAT64", INFO_DATATYPE_FLOAT64)
        .value("PTR", INFO_DATATYPE_PTR)
        .value("BOOL8", INFO_DATATYPE_BOOL8)
        .value("SIZET", INFO_DATATYPE_SIZET)
        .value("BUFFER", INFO_DATATYPE_BUFFER)
        .value("PTRDIFF", INFO_DATATYPE_PTRDIFF)
        .export_values();

    // ---- TL_INFO_CMD -------------------------------------------------------
    nb::enum_<TL_INFO_CMD_LIST>(m, "TL_INFO_CMD", nb::is_arithmetic())
        .value("TL_INFO_ID", TL_INFO_ID)
        .value("TL_INFO_VENDOR", TL_INFO_VENDOR)
        .value("TL_INFO_MODEL", TL_INFO_MODEL)
        .value("TL_INFO_VERSION", TL_INFO_VERSION)
        .value("TL_INFO_TLTYPE", TL_INFO_TLTYPE)
        .value("TL_INFO_NAME", TL_INFO_NAME)
        .value("TL_INFO_PATHNAME", TL_INFO_PATHNAME)
        .value("TL_INFO_DISPLAYNAME", TL_INFO_DISPLAYNAME)
        .value("TL_INFO_CHAR_ENCODING", TL_INFO_CHAR_ENCODING)
        .value("TL_INFO_GENTL_VER_MAJOR", TL_INFO_GENTL_VER_MAJOR)
        .value("TL_INFO_GENTL_VER_MINOR", TL_INFO_GENTL_VER_MINOR)
        .export_values();

    // ---- INTERFACE_INFO_CMD -------------------------------------------------
    nb::enum_<INTERFACE_INFO_CMD_LIST>(m, "INTERFACE_INFO_CMD",
                                        nb::is_arithmetic())
        .value("INTERFACE_INFO_ID", INTERFACE_INFO_ID)
        .value("INTERFACE_INFO_DISPLAYNAME", INTERFACE_INFO_DISPLAYNAME)
        .value("INTERFACE_INFO_TLTYPE", INTERFACE_INFO_TLTYPE)
        .export_values();

    // ---- DEVICE_ACCESS_FLAGS ------------------------------------------------
    nb::enum_<DEVICE_ACCESS_FLAGS_LIST>(m, "DEVICE_ACCESS_FLAGS",
                                         nb::is_arithmetic())
        .value("UNKNOWN", DEVICE_ACCESS_UNKNOWN)
        .value("NONE", DEVICE_ACCESS_NONE)
        .value("READONLY", DEVICE_ACCESS_READONLY)
        .value("CONTROL", DEVICE_ACCESS_CONTROL)
        .value("EXCLUSIVE", DEVICE_ACCESS_EXCLUSIVE)
        .export_values();

    // ---- DEVICE_ACCESS_STATUS -----------------------------------------------
    nb::enum_<DEVICE_ACCESS_STATUS_LIST>(m, "DEVICE_ACCESS_STATUS",
                                          nb::is_arithmetic())
        .value("UNKNOWN", DEVICE_ACCESS_STATUS_UNKNOWN)
        .value("READWRITE", DEVICE_ACCESS_STATUS_READWRITE)
        .value("READONLY", DEVICE_ACCESS_STATUS_READONLY)
        .value("NOACCESS", DEVICE_ACCESS_STATUS_NOACCESS)
        .value("BUSY", DEVICE_ACCESS_STATUS_BUSY)
        .value("OPEN_READWRITE", DEVICE_ACCESS_STATUS_OPEN_READWRITE)
        .value("OPEN_READONLY", DEVICE_ACCESS_STATUS_OPEN_READONLY)
        .export_values();

    // ---- DEVICE_INFO_CMD -----------------------------------------------------
    nb::enum_<DEVICE_INFO_CMD_LIST>(m, "DEVICE_INFO_CMD", nb::is_arithmetic())
        .value("DEVICE_INFO_ID", DEVICE_INFO_ID)
        .value("DEVICE_INFO_VENDOR", DEVICE_INFO_VENDOR)
        .value("DEVICE_INFO_MODEL", DEVICE_INFO_MODEL)
        .value("DEVICE_INFO_TLTYPE", DEVICE_INFO_TLTYPE)
        .value("DEVICE_INFO_DISPLAYNAME", DEVICE_INFO_DISPLAYNAME)
        .value("DEVICE_INFO_ACCESS_STATUS", DEVICE_INFO_ACCESS_STATUS)
        .value("DEVICE_INFO_USER_DEFINED_NAME",
               DEVICE_INFO_USER_DEFINED_NAME)
        .value("DEVICE_INFO_SERIAL_NUMBER", DEVICE_INFO_SERIAL_NUMBER)
        .value("DEVICE_INFO_VERSION", DEVICE_INFO_VERSION)
        .value("DEVICE_INFO_TIMESTAMP_FREQUENCY",
               DEVICE_INFO_TIMESTAMP_FREQUENCY)
        .export_values();

    // ---- ACQ_* ---------------------------------------------------------------
    nb::enum_<ACQ_STOP_FLAGS_LIST>(m, "ACQ_STOP_FLAGS", nb::is_arithmetic())
        .value("DEFAULT", ACQ_STOP_FLAGS_DEFAULT)
        .value("KILL", ACQ_STOP_FLAGS_KILL)
        .export_values();

    nb::enum_<ACQ_START_FLAGS_LIST>(m, "ACQ_START_FLAGS", nb::is_arithmetic())
        .value("DEFAULT", ACQ_START_FLAGS_DEFAULT)
        .export_values();

    nb::enum_<ACQ_QUEUE_TYPE_LIST>(m, "ACQ_QUEUE_TYPE", nb::is_arithmetic())
        .value("INPUT_TO_OUTPUT", ACQ_QUEUE_INPUT_TO_OUTPUT)
        .value("OUTPUT_DISCARD", ACQ_QUEUE_OUTPUT_DISCARD)
        .value("ALL_TO_INPUT", ACQ_QUEUE_ALL_TO_INPUT)
        .value("UNQUEUED_TO_INPUT", ACQ_QUEUE_UNQUEUED_TO_INPUT)
        .value("ALL_DISCARD", ACQ_QUEUE_ALL_DISCARD)
        .export_values();

    // ---- STREAM_INFO_CMD ------------------------------------------------------
    nb::enum_<STREAM_INFO_CMD_LIST>(m, "STREAM_INFO_CMD", nb::is_arithmetic())
        .value("STREAM_INFO_ID", STREAM_INFO_ID)
        .value("STREAM_INFO_NUM_DELIVERED", STREAM_INFO_NUM_DELIVERED)
        .value("STREAM_INFO_NUM_UNDERRUN", STREAM_INFO_NUM_UNDERRUN)
        .value("STREAM_INFO_NUM_ANNOUNCED", STREAM_INFO_NUM_ANNOUNCED)
        .value("STREAM_INFO_NUM_QUEUED", STREAM_INFO_NUM_QUEUED)
        .value("STREAM_INFO_NUM_AWAIT_DELIVERY",
               STREAM_INFO_NUM_AWAIT_DELIVERY)
        .value("STREAM_INFO_NUM_STARTED", STREAM_INFO_NUM_STARTED)
        .value("STREAM_INFO_PAYLOAD_SIZE", STREAM_INFO_PAYLOAD_SIZE)
        .value("STREAM_INFO_IS_GRABBING", STREAM_INFO_IS_GRABBING)
        .value("STREAM_INFO_DEFINES_PAYLOADSIZE",
               STREAM_INFO_DEFINES_PAYLOADSIZE)
        .value("STREAM_INFO_TLTYPE", STREAM_INFO_TLTYPE)
        .value("STREAM_INFO_NUM_CHUNKS_MAX", STREAM_INFO_NUM_CHUNKS_MAX)
        .value("STREAM_INFO_BUF_ANNOUNCE_MIN",
               STREAM_INFO_BUF_ANNOUNCE_MIN)
        .value("STREAM_INFO_BUF_ALIGNMENT", STREAM_INFO_BUF_ALIGNMENT)
        .export_values();

    // ---- BUFFER_INFO_CMD -------------------------------------------------------
    nb::enum_<BUFFER_INFO_CMD_LIST>(m, "BUFFER_INFO_CMD", nb::is_arithmetic())
        .value("BUFFER_INFO_BASE", BUFFER_INFO_BASE)
        .value("BUFFER_INFO_SIZE", BUFFER_INFO_SIZE)
        .value("BUFFER_INFO_USER_PTR", BUFFER_INFO_USER_PTR)
        .value("BUFFER_INFO_TIMESTAMP", BUFFER_INFO_TIMESTAMP)
        .value("BUFFER_INFO_NEW_DATA", BUFFER_INFO_NEW_DATA)
        .value("BUFFER_INFO_IS_QUEUED", BUFFER_INFO_IS_QUEUED)
        .value("BUFFER_INFO_IS_ACQUIRING", BUFFER_INFO_IS_ACQUIRING)
        .value("BUFFER_INFO_IS_INCOMPLETE", BUFFER_INFO_IS_INCOMPLETE)
        .value("BUFFER_INFO_TLTYPE", BUFFER_INFO_TLTYPE)
        .value("BUFFER_INFO_SIZE_FILLED", BUFFER_INFO_SIZE_FILLED)
        .value("BUFFER_INFO_WIDTH", BUFFER_INFO_WIDTH)
        .value("BUFFER_INFO_HEIGHT", BUFFER_INFO_HEIGHT)
        .value("BUFFER_INFO_XOFFSET", BUFFER_INFO_XOFFSET)
        .value("BUFFER_INFO_YOFFSET", BUFFER_INFO_YOFFSET)
        .value("BUFFER_INFO_XPADDING", BUFFER_INFO_XPADDING)
        .value("BUFFER_INFO_YPADDING", BUFFER_INFO_YPADDING)
        .value("BUFFER_INFO_FRAMEID", BUFFER_INFO_FRAMEID)
        .value("BUFFER_INFO_IMAGEPRESENT", BUFFER_INFO_IMAGEPRESENT)
        .value("BUFFER_INFO_IMAGEOFFSET", BUFFER_INFO_IMAGEOFFSET)
        .value("BUFFER_INFO_PAYLOADTYPE", BUFFER_INFO_PAYLOADTYPE)
        .value("BUFFER_INFO_PIXELFORMAT", BUFFER_INFO_PIXELFORMAT)
        .value("BUFFER_INFO_PIXELFORMAT_NAMESPACE",
               BUFFER_INFO_PIXELFORMAT_NAMESPACE)
        .value("BUFFER_INFO_DELIVERED_IMAGEHEIGHT",
               BUFFER_INFO_DELIVERED_IMAGEHEIGHT)
        .value("BUFFER_INFO_DELIVERED_CHUNKPAYLOADSIZE",
               BUFFER_INFO_DELIVERED_CHUNKPAYLOADSIZE)
        .value("BUFFER_INFO_CHUNKLAYOUTID", BUFFER_INFO_CHUNKLAYOUTID)
        .value("BUFFER_INFO_FILENAME", BUFFER_INFO_FILENAME)
        .value("BUFFER_INFO_PIXEL_ENDIANNESS",
               BUFFER_INFO_PIXEL_ENDIANNESS)
        .value("BUFFER_INFO_DATA_SIZE", BUFFER_INFO_DATA_SIZE)
        .value("BUFFER_INFO_TIMESTAMP_NS", BUFFER_INFO_TIMESTAMP_NS)
        .value("BUFFER_INFO_DATA_LARGER_THAN_BUFFER",
               BUFFER_INFO_DATA_LARGER_THAN_BUFFER)
        .value("BUFFER_INFO_CONTAINS_CHUNKDATA",
               BUFFER_INFO_CONTAINS_CHUNKDATA)
        .export_values();

    // ---- BUFFER_PART_INFO_CMD -----------------------------------------------
    nb::enum_<BUFFER_PART_INFO_CMD_LIST>(m, "BUFFER_PART_INFO_CMD",
                                          nb::is_arithmetic())
        .value("BUFFER_PART_INFO_BASE", BUFFER_PART_INFO_BASE)
        .value("BUFFER_PART_INFO_DATA_SIZE", BUFFER_PART_INFO_DATA_SIZE)
        .value("BUFFER_PART_INFO_DATA_TYPE", BUFFER_PART_INFO_DATA_TYPE)
        .value("BUFFER_PART_INFO_DATA_FORMAT", BUFFER_PART_INFO_DATA_FORMAT)
        .value("BUFFER_PART_INFO_DATA_FORMAT_NAMESPACE",
               BUFFER_PART_INFO_DATA_FORMAT_NAMESPACE)
        .value("BUFFER_PART_INFO_WIDTH", BUFFER_PART_INFO_WIDTH)
        .value("BUFFER_PART_INFO_HEIGHT", BUFFER_PART_INFO_HEIGHT)
        .value("BUFFER_PART_INFO_XOFFSET", BUFFER_PART_INFO_XOFFSET)
        .value("BUFFER_PART_INFO_YOFFSET", BUFFER_PART_INFO_YOFFSET)
        .value("BUFFER_PART_INFO_XPADDING", BUFFER_PART_INFO_XPADDING)
        .value("BUFFER_PART_INFO_SOURCE_ID", BUFFER_PART_INFO_SOURCE_ID)
        .value("BUFFER_PART_INFO_DELIVERED_IMAGEHEIGHT",
               BUFFER_PART_INFO_DELIVERED_IMAGEHEIGHT)
        .export_values();

    // ---- PAYLOADTYPE_INFO_IDS ----------------------------------------------
    nb::enum_<PAYLOADTYPE_INFO_IDS>(m, "PAYLOADTYPE_INFO_ID",
                                      nb::is_arithmetic())
        .value("UNKNOWN", PAYLOAD_TYPE_UNKNOWN)
        .value("IMAGE", PAYLOAD_TYPE_IMAGE)
        .value("RAW_DATA", PAYLOAD_TYPE_RAW_DATA)
        .value("FILE", PAYLOAD_TYPE_FILE)
        .value("CHUNK_DATA", PAYLOAD_TYPE_CHUNK_DATA)
        .value("JPEG", PAYLOAD_TYPE_JPEG)
        .value("JPEG2000", PAYLOAD_TYPE_JPEG2000)
        .value("H264", PAYLOAD_TYPE_H264)
        .value("CHUNK_ONLY", PAYLOAD_TYPE_CHUNK_ONLY)
        .value("DEVICE_SPECIFIC", PAYLOAD_TYPE_DEVICE_SPECIFIC)
        .value("MULTI_PART", PAYLOAD_TYPE_MULTI_PART)
        .export_values();

    // ---- PIXELFORMAT_NAMESPACE_IDS -----------------------------------------
    nb::enum_<PIXELFORMAT_NAMESPACE_IDS>(m, "PIXELFORMAT_NAMESPACE_ID",
                                          nb::is_arithmetic())
        .value("UNKNOWN", PIXELFORMAT_NAMESPACE_UNKNOWN)
        .value("GEV", PIXELFORMAT_NAMESPACE_GEV)
        .value("IIDC", PIXELFORMAT_NAMESPACE_IIDC)
        .value("PFNC_16BIT", PIXELFORMAT_NAMESPACE_PFNC_16BIT)
        .value("PFNC_32BIT", PIXELFORMAT_NAMESPACE_PFNC_32BIT)
        .export_values();

    // ---- PORT_INFO_CMD -------------------------------------------------------
    nb::enum_<PORT_INFO_CMD_LIST>(m, "PORT_INFO_CMD", nb::is_arithmetic())
        .value("PORT_INFO_ID", PORT_INFO_ID)
        .value("PORT_INFO_VENDOR", PORT_INFO_VENDOR)
        .value("PORT_INFO_MODEL", PORT_INFO_MODEL)
        .value("PORT_INFO_TLTYPE", PORT_INFO_TLTYPE)
        .value("PORT_INFO_MODULE", PORT_INFO_MODULE)
        .value("PORT_INFO_LITTLE_ENDIAN", PORT_INFO_LITTLE_ENDIAN)
        .value("PORT_INFO_BIG_ENDIAN", PORT_INFO_BIG_ENDIAN)
        .value("PORT_INFO_ACCESS_READ", PORT_INFO_ACCESS_READ)
        .value("PORT_INFO_ACCESS_WRITE", PORT_INFO_ACCESS_WRITE)
        .value("PORT_INFO_ACCESS_NA", PORT_INFO_ACCESS_NA)
        .value("PORT_INFO_ACCESS_NI", PORT_INFO_ACCESS_NI)
        .value("PORT_INFO_VERSION", PORT_INFO_VERSION)
        .value("PORT_INFO_PORTNAME", PORT_INFO_PORTNAME)
        .export_values();

    // ---- URL_INFO_CMD / URL_SCHEME_IDS -------------------------------------
    nb::enum_<URL_INFO_CMD_LIST>(m, "URL_INFO_CMD", nb::is_arithmetic())
        .value("URL_INFO_URL", URL_INFO_URL)
        .value("URL_INFO_SCHEMA_VER_MAJOR", URL_INFO_SCHEMA_VER_MAJOR)
        .value("URL_INFO_SCHEMA_VER_MINOR", URL_INFO_SCHEMA_VER_MINOR)
        .value("URL_INFO_FILE_VER_MAJOR", URL_INFO_FILE_VER_MAJOR)
        .value("URL_INFO_FILE_VER_MINOR", URL_INFO_FILE_VER_MINOR)
        .value("URL_INFO_FILE_VER_SUBMINOR", URL_INFO_FILE_VER_SUBMINOR)
        .value("URL_INFO_FILE_SHA1_HASH", URL_INFO_FILE_SHA1_HASH)
        .value("URL_INFO_FILE_REGISTER_ADDRESS",
               URL_INFO_FILE_REGISTER_ADDRESS)
        .value("URL_INFO_FILE_SIZE", URL_INFO_FILE_SIZE)
        .value("URL_INFO_SCHEME", URL_INFO_SCHEME)
        .value("URL_INFO_FILENAME", URL_INFO_FILENAME)
        .export_values();

    nb::enum_<URL_SCHEME_IDS>(m, "URL_SCHEME_ID", nb::is_arithmetic())
        .value("LOCAL", URL_SCHEME_LOCAL)
        .value("HTTP", URL_SCHEME_HTTP)
        .value("FILE", URL_SCHEME_FILE)
        .export_values();

    // ---- EVENT_TYPE / EVENT_INFO_CMD / EVENT_DATA_INFO_CMD -----------------
    nb::enum_<EVENT_TYPE_LIST>(m, "EVENT_TYPE", nb::is_arithmetic())
        .value("ERROR", EVENT_ERROR)
        .value("NEW_BUFFER", EVENT_NEW_BUFFER)
        .value("FEATURE_INVALIDATE", EVENT_FEATURE_INVALIDATE)
        .value("FEATURE_CHANGE", EVENT_FEATURE_CHANGE)
        .value("REMOTE_DEVICE", EVENT_REMOTE_DEVICE)
        .value("MODULE", EVENT_MODULE)
        .export_values();

    nb::enum_<EVENT_INFO_CMD_LIST>(m, "EVENT_INFO_CMD", nb::is_arithmetic())
        .value("EVENT_EVENT_TYPE", EVENT_EVENT_TYPE)
        .value("EVENT_NUM_IN_QUEUE", EVENT_NUM_IN_QUEUE)
        .value("EVENT_NUM_FIRED", EVENT_NUM_FIRED)
        .value("EVENT_SIZE_MAX", EVENT_SIZE_MAX)
        .value("EVENT_INFO_DATA_SIZE_MAX", EVENT_INFO_DATA_SIZE_MAX)
        .export_values();

    nb::enum_<EVENT_DATA_INFO_CMD_LIST>(m, "EVENT_DATA_INFO_CMD",
                                         nb::is_arithmetic())
        .value("EVENT_DATA_ID", EVENT_DATA_ID)
        .value("EVENT_DATA_VALUE", EVENT_DATA_VALUE)
        .value("EVENT_DATA_NUMID", EVENT_DATA_NUMID)
        .export_values();

    // ---- PARTDATATYPE_IDS ---------------------------------------------------
    nb::enum_<PARTDATATYPE_IDS>(m, "PARTDATATYPE_ID", nb::is_arithmetic())
        .value("UNKNOWN", PART_DATATYPE_UNKNOWN)
        .value("2D_IMAGE", PART_DATATYPE_2D_IMAGE)
        .value("2D_PLANE_BIPLANAR", PART_DATATYPE_2D_PLANE_BIPLANAR)
        .value("2D_PLANE_TRIPLANAR", PART_DATATYPE_2D_PLANE_TRIPLANAR)
        .value("2D_PLANE_QUADPLANAR", PART_DATATYPE_2D_PLANE_QUADPLANAR)
        .value("3D_IMAGE", PART_DATATYPE_3D_IMAGE)
        .value("3D_PLANE_BIPLANAR", PART_DATATYPE_3D_PLANE_BIPLANAR)
        .value("3D_PLANE_TRIPLANAR", PART_DATATYPE_3D_PLANE_TRIPLANAR)
        .value("3D_PLANE_QUADPLANAR", PART_DATATYPE_3D_PLANE_QUADPLANAR)
        .value("CONFIDENCE_MAP", PART_DATATYPE_CONFIDENCE_MAP)
        .export_values();

    // ---- Port --------------------------------------------------------------
    nb::class_<PyPort>(m, "Port",
                         "Register-level access to a GenTL module or a "
                         "remote device (GenApi IPort backend).")
        .def("read", &PyPort::read, "address"_a, "size"_a,
             "Read `size` bytes starting at `address`.")
        .def("write", &PyPort::write, "address"_a, "data"_a,
             "Write `data` at `address`; returns the number of bytes "
             "actually written.")
        .def("get_info", &PyPort::get_info, "cmd"_a)
        .def("get_num_urls", &PyPort::get_num_urls)
        .def("get_url_info", &PyPort::get_url_info, "index"_a, "cmd"_a);

    // ---- Event --------------------------------------------------------------
    nb::class_<PyEvent>(m, "Event", "A registered GenTL event source.")
        .def("get_data", &PyEvent::get_data, "timeout_ms"_a = 0,
             "Block until the event fires (or `timeout_ms` elapses) and "
             "return its raw payload.")
        .def("get_new_buffer_data", &PyEvent::get_new_buffer_data,
             "timeout_ms"_a = 0,
             "For events registered via "
             "DataStream.register_new_buffer_event(): block until a "
             "buffer is delivered and return "
             "(buffer_handle, user_pointer).")
        .def("flush", &PyEvent::flush)
        .def("kill", &PyEvent::kill,
             "Abort a single pending get_data() call.")
        .def("get_info", &PyEvent::get_info, "cmd"_a);

    // ---- Buffer ------------------------------------------------------------
    nb::class_<PyBuffer>(m, "Buffer",
                           "A single acquisition buffer announced to a "
                           "DataStream.")
        .def("queue", &PyBuffer::queue,
             "Put this buffer into the input pool for acquisition.")
        .def("revoke", &PyBuffer::revoke,
             "Remove this buffer from the acquisition engine; returns "
             "(base_address, private_ptr).")
        .def("get_info", &PyBuffer::get_info, "cmd"_a)
        .def("get_data", &PyBuffer::get_data,
             "Return a *copy* of the buffer's current payload as bytes.")
        .def_prop_ro("handle", &PyBuffer::handle_value);

    // ---- DataStream --------------------------------------------------------
    nb::class_<PyDataStream>(m, "DataStream",
                               "An acquisition channel opened on a Device.")
        .def("close", &PyDataStream::close)
        .def("announce_buffer", &PyDataStream::announce_buffer, "data"_a,
             "private_ptr"_a = 0,
             "Announce consumer-allocated memory (kept alive by Python) to "
             "the acquisition engine.")
        .def("alloc_and_announce_buffer",
             &PyDataStream::alloc_and_announce_buffer, "size"_a,
             "private_ptr"_a = 0,
             "Ask the Producer to allocate and announce a buffer.")
        .def("flush_queue", &PyDataStream::flush_queue, "op"_a)
        .def("start_acquisition", &PyDataStream::start_acquisition,
             "flags"_a = ACQ_START_FLAGS_DEFAULT,
             "num_to_acquire"_a = GENTL_INFINITE)
        .def("stop_acquisition", &PyDataStream::stop_acquisition,
             "flags"_a = ACQ_STOP_FLAGS_DEFAULT)
        .def("get_info", &PyDataStream::get_info, "cmd"_a)
        .def("get_buffer_by_index", &PyDataStream::get_buffer_by_index,
             "index"_a)
        .def("register_new_buffer_event",
             &PyDataStream::register_new_buffer_event,
             "Register for the EVENT_NEW_BUFFER notification.")
        .def("__enter__", [](std::shared_ptr<PyDataStream> self) { return self; })
        .def(
            "__exit__",
            [](PyDataStream &self, nb::object, nb::object, nb::object) {
                self.close();
            },
            "exc_type"_a.none(), "exc_value"_a.none(),
            "traceback"_a.none());

    // ---- Device --------------------------------------------------------------
    nb::class_<PyDevice>(m, "Device", "A remote device opened on an Interface.")
        .def("close", &PyDevice::close)
        .def("get_port", &PyDevice::get_port,
             "Port to access the remote device's own register map.")
        .def("control_port", &PyDevice::control_port,
             "Port to access this local Device module's own register map.")
        .def("get_num_data_streams", &PyDevice::get_num_data_streams)
        .def("get_data_stream_id", &PyDevice::get_data_stream_id, "index"_a)
        .def("open_data_stream", &PyDevice::open_data_stream, "stream_id"_a)
        .def("get_info", &PyDevice::get_info, "cmd"_a)
        .def("__enter__", [](std::shared_ptr<PyDevice> self) { return self; })
        .def(
            "__exit__",
            [](PyDevice &self, nb::object, nb::object, nb::object) {
                self.close();
            },
            "exc_type"_a.none(), "exc_value"_a.none(),
            "traceback"_a.none());

    // ---- Interface --------------------------------------------------------------
    nb::class_<PyInterface>(m, "Interface", "A physical interface opened on a System.")
        .def("close", &PyInterface::close)
        .def("update_device_list", &PyInterface::update_device_list,
             "timeout_ms"_a = GENTL_INFINITE)
        .def("get_num_devices", &PyInterface::get_num_devices)
        .def("get_device_id", &PyInterface::get_device_id, "index"_a)
        .def("get_device_info", &PyInterface::get_device_info, "device_id"_a,
             "cmd"_a)
        .def("open_device", &PyInterface::open_device, "device_id"_a,
             "flags"_a = DEVICE_ACCESS_EXCLUSIVE)
        .def("get_info", &PyInterface::get_info, "cmd"_a)
        .def("__enter__", [](std::shared_ptr<PyInterface> self) { return self; })
        .def(
            "__exit__",
            [](PyInterface &self, nb::object, nb::object, nb::object) {
                self.close();
            },
            "exc_type"_a.none(), "exc_value"_a.none(),
            "traceback"_a.none());

    // ---- System --------------------------------------------------------------
    nb::class_<PySystem>(m, "System", "The GenTL Producer's root (TL) module.")
        .def("close", &PySystem::close)
        .def("update_interface_list", &PySystem::update_interface_list,
             "timeout_ms"_a = GENTL_INFINITE)
        .def("get_num_interfaces", &PySystem::get_num_interfaces)
        .def("get_interface_id", &PySystem::get_interface_id, "index"_a)
        .def("get_interface_info", &PySystem::get_interface_info,
             "interface_id"_a, "cmd"_a)
        .def("open_interface", &PySystem::open_interface, "interface_id"_a)
        .def("get_info", &PySystem::get_info, "cmd"_a)
        .def("__enter__", [](std::shared_ptr<PySystem> self) { return self; })
        .def(
            "__exit__",
            [](PySystem &self, nb::object, nb::object, nb::object) {
                self.close();
            },
            "exc_type"_a.none(), "exc_value"_a.none(),
            "traceback"_a.none());

    // ---- Producer --------------------------------------------------------------
    nb::class_<PyProducer>(
        m, "Producer",
        "Loads a GenTL Producer (.cti) shared library and brackets its "
        "GCInitLib/GCCloseLib lifecycle.\n\n"
        "Example\n"
        "-------\n"
        ">>> with gentl15.Producer('/path/to/producer.cti') as producer:\n"
        "...     system = producer.open_system()\n")
        .def(nb::init<const std::string &>(), "path"_a)
        .def("init_lib", &PyProducer::init_lib)
        .def("close_lib", &PyProducer::close_lib)
        .def("get_info", &PyProducer::get_info, "cmd"_a)
        .def("open_system", &PyProducer::open_system)
        .def(
            "__enter__",
            [](PyProducer &self) -> PyProducer & {
                self.init_lib();
                return self;
            },
            nb::rv_policy::reference_internal)
        .def("__exit__", &PyProducer::exit, "exc_type"_a.none(),
             "exc_value"_a.none(), "traceback"_a.none());

    m.def(
        "attach_data_stream", &attach_data_stream, "cti_path"_a, "handle"_a,
        "Attach to a DS_HANDLE already opened by a different GenTL binding "
        "in this process (e.g. genicam.gentl), so its buffers can be "
        "managed with Producer-controlled allocation "
        "(DataStream.alloc_and_announce_buffer). Does not take ownership: "
        "the returned DataStream's close() is a no-op with respect to the "
        "underlying handle -- whoever originally opened it remains "
        "responsible for closing it.");
} // register_module

} // namespace gentl15

NB_MODULE(gentl15, m) {
    gentl15::register_module(m);
}
