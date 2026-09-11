# bundled_genicam

Build and packaging home for `gentl` and `gentl15`: nanobind-based Python
extensions that expose the GenICam GenTL Producer C interface, loading any
vendor's `.cti` shared library dynamically at runtime (see `src/gentl.cpp`
and `src/gentl15.cpp`).

- `gentl` targets **GenTL 1.6** (the current spec).
- `gentl15` targets **GenTL 1.5** (as specified in GenICam Package v3.0.2).

## Building

```bash
uv sync
source .venv/bin/activate
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

This produces `build/gentl*.so` and `build/gentl15*.so` (or `.pyd` on
Windows) plus fully typed `build/gentl.pyi` and `build/gentl15.pyi` stubs.

## Testing

The test suite (`tests/`) runs entirely against
[Viky](https://pypi.org/project/genicam/), the official EMVA GenTL Producer
simulator ("world first game playing GenTL Producer") shipped inside the
`genicam` PyPI package -- no camera hardware required. It's already
installed as a dev dependency by `uv sync`.

```bash
source .venv/bin/activate
pytest
```

Every test runs in its own forked subprocess (`pytest-forked`), because a
real GenTL Producer keeps process-global state (see `tests/conftest.py`'s
module docstring) and a native crash anywhere in the wrapper must not take
down the whole run. `pytest-timeout` guards against a stuck acquisition
call hanging forever. Both are pre-configured in `pyproject.toml`; plain
`pytest` picks them up automatically.

To point the suite at a different `.cti` (e.g. real hardware) or a
non-default build directory:

```bash
GENTL_TEST_CTI=/path/to/other/producer.cti GENTL_BUILD_DIR=build-debug pytest
```

## Documentation

The full user guide (installation, core concepts, quick start, a
Producer-controlled-buffers acquisition walkthrough, error handling, typing,
API reference, troubleshooting) lives under `docs/book/` and is built with
[mdBook](https://rust-lang.github.io/mdBook/):

```bash
cd docs/book
mdbook build      # renders static HTML into docs/book/book/
mdbook serve      # or: live-reloading local preview
```

Start reading at [`docs/book/src/introduction.md`](docs/book/src/introduction.md)
or the rendered [`docs/book/book/index.html`](docs/book/book/index.html)
after building.

The GenTL 1.6 specification PDF referenced throughout the guide is bundled
at `docs/GenICam_GenTL_1_6.pdf`.
