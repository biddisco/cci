set(ltdl_SRCS
  ${CMAKE_CURRENT_SOURCE_DIR}/libltdl/argz.c      
  ${CMAKE_CURRENT_SOURCE_DIR}/libltdl/lt__dirent.c
  ${CMAKE_CURRENT_SOURCE_DIR}/libltdl/lt_dlloader.c
  ${CMAKE_CURRENT_SOURCE_DIR}/libltdl/ltdl.c
  ${CMAKE_CURRENT_SOURCE_DIR}/libltdl/lt__alloc.c 
  ${CMAKE_CURRENT_SOURCE_DIR}/libltdl/lt__strl.c  
  ${CMAKE_CURRENT_SOURCE_DIR}/libltdl/lt_error.c  
  ${CMAKE_CURRENT_SOURCE_DIR}/libltdl/slist.c
)

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/libtldl)

add_definitions(-DLTDL)

add_definitions(-DLT_CONFIG_H=\"${CMAKE_CURRENT_SOURCE_DIR}/libltdl/config.h\")

add_library(ltdl ${ltdl_SRCS})

