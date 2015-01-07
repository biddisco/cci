#--------------------------------------------------
# quick first cut at libs  
#--------------------------------------------------
add_subdirectory(api)
add_subdirectory(util)
add_subdirectory(plugins)

#--------------------------------------------------
# cmake gives a warning if we build a lib with no source files
# since this is built from other libs, just add a dummy file to shut cmake up
#--------------------------------------------------
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/dummy.c " ")
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/dummy.c PROPERTIES GENERATED TRUE)

#--------------------------------------------------
# main cci lib
#--------------------------------------------------
add_library(cci SHARED ${CMAKE_CURRENT_BINARY_DIR}/dummy.c)
target_link_libraries(cci
  cci_api
  cci_util
  cci_plugins_base
  cci_plugins_framework_ctp
  ${LTDL_LIBRARIES}
)

#--------------------------------------------------
# Testing
#--------------------------------------------------
add_subdirectory(tests)