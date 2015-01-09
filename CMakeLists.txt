project(CCI)

cmake_minimum_required(VERSION 2.8)

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
  include(${LTDL_INCLUDE_DIR})
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
