# ******************************************************************************
# Copyright 2018-2020 Intel Corporation
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
# ******************************************************************************

include(ExternalProject)

Zlib
set(ZLIB_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/ext_zlib)
set(ZLIB_SRC_DIR ${ZLIB_PREFIX}/src)
set(ZLIB_LIB_DIR ${ZLIB_SRC_DIR}/ext_zlib-build)
set(ZLIB_REPO_URL https://github.com/madler/zlib.git)
set(ZLIB_GIT_TAG v1.2.11)

ExternalProject_Add(ext_zlib
    GIT_REPOSITORY    ${ZLIB_REPO_URL}
    GIT_TAG           ${ZLIB_GIT_TAG}
    PREFIX            ${ZLIB_PREFIX}
    INSTALL_COMMAND   ""
    UPDATE_COMMAND    ""
)

add_library(zlib SHARED IMPORTED)
set_target_properties(zlib
                        PROPERTIES IMPORTED_LOCATION ${ZLIB_LIB_DIR}/libz.so)
add_dependencies(zlib ext_zlib)

install(DIRECTORY ${ZLIB_LIB_DIR}/
        DESTINATION ${EXTERNAL_INSTALL_LIB_DIR}
        FILES_MATCHING
        PATTERN "*.so"
        PATTERN "*.so*"
        PATTERN "*.a"
        )

# SEAL
set(SEAL_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/ext_seal)
set(SEAL_SRC_DIR ${SEAL_PREFIX}/src/ext_seal) #Should change the dir
set(SEAL_REPO_URL https://github.com/Microsoft/SEAL.git)
set(SEAL_GIT_TAG 3.6.5)
if (NGRAPH_HE_ABY_ENABLE)
  set(SEAL_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/seal.aby_patch)
else()
  set(SEAL_PATCH ${CMAKE_CURRENT_SOURCE_DIR}/cmake/seal.patch)
endif()

# Without these, SEAL's globals.cpp will be deallocated twice, once by
# he_seal_backend, which loads libseal.a, and once by the global destructor.
set(SEAL_CXX_FLAGS
    "${CMAKE_CXX_FLAGS} -fvisibility=hidden -fvisibility-inlines-hidden")
if("${CMAKE_CXX_COMPILER_ID}" MATCHES "^(Apple)?Clang$")
  add_compile_options(-Wno-undef)
  add_compile_options(-Wno-newline-eof)
  add_compile_options(-Wno-reserved-id-macro)
  add_compile_options(-Wno-documentation)
  add_compile_options(-Wno-documentation-unknown-command)
  add_compile_options(-Wno-inconsistent-missing-destructor-override)
  add_compile_options(-Wno-extra-semi)
  add_compile_options(-Wno-old-style-cast)
  add_compile_options(-fnew-alignment=64)
endif()


ExternalProject_Add(
  ext_seal
  GIT_REPOSITORY ${SEAL_REPO_URL}
  GIT_TAG ${SEAL_GIT_TAG}
  PREFIX ${SEAL_PREFIX}
  INSTALL_DIR ${EXTERNAL_INSTALL_DIR}
  CMAKE_ARGS {NGRAPH_HE_FORWARD_CMAKE_ARGS}
  CONFIGURE_COMMAND cmake
                    ${SEAL_SRC_DIR}
                    -DCMAKE_INSTALL_PREFIX=${EXTERNAL_INSTALL_DIR}
                    -DCMAKE_CXX_FLAGS=${SEAL_CXX_FLAGS}
                    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
                    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
                    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
                    -DSEAL_USE_CXX17=ON
                    -DCMAKE_INSTALL_LIBDIR=${EXTERNAL_INSTALL_LIB_DIR}
                    -DCMAKE_INSTALL_INCLUDEDIR=${EXTERNAL_INSTALL_INCLUDE_DIR}
                    -DSEAL_USE_INTEL_HEXL=ON
  PATCH_COMMAND git apply ${SEAL_PATCH}
  # Skip updates
  UPDATE_COMMAND ""
)

# ExternalProject_Get_Property(ext_seal SOURCE_DIR)
add_library(libseal_only STATIC IMPORTED)

set(SEAL_HEADERS_PATH ${EXTERNAL_INSTALL_INCLUDE_DIR}/SEAL-3.6)

target_include_directories(libseal_only SYSTEM
                           INTERFACE ${EXTERNAL_INSTALL_INCLUDE_DIR}/SEAL-3.6)
set_target_properties(libseal_only
                      PROPERTIES IMPORTED_LOCATION
                                 ${EXTERNAL_INSTALL_LIB_DIR}/libseal-3.6.a)
add_dependencies(libseal_only ext_seal)

# Link to this library to also link with zlib, which SEAL uses
add_library(libseal INTERFACE)
target_link_libraries(libseal INTERFACE zlib libseal_only)
