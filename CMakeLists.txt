project(CCI)

cmake_minimum_required(VERSION 2.8)

set(CCI_VERSION 0.1.6)

#--------------------------------------------------
# cmake policies
# see http://www.cmake.org/cmake/help/v2.8.12/cmake.html#section_Policies
#--------------------------------------------------
foreach(p
    CMP0003
    CMP0017
    CMP0023
    CMP0042
    )
  if(POLICY ${p})
    cmake_policy(SET ${p} NEW)
  endif()
endforeach()

#--------------------------------------------------
# CMake path for find_package modules
#--------------------------------------------------
set(CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/CMake ${CMAKE_MODUE_PATH})

find_package(LTDL)
if(LTDL_FOUND)
  include_directories(${LTDL_INCLUDE_DIR})
else()
  message(WARNING "WARNING : libtldl ws not found and must be installed. If the library is present,"
  "set the path in LTDL_LIBRARY variable and the headers will be provided by CCI,"
  "if compilation fails, then install libtldl."
  "@TODO, provide libtldl source with cmake build\n")
  set(LTDL_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/libltdl)
endif()

#--------------------------------------------------
# Setup Output directories
#--------------------------------------------------
SET (CMAKE_RUNTIME_OUTPUT_DIRECTORY
${PROJECT_BINARY_DIR}/bin CACHE PATH "Single Directory for all Executables."
)
SET (CMAKE_LIBRARY_OUTPUT_DIRECTORY
${PROJECT_BINARY_DIR}/bin CACHE PATH "Single Directory for all Libraries"
)
SET (CMAKE_ARCHIVE_OUTPUT_DIRECTORY
${PROJECT_BINARY_DIR}/bin CACHE PATH "Single Directory for all static libraries."
)

#--------------------------------------------------
# for shared libraries
#--------------------------------------------------
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

#--------------------------------------------------
# @TODO fix this to generate info properly
#--------------------------------------------------
set(CCI_PKGLIBDIR \"${CMAKE_LIBRARY_OUTPUT_DIRECTORY}\")
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/CMake/configure.h.in 
  ${CMAKE_CURRENT_BINARY_DIR}/cci/configure_output.h
  @ONLY
)

#--------------------------------------------------
# For plugins we need to link without symbol resolution
#--------------------------------------------------
if ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Clang")
  set(CCI_MODULE_LINKER_FLAGS "-undefined dynamic_lookup")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "GNU")
  set(CCI_MODULE_LINKER_FLAGS "")
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Intel")
  # using Intel C++
elseif ("${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
  # using Visual Studio C++
endif()

#--------------------------------------------------
# include paths
#--------------------------------------------------
include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_SOURCE_DIR}/src/api
  ${CMAKE_CURRENT_BINARY_DIR}
  ${LTDL_INCLUDE_DIR}
)

add_definitions(-DHAVE_CONFIG_H)

set(CCI_LIBS
  cci_api
  cci_util
  cci_plugins_base
  cci_plugins_framework_ctp
  ${LTDL_LIBRARY}
  pthread
)

#--------------------------------------------------
# build libs and tests
#--------------------------------------------------
add_subdirectory(src)

#--------------------------------------------------
# INSTALL : setup files for projects that find_package(CCI)
# CCITargets.cmake (any targets we export that another project needs)
# CCIConfigVersion.cmake (version info)
# CCIConfig.cmake (all the other variables another project needs)
#
# Supports three methods of using CCI
# find_package(CCI) - pointing to the INSTALL location
# find_package(CCI) - pointing to the BUILD directory
# add_subdirectory(CCI) - where the project using
# CCI can simply set CCI_DIR to the build subdirectoy
# The main difference is that the build dir versions need the source tree
# include locations instead of the install tree.
#--------------------------------------------------
set(ConfigPackageLocation share/cmake/CCI)

install(DIRECTORY include DESTINATION include)
#install(FILES "${PROJECT_BINARY_DIR}/include/flatbuffers/options.h"
#  DESTINATION include/flatbuffers)

install(EXPORT CCITargets
  FILE CCITargets.cmake DESTINATION .)

install(EXPORT CCITargets
  FILE CCITargets.cmake DESTINATION ${ConfigPackageLocation})

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/CCIConfigVersion.cmake"
  VERSION ${CCI_VERSION} COMPATIBILITY AnyNewerVersion)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CCIConfigVersion.cmake"
  DESTINATION ${ConfigPackageLocation} COMPONENT dev)

# Setup include files for the BUILD dir and source location
set(CCI_INCLUDE_DIRS
  "${PROJECT_BINARY_DIR}/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/include")

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/CMake/CCIConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/CCIConfig.cmake" @ONLY)

# Setup include files for the INSTALL dir
set(CCI_INCLUDE_DIRS "${CMAKE_INSTALL_PREFIX}/include")
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/CMake/CCIConfig.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/CCIConfig.cmake" @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/CCIConfig.cmake"
  DESTINATION ${ConfigPackageLocation} COMPONENT dev)
