set(CCI_API_SRCS
  accept.c
  arm_os_handle.c
  connect.c
  create_endpoint.c
  destroy_endpoint.c
  disconnect.c
  finalize.c
  get_devices.c
  get_event.c
  get_opt.c
  init.c
  reject.c
  return_event.c
  rma.c
  rma_deregister.c
  rma_register.c
  send.c
  sendv.c
  set_opt.c
  strerror.c
)

add_library(cci_api STATIC
  ${CCI_API_SRCS}
)
