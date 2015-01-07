#--------------------------------------------------
# plugin support
#--------------------------------------------------
set(CCI_PLUGINS_BASE_SRCS
  ${CMAKE_CURRENT_SOURCE_DIR}/base/close.c
  ${CMAKE_CURRENT_SOURCE_DIR}/base/init.c
  ${CMAKE_CURRENT_SOURCE_DIR}/base/open.c
)

set(CCI_PLUGINS_CTP_SRCS
  ${CMAKE_CURRENT_SOURCE_DIR}/ctp/base/close.c
  ${CMAKE_CURRENT_SOURCE_DIR}/ctp/base/open.c
  ${CMAKE_CURRENT_SOURCE_DIR}/ctp/base/verify.c
)

add_library(cci_plugins_base
  ${CCI_PLUGINS_BASE_SRCS}
)

add_library(cci_plugins_framework_ctp
  ${CCI_PLUGINS_CTP_SRCS}
)

#--------------------------------------------------
# macro for plugins
#--------------------------------------------------
function(add_ctp_plugin NAME SRCS)
  add_library(${NAME} MODULE ${SRCS})
  
  set_target_properties(${NAME}
    PROPERTIES 
      OUTPUT_NAME ${NAME}
      PREFIX ""
      LINK_FLAGS "${CCI_MODULE_LINKER_FLAGS}"
  )
endfunction()

#--------------------------------------------------
# tcp plugin
#--------------------------------------------------
option(CCI_BUILD_PLUGIN_TCP ON "Build TCP plugin")
if(CCI_BUILD_PLUGIN_TCP)
  set(CCI_PLUGINS_TCP_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/ctp/tcp/ctp_tcp_module.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ctp/tcp/ctp_tcp_api.c
  )

  add_ctp_plugin(cci_ctp_tcp "${CCI_PLUGINS_TCP_SRCS}")
endif()

#--------------------------------------------------
# sock plugin
#--------------------------------------------------
option(CCI_BUILD_PLUGIN_SOCK ON "Build Sock plugin")
if(CCI_BUILD_PLUGIN_SOCK)
  set(CCI_PLUGINS_SOCK_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/ctp/sock/ctp_sock_module.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ctp/sock/ctp_sock_api.c
  )

  add_ctp_plugin(cci_ctp_sock "${CCI_PLUGINS_SOCK_SRCS}")
endif()

#--------------------------------------------------
# sm plugin
#--------------------------------------------------
option(CCI_BUILD_PLUGIN_SM ON "Build Shared Memory plugin")
if(CCI_BUILD_PLUGIN_SM)
  set(CCI_PLUGINS_SM_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/ctp/sm/ctp_sm_module.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ctp/sm/ctp_sm_api.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ctp/sm/ring.c
  )

  add_ctp_plugin(cci_ctp_sm "${CCI_PLUGINS_SM_SRCS}")
endif()