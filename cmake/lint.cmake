# Copyright (C) 2026 CharOfString <root@charofstring.cc>
#
# This program is free software: you can redistribute it and/or modify it under
# the terms of the GNU General Public License as published by the Free Software
# Foundation, either version 3 of the License, or (at your option) any later
# version.
#
# This program is distributed in the hope that it will be useful, but WITHOUT
# ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
# details.
#
# You should have received a copy of the GNU General Public License along with
# this program. If not, see <https://www.gnu.org/licenses/>.
# -----------------------------------------------------------------------------
# Lint targets for the first-party C++ code, which follows the Google C++ style:
#   cpplint             cpplint (CPPLINT.cfg)
#   clang-format        reformat in place (.clang-format)
#   clang-format-check  fail on unformatted files
#   clang-tidy          clang-tidy (.clang-tidy), after building flakewm
#   lint                cpplint and clang-format-check
# The C code keeps the KylinSoft style and vendored code its upstream style, so
# neither is linted.
#
# A .h file is C++ when a .cc or .cpp file with the same name sits next to it,
# or when it is listed in kLintCxxHeaders. .clang-format-ignore and
# .editorconfig name the same C++ headers, and clang-format-check fails when a
# C++ file is ignored by clang-format.

# cmake -P mode: the clang-format-check command, see below.
if(CMAKE_SCRIPT_MODE_FILE)
  execute_process(
    COMMAND "${CLANG_FORMAT}" --list-ignored ${FILES}
    WORKING_DIRECTORY "${ROOT}"
    OUTPUT_VARIABLE ignored
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(ignored)
    string(REPLACE "\n" "\n  " ignored "${ignored}")
    message(FATAL_ERROR "C++ files ignored by .clang-format-ignore, add a "
                        "!path line for them:\n  ${ignored}")
  endif()
  execute_process(
    COMMAND "${CLANG_FORMAT}" --dry-run --Werror ${FILES}
    WORKING_DIRECTORY "${ROOT}"
    RESULT_VARIABLE result)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Unformatted C++ files, run the clang-format target")
  endif()
  return()
endif()

# C++ headers without a source file of their own.
set(kLintCxxHeaders
  src/c_compact.h
)
set(kLintVendorRegex
  "^(libs|src/vendor|examples/gui|tools/flakectl/hypr(land|utils))/")

file(GLOB_RECURSE lint_candidates
  RELATIVE "${PROJECT_SOURCE_DIR}"
  CONFIGURE_DEPENDS
  "${PROJECT_SOURCE_DIR}/src/*.cc"
  "${PROJECT_SOURCE_DIR}/src/*.cpp"
  "${PROJECT_SOURCE_DIR}/src/*.h"
  "${PROJECT_SOURCE_DIR}/tools/*.cc"
  "${PROJECT_SOURCE_DIR}/tools/*.cpp"
  "${PROJECT_SOURCE_DIR}/tools/*.h"
  "${PROJECT_SOURCE_DIR}/tests/*.cc"
  "${PROJECT_SOURCE_DIR}/tests/*.cpp"
  "${PROJECT_SOURCE_DIR}/tests/*.h"
)
list(FILTER lint_candidates EXCLUDE REGEX "${kLintVendorRegex}")

set(lint_cxx_sources)
set(lint_cxx_headers)
foreach(file IN LISTS lint_candidates)
  if(file MATCHES "\\.(cc|cpp)$")
    list(APPEND lint_cxx_sources "${file}")
    continue()
  endif()
  string(REGEX REPLACE "\\.h$" "" stem "${file}")
  if(EXISTS "${PROJECT_SOURCE_DIR}/${stem}.cc" OR
     EXISTS "${PROJECT_SOURCE_DIR}/${stem}.cpp" OR
     file IN_LIST kLintCxxHeaders)
    list(APPEND lint_cxx_headers "${file}")
  endif()
endforeach()
set(lint_cxx_files ${lint_cxx_sources} ${lint_cxx_headers})

# Anchored regex for each file, as absolute paths in compile_commands.json.
function(lint_path_regexes out)
  set(regexes)
  foreach(file IN LISTS ARGN)
    string(REGEX REPLACE "([][.+*?^$(){}|\\])" "\\\\\\1" escaped
                         "${PROJECT_SOURCE_DIR}/${file}")
    list(APPEND regexes "^${escaped}$")
  endforeach()
  set(${out} "${regexes}" PARENT_SCOPE)
endfunction()

set(lint_targets)

find_program(CPPLINT_EXECUTABLE cpplint)
if(CPPLINT_EXECUTABLE)
  add_custom_target(cpplint
    COMMAND "${CPPLINT_EXECUTABLE}" --quiet ${lint_cxx_files}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "cpplint on C++ sources"
    VERBATIM)
  list(APPEND lint_targets cpplint)
endif()

find_program(CLANG_FORMAT_EXECUTABLE clang-format)
if(CLANG_FORMAT_EXECUTABLE)
  add_custom_target(clang-format
    COMMAND "${CLANG_FORMAT_EXECUTABLE}" -i ${lint_cxx_files}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "clang-format on C++ sources"
    VERBATIM)
  add_custom_target(clang-format-check
    COMMAND "${CMAKE_COMMAND}"
            "-DCLANG_FORMAT=${CLANG_FORMAT_EXECUTABLE}"
            "-DROOT=${PROJECT_SOURCE_DIR}"
            "-DFILES=${lint_cxx_files}"
            -P "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "Checking C++ formatting"
    VERBATIM)
  list(APPEND lint_targets clang-format-check)
endif()

# run-clang-tidy only takes the files found in compile_commands.json, so
# sources of targets that are not configured (no Qt, no flakectl) drop out.
find_program(CLANG_TIDY_EXECUTABLE clang-tidy)
find_program(RUN_CLANG_TIDY_EXECUTABLE run-clang-tidy)
if(CLANG_TIDY_EXECUTABLE AND RUN_CLANG_TIDY_EXECUTABLE)
  lint_path_regexes(tidy_sources ${lint_cxx_sources})
  lint_path_regexes(tidy_headers ${lint_cxx_headers})
  list(JOIN tidy_headers "|" tidy_header_filter)
  add_custom_target(clang-tidy
    COMMAND "${RUN_CLANG_TIDY_EXECUTABLE}"
            -clang-tidy-binary "${CLANG_TIDY_EXECUTABLE}"
            -p "${PROJECT_BINARY_DIR}"
            -header-filter "${tidy_header_filter}"
            -quiet
            ${tidy_sources}
    WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    COMMENT "clang-tidy on C++ sources"
    VERBATIM)
  # generated protocol and moc headers
  foreach(target IN ITEMS flakewm occultation-exoskeleton flakectl)
    if(TARGET ${target})
      add_dependencies(clang-tidy ${target})
    endif()
  endforeach()
endif()

if(lint_targets)
  add_custom_target(lint)
  add_dependencies(lint ${lint_targets})
endif()
