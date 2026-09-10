cmake_minimum_required(VERSION 3.15...3.27)
project(genicam-py) # Replace 'my_project' with the name of your project

if (CMAKE_VERSION VERSION_LESS 3.18)
  set(DEV_MODULE Development)
else()
  set(DEV_MODULE Development.Module)
endif()

find_package(Python 3.10 COMPONENTS Interpreter ${DEV_MODULE} REQUIRED)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

# Detect the installed nanobind package and import it into CMake
execute_process(
  COMMAND "${Python_EXECUTABLE}" -m nanobind --cmake_dir
  OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE nanobind_ROOT)
find_package(nanobind CONFIG REQUIRED)

# NB_STATIC/LTO/STABLE_ABI keep the wheel small and interpreter-version
# agnostic; STUB generates gentl.pyi for full static typing support.
nanobind_add_module(
  gentl
  STABLE_ABI
  LTO
  NB_STATIC
  src/gentl.cpp
)

target_include_directories(gentl PRIVATE src/headers)

if (NOT WIN32)
  # GenTL Producers (.cti) are loaded at runtime via dlopen()/dlsym().
  target_link_libraries(gentl PRIVATE ${CMAKE_DL_LIBS})
endif()

# Generate a fully typed .pyi stub next to the compiled extension so that
# IDEs/type-checkers see a complete, statically typed API surface.
nanobind_add_stub(
  gentl_stub
  MODULE gentl
  OUTPUT gentl.pyi
  PYTHON_PATH $<TARGET_FILE_DIR:gentl>
  DEPENDS gentl
)

install(TARGETS gentl LIBRARY DESTINATION .)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gentl.pyi DESTINATION .)
