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

find_package(LTDL REQUIRED)

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
set(CCI_PKGLIBDIR \"${PROJECT_BINARY_DIR}/bin\")
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
  # using GCC
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
  ${LTDL_INCLUDE_DIR}
  ${CMAKE_CURRENT_BINARY_DIR}
)

add_definitions(-DHAVE_CONFIG_H)

set(CCI_LIBS
#target_link_libraries(cci
  cci_api
  cci_util
  cci_plugins_base
  cci_plugins_framework_ctp
  ${LTDL_LIBRARIES}
)

#--------------------------------------------------
# build libs and tests
#--------------------------------------------------
add_subdirectory(src)