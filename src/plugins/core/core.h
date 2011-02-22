/*
 * Copyright (c) 2010 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 *
 * Types for CCI.
 */

#ifndef CCI_PLUGINS_CORE_H
#define CCI_PLUGINS_CORE_H

#include "cci/config.h"
#include "cci.h"
#include "cci_lib_types.h"
#include "plugins/plugins.h"

#ifdef HAVE_STDINT_H
#include <stdint.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_SYS_TYPES_H
#include <sys/time.h>
#endif

BEGIN_C_DECLS

/*
 * Typedefs for the CCI code plugin framework, used in the
 * cci_plugin_core_t struct, below.
 */

typedef int (*cci_init_fn_t)(uint32_t abi_ver, uint32_t flags, uint32_t *caps);
typedef const char *(*cci_strerror_fn_t)(enum cci_status status);
typedef int (*cci_get_devices_fn_t)(cci_device_t const ***devices);
typedef int (*cci_free_devices_fn_t)(cci_device_t const **devices);
typedef int (*cci_create_endpoint_fn_t)(cci_device_t *device, 
                                        int flags, 
                                        cci_endpoint_t **endpoint, 
                                        cci_os_handle_t *fd);
typedef int (*cci_destroy_endpoint_fn_t)(cci_endpoint_t *endpoint);
typedef int (*cci_bind_fn_t)(cci_device_t *device, int backlog, uint32_t *port, 
                             cci_service_t **service, cci_os_handle_t *fd);
typedef int (*cci_unbind_fn_t)(cci_service_t *service, cci_device_t *device);
typedef int (*cci_get_conn_req_fn_t)(cci_service_t *service, 
                                     cci_conn_req_t **conn_req);
typedef int (*cci_accept_fn_t)(cci_conn_req_t *conn_req, 
                               cci_endpoint_t *endpoint, 
                               cci_connection_t **connection);
typedef int (*cci_reject_fn_t)(cci_conn_req_t *conn_req);
typedef int (*cci_connect_fn_t)(cci_endpoint_t *endpoint, char *server_uri, 
                                uint32_t port,
                                void *data_ptr, uint32_t data_len, 
                                cci_conn_attribute_t attribute,
                                void *context, int flags, 
                                struct timeval *timeout);
typedef int (*cci_disconnect_fn_t)(cci_connection_t *connection);
typedef int (*cci_set_opt_fn_t)(cci_opt_handle_t *handle, 
                                cci_opt_level_t level, 
                                cci_opt_name_t name, const void* val, int len);
typedef int (*cci_get_opt_fn_t)(cci_opt_handle_t *handle, 
                                cci_opt_level_t level, 
                                cci_opt_name_t name, void** val, int *len);
typedef int (*cci_arm_os_handle_fn_t)(cci_endpoint_t *endpoint, int flags);
typedef int (*cci_get_event_fn_t)(cci_endpoint_t *endpoint, 
                                  cci_event_t ** const event,
                                  uint32_t flags);
typedef int (*cci_return_event_fn_t)(cci_endpoint_t *endpoint, 
                                     cci_event_t *event);
typedef int (*cci_send_fn_t)(cci_connection_t *connection, 
                             void *header_ptr, uint32_t header_len, 
                             void *data_ptr, uint32_t data_len, 
                             void *context, int flags);
typedef int (*cci_sendv_fn_t)(cci_connection_t *connection, 
                              void *header_ptr, uint32_t header_len, 
                              char **data_ptrs, int *data_lens,
                              uint segment_cnt, void *context, int flags);
typedef int (*cci_rma_register_fn_t)(cci_endpoint_t *endpoint, void *start, 
                                     uint64_t length, uint64_t *rma_handle);
typedef int (*cci_rma_register_phys_fn_t)(cci_endpoint_t *endpoint, 
                                          cci_sg_t *sg_list, uint32_t sg_cnt, 
                                          uint64_t *rma_handle);
typedef int (*cci_rma_deregister_fn_t)(uint64_t rma_handle);
typedef int (*cci_rma_fn_t)(cci_connection_t *connection, 
                            void *header_ptr, uint32_t header_len, 
                            uint64_t local_handle, uint64_t local_offset, 
                            uint64_t remote_handle, uint64_t remote_offset,
                            uint64_t data_len, void *context, int flags);

/* Plugin struct */

typedef struct {
    cci_plugin_t base;

    /* CCI API function pointers */
    cci_init_fn_t init;
    cci_strerror_fn_t strerror;
    cci_get_devices_fn_t get_devices;
    cci_free_devices_fn_t free_devices;
    cci_create_endpoint_fn_t create_endpoint;
    cci_destroy_endpoint_fn_t destroy_endpoint;
    cci_bind_fn_t bind;
    cci_unbind_fn_t unbind;
    cci_get_conn_req_fn_t get_conn_req;
    cci_accept_fn_t accept;
    cci_reject_fn_t reject;
    cci_connect_fn_t connect;
    cci_disconnect_fn_t disconnect;
    cci_set_opt_fn_t set_opt;
    cci_get_opt_fn_t get_opt;
    cci_arm_os_handle_fn_t arm_os_handle;
    cci_get_event_fn_t get_event;
    cci_return_event_fn_t return_event;
    cci_send_fn_t send;
    cci_sendv_fn_t sendv;
    cci_rma_register_fn_t rma_register;
    cci_rma_register_phys_fn_t rma_register_phys;
    cci_rma_deregister_fn_t rma_deregister;
    cci_rma_fn_t rma;
} cci_plugin_core_t;

/* Global variable with the pointers to all the CCI plugin
   functions */
extern cci_plugin_core_t *cci_core;

/* Define for the version of this plugin type header file */
#define CCI_CORE_API_VERSION_MAJOR 1
#define CCI_CORE_API_VERSION_MINOR 0
#define CCI_CORE_API_VERSION_RELEASE 0
#define CCI_CORE_API_VERSION \
    "core", \
    CCI_CORE_API_VERSION_MAJOR, \
    CCI_CORE_API_VERSION_MINOR, \
    CCI_CORE_API_VERSION_RELEASE

END_C_DECLS

#endif /* CCI_PLUGINS_CORE_H */