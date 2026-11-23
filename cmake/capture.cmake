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
# This CMake helper outputs a standard output of a command to a file.
# Here are some example usages:
#   1. Embedding shaders and SVGs to C headers.
#   2. wlcom_capture_header compiles the tool qtkey-mapper first, and stores
#      that tool's output as qtkey_map_table.h

if(NOT DEFINED OUTPUT)
  message(FATAL_ERROR "capture.cmake: OUTPUT is not set")
endif()

set(_command "")
set(_collect FALSE)
math(EXPR _last "${CMAKE_ARGC} - 1")

foreach(_i RANGE ${_last})
  if(_collect)
    list(APPEND _command "${CMAKE_ARGV${_i}}")
  elseif(CMAKE_ARGV${_i} STREQUAL "--")
    set(_collect TRUE)
  endif()
endforeach()

if(NOT _command)
  message(FATAL_ERROR "capture.cmake: no command given after --")
endif()

set(_input_args "")
if(DEFINED INPUT)
  set(_input_args INPUT_FILE "${INPUT}")
endif()

execute_process(
  COMMAND ${_command}
  ${_input_args}
  OUTPUT_FILE "${OUTPUT}.tmp"
  RESULT_VARIABLE _result
)

if(NOT _result EQUAL 0)
  file(REMOVE "${OUTPUT}.tmp")
  message(FATAL_ERROR "capture.cmake: '${_command}' failed: ${_result}")
endif()

file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")
