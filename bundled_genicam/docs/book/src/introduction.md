# Introduction

`gentl` is a small, fully typed Python extension module built with
[nanobind](https://nanobind.readthedocs.io/) that exposes the
[GenICam GenTL 1.6](https://www.emva.org/standards-technology/genicam/gentl/)
C interface to Python.

Unlike a typical binding project, `gentl` does **not** link against any
particular vendor's transport layer implementation at build time. A GenTL
*Producer* is a plugin (a `.cti` shared library) that is discovered and
loaded **at runtime**, exactly as described in chapter 6.1.1 of the GenTL
specification. `gentl.Producer(path)` does this for you: it `dlopen`s the
`.cti`, resolves every entry point it needs via `dlsym`, and wraps the raw C
API in a small set of Pythonic, exception-raising, context-manager-friendly
classes that mirror the GenTL module hierarchy:

```text
Producer (.cti)
└── System            (TL_HANDLE)
    └── Interface      (IF_HANDLE)
        └── Device     (DEV_HANDLE)
            └── DataStream   (DS_HANDLE)
                └── Buffer   (BUFFER_HANDLE)
```

Every level also exposes a `Port` for GenApi-style register access, and
`DataStream`/`Device`/etc. expose `Event` objects for GenTL's asynchronous
signaling mechanism (most importantly, the `EVENT_NEW_BUFFER` event used to
be notified about newly filled acquisition buffers).

This guide walks through:

- building the module and locating a `.cti` Producer on your system,
- the object model and how it maps to the GenTL C API,
- a complete quick-start example that opens a device and grabs a few frames,
- an in-depth guide to **Producer-controlled buffers** — the simplest and
  most portable acquisition mode, in which the Producer itself allocates
  and owns the buffer memory (`DSAllocAndAnnounceBuffer`), as opposed to
  Consumer-allocated buffers you provide yourself.

> **Note.** This module is a thin, faithful wrapper around the GenTL C API.
> If something here is unclear, the
> [GenTL 1.6 specification](../../GenICam_GenTL_1_6.pdf) bundled in
> `bundled_genicam/docs/` is the ultimate source of truth — every method
> below links back to the C function and GenTL chapter it implements.
