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

# ---------------------------------------------------------------------------
# gentl -- GenTL 1.6 bindings
# ---------------------------------------------------------------------------
nanobind_add_module(
  gentl
  STABLE_ABI
  LTO
  NB_STATIC
  src/gentl.cpp
)

target_include_directories(gentl PRIVATE src/headers)

if (NOT WIN32)
  target_link_libraries(gentl PRIVATE ${CMAKE_DL_LIBS})
endif()

nanobind_add_stub(
  gentl_stub
  MODULE gentl
  OUTPUT gentl.pyi
  PYTHON_PATH $<TARGET_FILE_DIR:gentl>
  DEPENDS gentl
)

install(TARGETS gentl LIBRARY DESTINATION .)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gentl.pyi DESTINATION .)

# ---------------------------------------------------------------------------
# gentl15 -- GenTL 1.5 bindings (GenICam Package v3.0.2)
# ---------------------------------------------------------------------------
nanobind_add_module(
  gentl15
  STABLE_ABI
  LTO
  NB_STATIC
  src/gentl15.cpp
)

target_include_directories(gentl15 PRIVATE src/headers)

if (NOT WIN32)
  target_link_libraries(gentl15 PRIVATE ${CMAKE_DL_LIBS})
endif()

nanobind_add_stub(
  gentl15_stub
  MODULE gentl15
  OUTPUT gentl15.pyi
  PYTHON_PATH $<TARGET_FILE_DIR:gentl15>
  DEPENDS gentl15
)

install(TARGETS gentl15 LIBRARY DESTINATION .)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gentl15.pyi DESTINATION .)
