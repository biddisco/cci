set(CCI_UTIL_SRCS
  ${CMAKE_CURRENT_SOURCE_DIR}/argv.c
)

add_library(cci_util 
  ${CCI_UTIL_SRCS}
)