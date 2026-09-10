# Type Stubs & IDE Support

The CMake build generates a complete `gentl.pyi` stub next to the compiled
extension via nanobind's `nanobind_add_stub()`:

```cmake
nanobind_add_stub(
  gentl_stub
  MODULE gentl
  OUTPUT gentl.pyi
  PYTHON_PATH $<TARGET_FILE_DIR:gentl>
  DEPENDS gentl
)
```

This means every class, method, and enum documented in this guide is also
visible to your editor/type-checker (Pylance, mypy, pyright, PyCharm, ...)
without any extra configuration — as long as `gentl.pyi` sits next to
`gentl*.so`/`gentl*.pyd`, which is exactly where the build places it.

A few things worth knowing about the generated stub:

- Enum parameters use the real GenTL enum types (e.g.
  `def get_info(self, cmd: TL_INFO_CMD) -> object: ...`), not raw `int`, so
  autocomplete on `gentl.TL_INFO_CMD.<Tab>` works as expected at call sites.
- `get_info()`/`get_url_info()` are necessarily typed as returning `object`:
  their actual Python type depends on the `INFO_DATATYPE` the Producer
  reports at *runtime* for that particular command (see the decoding table
  in [Core Concepts](./concepts.md#get_info-and-the-info_datatype-decoding)),
  which a static stub cannot express. Narrow the type yourself at the call
  site if needed, e.g.:

  ```python
  from typing import cast

  vendor = cast(str, producer.get_info(gentl.TL_INFO_CMD.TL_INFO_VENDOR))
  ```

- Module-level constants (`GENTL_MAJOR_VERSION`, `GENTL_INFINITE`,
  `GENTL_INVALID_HANDLE`, and every `GC_ERROR.*`/enum member re-exported at
  module scope) are typed with concrete literal values.

## Regenerating the stub

The stub is a build artifact, regenerated automatically whenever `gentl.cpp`
changes and you re-run:

```bash
cmake --build build
```

If you only need to refresh the stub without recompiling the extension
(e.g. after editing docstrings only), you can invoke the underlying
nanobind CLI stub generator directly:

```bash
python -m nanobind.stubgen -m gentl -O build
```
