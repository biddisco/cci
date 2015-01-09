add_executable(cci_info cci_info.c)

target_link_libraries(cci_info 
  ${CCI_LIBS}
)

add_executable(pingpong pingpong.c)

target_link_libraries(pingpong
  ${CCI_LIBS}
)
