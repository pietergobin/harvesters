# Installation & Building

## Prerequisites

- Python ≥ 3.10
- [uv](https://docs.astral.sh/uv/) (used for the dev environment) or plain
  `pip` + a virtualenv
- CMake ≥ 3.15
- A C++17 compiler
- A GenTL Producer to test against: a vendor-supplied `.cti` shared library
  (e.g. `libmvGenTLProducer.so` / `mvGenTLProducer.cti` from MATRIX VISION,
  or any other GenICam-compliant transport layer). Harvesters itself does
  **not** ship a Producer — you install the one that matches your camera's
  interface (GigE Vision, USB3 Vision, CoaXPress, …).

## Building the extension

From the `bundled_genicam/` directory:

```bash
uv sync                       # installs the nanobind build dependency
source .venv/bin/activate

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

This produces two artifacts inside `build/`:

- `gentl.cpython-3XY-<platform>.so` — the compiled extension module.
- `gentl.pyi` — a fully typed stub, generated automatically via
  `nanobind_add_stub()`, so your editor/type-checker sees the complete API
  surface (enums, classes, method signatures) without needing to inspect the
  compiled binary.

To use the module from a script or REPL without installing it, just point
`PYTHONPATH` (or `sys.path`) at the `build/` directory:

```bash
PYTHONPATH=build python -c "import gentl; print(gentl.GENTL_MAJOR_VERSION)"
```

## Locating a Producer on your system

GenTL Producers are conventionally installed into paths referenced by the
`GENICAM_GENTL32_PATH` / `GENICAM_GENTL64_PATH` environment variables
(GenTL spec, chapter 6.1.1), one `.cti` file per vendor/technology. A small
helper to discover them:

```python
import os
import pathlib

def discover_cti_files() -> list[pathlib.Path]:
    var = "GENICAM_GENTL64_PATH" if os.name != "nt" or True else "GENICAM_GENTL32_PATH"
    paths = os.environ.get(var, "").split(os.pathsep)
    result = []
    for p in filter(None, paths):
        result.extend(pathlib.Path(p).glob("*.cti"))
    return result
```

For quick experimentation you can of course just hard-code the path, e.g.
on a Linux box with a MATRIX VISION mvIMPACT Acquire installation:

```python
CTI_PATH = "/opt/ImpactAcquire/lib/x86_64/mvGenTLProducer.cti"
```
