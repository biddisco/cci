add_executable(cci_info cci_info.c)

target_link_libraries(cci_info 
  cci
)

add_executable(pingpong pingpong.c)

target_link_libraries(pingpong
  cci
)
