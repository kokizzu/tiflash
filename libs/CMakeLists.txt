# Copyright 2023 PingCAP, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

if (NOT NO_WERROR)
    set (CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
    set (CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
endif ()

# Optimized routines under arm
add_subdirectory (libarm-optimized-routines)
add_subdirectory (libcommon)
add_subdirectory (libpocoext)
add_subdirectory (libdaemon)
# Optimized memcpy under x86_64
add_subdirectory (libmemcpy)

if (GLIBC_COMPATIBILITY)
    add_subdirectory (libglibc-compatibility)
endif ()

add_subdirectory (libsymbolization)
add_subdirectory (libprocess_metrics)

if (USE_INTERNAL_LIBCLARA)
    add_subdirectory (libclara-cmake)
endif ()
