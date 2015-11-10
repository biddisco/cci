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
  ${CCI_LIBS}
)
SET_TARGET_PROPERTIES(cci PROPERTIES LINK_INTERFACE_LIBRARIES "")

install(TARGETS cci
  EXPORT CCITargets
  LIBRARY DESTINATION lib
  ARCHIVE DESTINATION lib
  RUNTIME DESTINATION bin
  PUBLIC_HEADER DESTINATION include)


#--------------------------------------------------
# Testing
#--------------------------------------------------
add_subdirectory(tests)