/*
 * Copyright (c) 2010 Cisco Systems, Inc.  All rights reserved.
 * $COPYRIGHT$
 */

#include "cci/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>
#include <ifaddrs.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>

#include "cci.h"
#include "plugins/core/core.h"
#include "core_verbs.h"

volatile int verbs_shut_down = 0;
volatile verbs_globals_t *vglobals = NULL;
pthread_t progress_tid;

/*
 * Local functions
 */
static int verbs_init(uint32_t abi_ver, uint32_t flags, uint32_t *caps);
static const char *verbs_strerror(enum cci_status status);
static int verbs_get_devices(cci_device_t const ***devices);
static int verbs_free_devices(cci_device_t const **devices);
static int verbs_create_endpoint(cci_device_t *device,
                                    int flags,
                                    cci_endpoint_t **endpoint,
                                    cci_os_handle_t *fd);
static int verbs_destroy_endpoint(cci_endpoint_t *endpoint);
static int verbs_accept(union cci_event *event,
                           cci_connection_t **connection);
static int verbs_reject(union cci_event *event);
static int verbs_connect(cci_endpoint_t *endpoint, char *server_uri,
                            void *data_ptr, uint32_t data_len,
                            cci_conn_attribute_t attribute,
                            void *context, int flags,
                            struct timeval *timeout);
static int verbs_disconnect(cci_connection_t *connection);
static int verbs_set_opt(cci_opt_handle_t *handle,
                            cci_opt_level_t level,
                            cci_opt_name_t name, const void* val, int len);
static int verbs_get_opt(cci_opt_handle_t *handle,
                            cci_opt_level_t level,
                            cci_opt_name_t name, void** val, int *len);
static int verbs_arm_os_handle(cci_endpoint_t *endpoint, int flags);
static int verbs_get_event(cci_endpoint_t *endpoint,
                              cci_event_t ** const event);
static int verbs_return_event(cci_event_t *event);
static int verbs_send(cci_connection_t *connection,
                         void *msg_ptr, uint32_t msg_len,
                         void *context, int flags);
static int verbs_sendv(cci_connection_t *connection,
                          struct iovec *data, uint32_t iovcnt,
                          void *context, int flags);
static int verbs_rma_register(cci_endpoint_t *endpoint,
                                 cci_connection_t *connection,
                                 void *start, uint64_t length,
                                 uint64_t *rma_handle);
static int verbs_rma_deregister(uint64_t rma_handle);
static int verbs_rma(cci_connection_t *connection,
                        void *msg_ptr, uint32_t msg_len,
                        uint64_t local_handle, uint64_t local_offset,
                        uint64_t remote_handle, uint64_t remote_offset,
                        uint64_t data_len, void *context, int flags);


/*
 * Public plugin structure.
 *
 * The name of this structure must be of the following form:
 *
 *    cci_core_<your_plugin_name>_plugin
 *
 * This allows the symbol to be found after the plugin is dynamically
 * opened.
 *
 * Note that your_plugin_name should match the direct name where the
 * plugin resides.
 */
cci_plugin_core_t cci_core_verbs_plugin = {
    {
        /* Logistics */
        CCI_ABI_VERSION,
        CCI_CORE_API_VERSION,
        "verbs",
        CCI_MAJOR_VERSION, CCI_MINOR_VERSION, CCI_RELEASE_VERSION,
        10,

        /* Bootstrap function pointers */
        cci_core_verbs_post_load,
        cci_core_verbs_pre_unload,
    },

    /* API function pointers */
    verbs_init,
    verbs_strerror,
    verbs_get_devices,
    verbs_free_devices,
    verbs_create_endpoint,
    verbs_destroy_endpoint,
    verbs_accept,
    verbs_reject,
    verbs_connect,
    verbs_disconnect,
    verbs_set_opt,
    verbs_get_opt,
    verbs_arm_os_handle,
    verbs_get_event,
    verbs_return_event,
    verbs_send,
    verbs_sendv,
    verbs_rma_register,
    verbs_rma_deregister,
    verbs_rma
};

static uint32_t
verbs_mtu_val(enum ibv_mtu mtu)
{
	switch (mtu) {
		/* most common first */
		case IBV_MTU_2048:
			return 2048;
		case IBV_MTU_256:
			return 256;
		case IBV_MTU_512:
			return 512;
		case IBV_MTU_1024:
			return 1024;
		case IBV_MTU_4096:
			return 4096;
		default:
			/* invalid speed */
			return 0;
	}
}

static uint64_t
verbs_device_rate(struct ibv_port_attr attr)
{
	uint64_t rate = 2500000000ULL; /* 2.5 Gbps */

	rate *= attr.active_speed;

	switch (attr.active_width) {
		case 1:
			break;
		case 2:
			rate *= 4;
			break;
		case 4:
			rate *= 8;
			break;
		case 8:
			rate *= 12;
			break;
		default:
			rate = 0;
	}
	return rate;
}

static int
verbs_ifa_to_context(struct ibv_context *context, struct sockaddr *sa)
{
	int			ret	= CCI_SUCCESS;
	struct rdma_cm_id	*id;

	CCI_ENTER;

	ret = rdma_create_id(NULL, &id, NULL, RDMA_PS_UDP);
	if (ret) {
		ret = errno;
		CCI_EXIT;
		goto out;
	}

	ret = rdma_bind_addr(id, sa);
	if (ret == 0) {
		if (id->verbs != context)
			ret = -1;
		rdma_destroy_id(id);
	}

out:
	CCI_EXIT;
	return ret;
}

static int
verbs_find_rdma_devices(struct ibv_context **contexts, int count, struct ifaddrs **ifaddrs)
{
	int		ret		= CCI_SUCCESS;
	int		i		= 0;
	struct ifaddrs	*addrs		= NULL;
	struct ifaddrs	*ifa		= NULL;
	struct ifaddrs	*tmp		= NULL;

	CCI_ENTER;

	addrs = calloc(count + 1, sizeof(*addrs));
	if (!addrs) {
		ret = CCI_ENOMEM;
		goto out;
	}

	ret = getifaddrs(&ifa);
	if (ret) {
		ret = errno;
		goto out;
	}

	for (i = 0; i < count; i++) {
		struct ibv_context	*c	= contexts[i];

		for (tmp = ifa; tmp != NULL; tmp = tmp->ifa_next) {
			if (tmp->ifa_addr->sa_family == AF_INET &&
				!(tmp->ifa_flags & IFF_LOOPBACK)) {
				ret = verbs_ifa_to_context(c, tmp->ifa_addr);
				if (!ret) {
					addrs[i].ifa_name = strdup(tmp->ifa_name);
					addrs[i].ifa_flags = tmp->ifa_flags;
					addrs[i].ifa_addr = tmp->ifa_addr;
					addrs[i].ifa_netmask = tmp->ifa_netmask;
					addrs[i].ifa_broadaddr = tmp->ifa_broadaddr;
					i++;
					break;
				}
			}
		}
	}

	freeifaddrs(ifa);
	*ifaddrs = addrs;
out:
	CCI_EXIT;
	return ret;
}

static verbs_tx_t *
verbs_get_tx_locked(verbs_ep_t *vep)
{
	verbs_tx_t	*tx	= NULL;

	if (!TAILQ_EMPTY(&vep->idle_txs)) {
		tx = TAILQ_FIRST(&vep->idle_txs);
		TAILQ_REMOVE(&vep->idle_txs, tx, entry);
	}
	return tx;
}

static verbs_tx_t *
verbs_get_tx(cci__ep_t *ep)
{
	verbs_ep_t	*vep	= ep->priv;
	verbs_tx_t	*tx	= NULL;

	pthread_mutex_lock(&ep->lock);
	tx = verbs_get_tx_locked(vep);
	pthread_mutex_unlock(&ep->lock);

	return tx;
}

static int
verbs_init(uint32_t abi_ver, uint32_t flags, uint32_t *caps)
{
	int		count		= 0;
	int		index		= 0;
	int		used[CCI_MAX_DEVICES];
	int		ret		= 0;
	cci__dev_t	*dev		= NULL;
	cci_device_t	**devices	= NULL;
	struct ifaddrs	*ifaddrs	= NULL;

	CCI_ENTER;

	memset(used, 0, CCI_MAX_DEVICES);

	/* init driver globals */
	vglobals = calloc(1, sizeof(*vglobals));
	if (!vglobals) {
		ret = CCI_ENOMEM;
		goto out;
	}

	devices = calloc(CCI_MAX_DEVICES, sizeof(*vglobals->devices));
	if (!devices) {
		ret = CCI_ENOMEM;
		goto out;
	}

	vglobals->contexts = rdma_get_devices(&count);
	if (!vglobals->contexts) {
		ret = -errno;
		goto out;
	}
	vglobals->count = count;

	/* for each ifaddr, check if it is a RDMA device */
	ret = verbs_find_rdma_devices(vglobals->contexts, count, &ifaddrs);
	if (ret) {
		/* TODO */
		ret = CCI_ENODEV;
		goto out;
	}
	vglobals->ifaddrs = ifaddrs;

	/* find devices we own */
	TAILQ_FOREACH(dev, &globals->devs, entry) {
	if (0 == strcmp("verbs", dev->driver)) {
		int			i		= 0;
		const char **arg;
		const char		*hca_id		= NULL;
		const char		*interface	= NULL;
		struct in_addr		in;
		uint16_t		port		= 0;
		uint32_t		mss		= 0;
		cci_device_t		*device		= NULL;
		verbs_dev_t		*vdev		= NULL;
		struct ibv_port_attr	port_attr;

		in.s_addr = INADDR_ANY;

		device = &dev->device;
		device->pci.domain = -1;	/* per CCI spec */
		device->pci.bus = -1;		/* per CCI spec */
		device->pci.dev = -1;		/* per CCI spec */
		device->pci.func = -1;		/* per CCI spec */

		dev->priv = calloc(1, sizeof(*vdev));
		if (!dev->priv) {
			ret = CCI_ENOMEM;
			goto out;
		}

		vdev = dev->priv;

		/* parse conf_argv */
		for (arg = device->conf_argv; *arg != NULL; arg++) {
			if (0 == strncmp("ip=", *arg, 3)) {
				const char *ip = *arg + 3;

				ret = inet_aton(ip, &in);
				if (!ret)
					debug(CCI_DB_INFO, "unable to parse %s", ip);
			} else if (0 == strncmp("port=", *arg, 5)) {
				const char *port_str = *arg + 5;

				port = (uint16_t) strtoul(port_str, NULL, 0);
			} else if (0 == strncmp("mss=", *arg, 4)) {
				const char *mss_str = *arg + 4;

				mss = strtoul(mss_str, NULL, 0);
				if (mss > IBV_MTU_4096) {
					debug(CCI_DB_INFO, "mss %s is larger than "
							"IBV_MTU_4096", mss_str);
					mss = IBV_MTU_4096;
				}
			} else if (0 == strncmp("hca_id=", *arg, 7)) {
				hca_id = *arg + 7;
			} else if (0 == strncmp("interface=", *arg, 10)) {
				interface = *arg + 10;
			} else if (0 == strncmp("driver=", *arg, 7)) {
				/* do nothing */
			} else {
				debug(CCI_DB_INFO, "unknown keyword %s", *arg);
			}
		}

		for (i = 0; i < count; i++) {
			struct ifaddrs		*ifa = &ifaddrs[i];
			struct sockaddr_in	*sin =
				(struct sockaddr_in *) ifa->ifa_addr;
			struct ibv_context	*ctx = vglobals->contexts[i];

			if (in.s_addr != INADDR_ANY) {
				if (sin->sin_addr.s_addr == in.s_addr) {
					if (used[i]) {
						debug(CCI_DB_WARN, "device already assigned "
							"%s %s %s", ctx->device->name,
							ifa->ifa_name,
							inet_ntoa(sin->sin_addr));
							goto out;
					}
					vdev->context = ctx;
					vdev->ifa = ifa;
					used[i]++;
					break;
				}
			} else if (interface) {
				if (0 == strcmp(interface, ifa->ifa_name)) {
					if (used[i]) {
						debug(CCI_DB_WARN, "device already assigned "
							"%s %s %s", ctx->device->name,
							ifa->ifa_name,
							inet_ntoa(sin->sin_addr));
							goto out;
					}
					vdev->context = ctx;
					vdev->ifa = ifa;
					used[i]++;
					break;
				}
			} else if (hca_id) {
				if (0 == strcmp(hca_id, ctx->device->name)) {
					if (used[i]) {
						debug(CCI_DB_WARN, "device already assigned "
							"%s %s %s", ctx->device->name,
							ifa->ifa_name,
							inet_ntoa(sin->sin_addr));
							goto out;
					}
					vdev->context = ctx;
					vdev->ifa = ifa;
					used[i]++;
					break;
				}
			} else {
				if (used[i]) {
					debug(CCI_DB_WARN, "device already assigned "
						"%s %s %s", ctx->device->name,
						ifa->ifa_name,
						inet_ntoa(sin->sin_addr));
						goto out;
				}
				vdev->context = ctx;
				vdev->ifa = ifa;
				used[i]++;
				break;
			}
		}

		if (!vdev->context)
			goto out;

		if (port) {
			struct sockaddr_in *sin = (struct sockaddr_in *)vdev->ifa->ifa_addr;
			sin->sin_port = htons(port);
		}

		ret = ibv_query_port(vdev->context, 1, &port_attr);
		if (ret) {
			ret = errno;
			goto out;
		}

		device->max_send_size = verbs_mtu_val(port_attr.max_mtu);
		device->rate = verbs_device_rate(port_attr);

		devices[index] = device;
		index++;
		dev->is_up = vdev->ifa->ifa_flags & IFF_UP;
	}
	}

	devices = realloc(devices, (vglobals->count + 1) * sizeof(cci_device_t *));
	devices[vglobals->count] = NULL;

	*((cci_device_t ***) &vglobals->devices) = devices;

	/* TODO  start progress thread */

	CCI_EXIT;
	return CCI_SUCCESS;

out:
	if (devices) {
		cci_device_t const *device;
		cci__dev_t *my_dev;

		for (device = devices[0]; device != NULL; device++) {
			my_dev = container_of(device, cci__dev_t, device);
			if (my_dev->priv)
				free(my_dev->priv);
		}
		free(devices);
	}
	if (vglobals) {
		if (vglobals->contexts)
			rdma_free_devices(vglobals->contexts);
		if (vglobals->ifaddrs)
			freeifaddrs(vglobals->ifaddrs);
		free((void *)vglobals);
		vglobals = NULL;
	}

	CCI_EXIT;
	return ret;
}


static const char *
verbs_strerror(enum cci_status status)
{
	CCI_ENTER;
	CCI_EXIT;
	return NULL;
}


static int
verbs_get_devices(cci_device_t const ***devices)
{
	CCI_ENTER;

	if (!vglobals) {
		CCI_EXIT;
		return CCI_ENODEV;
	}

	*devices = vglobals->devices;

	CCI_EXIT;
	return CCI_SUCCESS;
}


static int
verbs_free_devices(cci_device_t const **devices)
{
	cci__dev_t	*dev	= NULL;

	CCI_ENTER;

	if (!vglobals) {
		CCI_EXIT;
		return CCI_ENODEV;
	}

	pthread_mutex_lock(&globals->lock);
	verbs_shut_down = 1;
	pthread_mutex_unlock(&globals->lock);
	/* TODO join progress thread */

	pthread_mutex_lock(&globals->lock);
	TAILQ_FOREACH(dev, &globals->devs, entry)
		if (dev->priv)
			free(dev->priv);
	pthread_mutex_unlock(&globals->lock);

	free(vglobals->devices);
	free((void *)vglobals);

	CCI_EXIT;
	return CCI_SUCCESS;
}

static int
verbs_post_rx(cci__ep_t *ep, verbs_rx_t *rx)
{
	int			ret	= CCI_SUCCESS;
	verbs_ep_t		*vep	= ep->priv;
	struct ibv_sge		list;
	struct ibv_recv_wr	wr, *bad_wr;

	CCI_ENTER;

	memset(&list, 0, sizeof(list));
	list.addr = (uintptr_t) vep->rx_buf + rx->offset;
	list.length = ep->buffer_len;
	list.lkey = vep->rx_mr->lkey;

	memset(&wr, 0, sizeof(wr));
	wr.wr_id = (uintptr_t) rx;
	wr.sg_list = &list;
	wr.num_sge = 1;

	ret = ibv_post_srq_recv(vep->srq, &wr, &bad_wr);
	if (ret == -1)
		ret = errno;
	CCI_EXIT;
	return ret;
}

static int
verbs_create_endpoint(cci_device_t *device,
			int flags,
			cci_endpoint_t **endpoint,
			cci_os_handle_t *fd)
{
	int		i	= 0;
	int		ret	= CCI_SUCCESS;
	int		fflags	= 0;
	int		pg_sz	= 0;
	char		name[MAXHOSTNAMELEN + 16]; /* verbs:// + host + port */
	size_t		len	= 0;
	cci__dev_t	*dev	= NULL;
	cci__ep_t	*ep	= NULL;
	verbs_ep_t	*vep	= NULL;
	verbs_dev_t	*vdev	= NULL;
	struct ibv_srq_init_attr srq_attr;

	CCI_ENTER;

	if (!vglobals) {
		CCI_EXIT;
		return CCI_ENODEV;
	}

	dev = container_of(device, cci__dev_t, device);
	vdev = dev->priv;

	ep = container_of(*endpoint, cci__ep_t, endpoint);
	ep->priv = calloc(1, sizeof(*vep));
	if (!ep->priv) {
		ret = CCI_ENOMEM;
		goto out;
	}
	vep = ep->priv;

	TAILQ_INIT(&vep->txs);
	TAILQ_INIT(&vep->idle_txs);
	TAILQ_INIT(&vep->rxs);
	TAILQ_INIT(&vep->conns);
	TAILQ_INIT(&vep->active);
	TAILQ_INIT(&vep->passive);
	TAILQ_INIT(&vep->handles);
	TAILQ_INIT(&vep->rma_ops);

	(*endpoint)->max_recv_buffer_count = VERBS_EP_RX_CNT;
	ep->rx_buf_cnt = VERBS_EP_RX_CNT;
	ep->tx_buf_cnt = VERBS_EP_TX_CNT;
	ep->buffer_len = dev->device.max_send_size;
	ep->tx_timeout = 0; /* FIXME */

	vep->channel = rdma_create_event_channel();
	if (!vep->channel) {
		ret = errno;
		goto out;
	}

	fflags = fcntl(vep->channel->fd, F_GETFL, 0);
	if (fflags == -1) {
		ret = errno;
		goto out;
	}

	ret = fcntl(vep->channel->fd, F_SETFL, fflags | O_NONBLOCK);
	if (ret == -1) {
		ret = errno;
		goto out;
	}

	ret = rdma_create_id(vep->channel, &vep->id_rc, ep, RDMA_PS_TCP);
	if (ret == -1) {
		ret = errno;
		goto out;
	}

	ret = rdma_create_id(vep->channel, &vep->id_ud, ep, RDMA_PS_UDP);
	if (ret == -1) {
		ret = errno;
		goto out;
	}

	vep->sin = *((struct sockaddr_in *) vdev->ifa->ifa_addr);

	ret = rdma_bind_addr(vep->id_rc, (struct sockaddr *) &vep->sin);
	if (ret == -1) {
		ret = errno;
		goto out;
	}
	vep->sin.sin_port = rdma_get_src_port(vep->id_rc);

	ret = rdma_listen(vep->id_rc, 1024);
	if (ret == -1) {
		ret = errno;
		goto out;
	}

	ret = rdma_bind_addr(vep->id_ud, (struct sockaddr *) &vep->sin);
	if (ret == -1) {
		ret = errno;
		goto out;
	}

	memset(name, 0, sizeof(name));
	sprintf(name, "%s%s:%hu", VERBS_URI,
			inet_ntoa(vep->sin.sin_addr), ntohs(vep->sin.sin_port));
	*((char **)&ep->endpoint.name) = strdup(name);

	vep->pd = ibv_alloc_pd(vdev->context);
	if (!vep->pd) {
		ret = errno;
		goto out;
	}

	vep->cq = ibv_create_cq(vdev->context, VERBS_EP_CQ_CNT, ep, NULL, 0);
	if (!vep->cq) {
		ret = errno;
		goto out;
	}

	pg_sz = getpagesize();

	len = VERBS_EP_TX_CNT * dev->device.max_send_size;
	ret = posix_memalign((void **) &vep->tx_buf, pg_sz, len);
	if (ret)
		goto out;

	vep->tx_mr = ibv_reg_mr(vep->pd, vep->tx_buf, len, IBV_ACCESS_LOCAL_WRITE);
	if (!vep->tx_mr) {
		ret = errno;
		goto out;
	}

	for (i = 0; i < VERBS_EP_TX_CNT; i++) {
		uintptr_t	offset	= i * ep->buffer_len;
		verbs_tx_t	*tx	= NULL;

		tx = calloc(1, sizeof(*tx));
		if (!tx) {
			ret = CCI_ENOMEM;
			goto out;
		}
		tx->evt.ep = ep;
		tx->buffer = vep->tx_buf + offset;
		TAILQ_INSERT_TAIL(&vep->txs, tx, gentry);
		TAILQ_INSERT_TAIL(&vep->idle_txs, tx, entry);
	}

	len = VERBS_EP_RX_CNT * dev->device.max_send_size;
	ret = posix_memalign((void **) &vep->rx_buf, pg_sz, len);
	if (ret)
		goto out;

	vep->rx_mr = ibv_reg_mr(vep->pd, vep->rx_buf, len, IBV_ACCESS_LOCAL_WRITE);
	if (!vep->rx_mr) {
		ret = errno;
		goto out;
	}

	memset(&srq_attr, 0, sizeof(srq_attr));
	srq_attr.attr.max_wr = VERBS_EP_CQ_CNT * 2;
	srq_attr.attr.max_sge = 1;
	vep->srq = ibv_create_srq(vep->pd, &srq_attr);
	if (!vep->srq) {
		ret = errno;
		goto out;
	}

	for (i = 0; i < VERBS_EP_RX_CNT; i++) {
		uintptr_t		offset = i * ep->buffer_len;
		verbs_rx_t		*rx	= NULL;

		rx = calloc(1, sizeof(*rx));
		if (!rx) {
			ret = CCI_ENOMEM;
			goto out;
		}

		rx->evt.ep = ep;
		rx->offset = offset;
		TAILQ_INSERT_TAIL(&vep->rxs, rx, entry);

		ret = verbs_post_rx(ep, rx);
		if (ret)
			goto out;
	}

	CCI_EXIT;
	return CCI_SUCCESS;

out:
	/* TODO lots of clean up */
	if (ep->priv) {
		verbs_ep_t	*vep	= ep->priv;

		if (vep->srq)
			ibv_destroy_srq(vep->srq);

		while (!TAILQ_EMPTY(&vep->rxs)) {
			verbs_rx_t *rx = TAILQ_FIRST(&vep->rxs);
			TAILQ_REMOVE(&vep->rxs, rx, entry);
			free(rx);
		}

		if (vep->rx_mr) {
			ret = ibv_dereg_mr(vep->rx_mr);
			if (ret)
				debug(CCI_DB_WARN, "deregistering new endpoint rx_mr "
					"failed with %s\n", strerror(ret));
		}

		if (vep->rx_buf)
			free(vep->rx_buf);

		while (!TAILQ_EMPTY(&vep->txs)) {
			verbs_tx_t *tx = TAILQ_FIRST(&vep->txs);
			TAILQ_REMOVE(&vep->txs, tx, entry);
			free(tx);
		}

		if (vep->tx_mr) {
			ret = ibv_dereg_mr(vep->tx_mr);
			if (ret)
				debug(CCI_DB_WARN, "deregistering new endpoint tx_mr "
					"failed with %s\n", strerror(ret));
		}

		if (vep->tx_buf)
			free(vep->tx_buf);

		if (vep->cq) {
			ret = ibv_destroy_cq(vep->cq);
			if (ret)
				debug(CCI_DB_WARN, "destroying new endpoint cq "
					"failed with %s\n", strerror(ret));
		}

		if (vep->pd) {
			ret = ibv_dealloc_pd(vep->pd);
			if (ret)
				debug(CCI_DB_WARN, "deallocing new endpoint pd "
					"failed with %s\n", strerror(ret));
		}

		if (vep->id_rc)
			rdma_destroy_id(vep->id_rc);

		if (vep->id_ud)
			rdma_destroy_id(vep->id_ud);

		if (vep->channel)
			rdma_destroy_event_channel(vep->channel);

		free(vep);
		ep->priv = NULL;
	}
	return ret;
}


static int
verbs_destroy_endpoint(cci_endpoint_t *endpoint)
{
	CCI_ENTER;
	CCI_EXIT;
	return CCI_ERR_NOT_IMPLEMENTED;
}

static const char *
verbs_msg_type_str(verbs_msg_type_t msg_type)
{
	char *str;

	switch (msg_type) {
		case VERBS_MSG_CONN_REQUEST:
			str = "conn_request";
			break;
		case VERBS_MSG_CONN_PAYLOAD:
			str = "conn_payload";
			break;
		case VERBS_MSG_CONN_REPLY:
			str = "conn_reply";
			break;
		case VERBS_MSG_DISCONNECT:
			str = "disconnect";
			break;
		case VERBS_MSG_SEND:
			str = "send";
			break;
		case VERBS_MSG_KEEPALIVE:
			str = "keepalive";
			break;
		default:
			str = "invalid";
			break;
	}
	return str;
}

static int
verbs_vconn_set_mss(verbs_conn_t *vconn)
{
	int			ret	= CCI_SUCCESS;
	struct ibv_qp_attr	attr;
	struct ibv_qp_init_attr	init;

	CCI_ENTER;

	ret = ibv_query_qp(vconn->id->qp, &attr, IBV_QP_PATH_MTU, &init);
	if (ret == -1) {
		/* FIXME do something here */
		ret = errno;
		goto out;
	}
	vconn->mss = verbs_mtu_val(attr.path_mtu);

out:
	CCI_EXIT;
	return ret;
}

static int
verbs_post_send(cci__conn_t *conn, uint64_t id, void *buffer, uint32_t len, uint32_t header)
{
	int		ret	= CCI_SUCCESS;
	cci__ep_t	*ep	= NULL;
	verbs_conn_t	*vconn	= conn->priv;
	verbs_ep_t	*vep	= NULL;
	struct ibv_sge	list;
	struct ibv_send_wr wr, *bad_wr;

	CCI_ENTER;

	ep = container_of(conn->connection.endpoint, cci__ep_t, endpoint);
	vep = ep->priv;

	if (buffer && len) {
		memset(&list, 0, sizeof(list));
		list.addr = (uintptr_t) buffer;
		list.length = len;
		list.lkey = vep->tx_mr->lkey;
	}

	memset(&wr, 0, sizeof(wr));
	wr.wr_id = (uintptr_t) id;
	if (buffer && len) {
		wr.sg_list = &list;
		wr.num_sge = 1;
	} else {
		wr.sg_list = NULL;
		wr.num_sge = 0;
	}
	if (header) {
		wr.opcode = IBV_WR_SEND_WITH_IMM;
		wr.imm_data = htonl(header);
	} else {
		wr.opcode = IBV_WR_SEND;
	}
	wr.send_flags = IBV_SEND_SIGNALED;

	ret = ibv_post_send(vconn->id->qp, &wr, &bad_wr);
	if (ret == -1) {
		ret = errno;
		debug(CCI_DB_CONN, "unable to send id 0x%"PRIx64" buffer %p len %u header %u",
			id, buffer, len, header);
	}
	CCI_EXIT;
	return ret;
}

static int
verbs_accept(union cci_event *event,
		cci_connection_t **connection)
{
	int		ret		= CCI_SUCCESS;
	cci__ep_t	*ep		= NULL;
	cci__conn_t	*conn		= NULL;
	cci__evt_t	*evt		= NULL;
	verbs_ep_t	*vep		= NULL;
	verbs_conn_t	*vconn		= NULL;
	verbs_rx_t	*rx		= NULL;
	cci_endpoint_t	*endpoint	= NULL;
	uint32_t	header		= 0;

	CCI_ENTER;

	evt = container_of(event, cci__evt_t, event);
	rx = container_of(evt, verbs_rx_t, evt);
	ep = evt->ep;
	vep = ep->priv;
	endpoint = &ep->endpoint;

	conn = evt->conn;
	vconn = conn->priv;

	ret = verbs_vconn_set_mss(vconn);
	if (ret) {
		/* TODO */
		goto out;
	}
	conn->connection.max_send_size = vconn->mss;

	header = VERBS_MSG_CONN_REPLY;
	header |= (CCI_EVENT_CONNECT_ACCEPTED << 4);

	pthread_mutex_lock(&ep->lock);
	TAILQ_INSERT_TAIL(&vep->conns, vconn, entry);
	pthread_mutex_unlock(&ep->lock);

	ret = verbs_post_send(conn, 0, NULL, 0, header);
	if (ret) {
		pthread_mutex_lock(&ep->lock);
		TAILQ_REMOVE(&vep->conns, vconn, entry);
		pthread_mutex_unlock(&ep->lock);
		goto out;
	}

	*connection = &conn->connection;

out:
	CCI_EXIT;
	return ret;
}


static int
verbs_reject(union cci_event *event)
{
	CCI_ENTER;
	CCI_EXIT;
	return CCI_ERR_NOT_IMPLEMENTED;
}


static int
verbs_parse_uri(const char *uri, char **node, char **service)
{
	int	ret	= CCI_SUCCESS;
	int	len	= strlen(VERBS_URI);
	char	*ip	= NULL;
	char	*port	= NULL;
	char	*colon	= NULL;

	CCI_ENTER;

	if (0 == strncmp(VERBS_URI, uri, len)) {
		ip = strdup(&uri[len]);
	} else {
		ret = CCI_EINVAL;
		goto out;
	}

	colon = strchr(ip, ':');
	if (colon) {
		*colon = '\0';
	} else {
		ret = CCI_EINVAL;
		goto out;
	}

	colon++;
	port = colon;

	*node = ip;
	*service = port;

out:
	if (ret != CCI_SUCCESS) {
		if (ip)
			free(ip);
	}
	CCI_EXIT;
	return ret;
}

static int
verbs_connect(cci_endpoint_t *endpoint, char *server_uri,
		void *data_ptr, uint32_t data_len,
		cci_conn_attribute_t attribute,
		void *context, int flags,
		struct timeval *timeout)
{
	int			ret		= CCI_SUCCESS;
	char			*node		= NULL;
	char			*service	= NULL;
	cci__ep_t		*ep		= NULL;
	cci__conn_t		*conn		= NULL;
	verbs_ep_t		*vep		= NULL;
	verbs_conn_t		*vconn		= NULL;
	verbs_conn_request_t	*cr		= NULL;
	struct rdma_addrinfo	hints, *res	= NULL;
	struct ibv_qp_init_attr	attr;
	struct rdma_conn_param	param;
	uint32_t		header		= 0;

	CCI_ENTER;

	ep = container_of(endpoint, cci__ep_t, endpoint);
	vep = ep->priv;

	conn = calloc(1, sizeof(*conn));
	if (!conn) {
		ret = CCI_ENOMEM;
		goto out;
	}

	conn->priv = calloc(1, sizeof(*vconn));
	if (!conn->priv) {
		ret = CCI_ENOMEM;
		goto out;
	}
	vconn = conn->priv;
	vconn->conn = conn;
	TAILQ_INIT(&vconn->remotes);

	if (context || data_len) {
		cr = calloc(1, sizeof(*cr));
		if (!cr) {
			ret = CCI_ENOMEM;
			goto out;
		}
		vconn->conn_req = cr;

		cr->context = context;
		cr->attr = attribute;
		if (data_len) {
			cr->len = data_len;
			cr->ptr = calloc(1, data_len);
			if (!cr->ptr) {
				ret = CCI_ENOMEM;
				goto out;
			}
			memcpy(cr->ptr, data_ptr, data_len);
		}
	}

	/* conn->tx_timeout = 0;  by default */

	conn->connection.attribute = attribute;
	conn->connection.endpoint = endpoint;

	ret = verbs_parse_uri(server_uri, &node, &service);
	if (ret)
		goto out;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_qp_type = IBV_QPT_RC;
	ret = rdma_getaddrinfo(node, service, &hints, &res);
	if (ret == -1) {
		ret = errno;
		debug(CCI_DB_CONN, "rdma_getaddrinfo() returned %s",
			strerror(ret));
		goto out;
	}

	memset(&attr, 0, sizeof(attr));
	attr.qp_type = IBV_QPT_RC;
	attr.send_cq = vep->cq;
	attr.recv_cq = vep->cq;
	attr.srq = vep->srq;
	attr.cap.max_send_wr = VERBS_EP_TX_CNT;
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;
	ret = rdma_create_ep(&vconn->id, res, vep->pd, &attr);
	if (ret == -1) {
		ret = errno;
		debug(CCI_DB_CONN, "rdma_create_ep() returned %s",
			strerror(ret));
		goto out;
	}

	ret = rdma_migrate_id(vconn->id, vep->channel);
	if (ret == -1) {
		ret = errno;
		debug(CCI_DB_CONN, "rdma_migrate_id() returned %s",
			strerror(ret));
		goto out;
	}
	vconn->id->context = conn;
	vconn->state = VERBS_CONN_ACTIVE;

	header = VERBS_MSG_CONN_REQUEST;
	header = htonl(header);

	pthread_mutex_lock(&ep->lock);
	TAILQ_INSERT_TAIL(&vep->active, vconn, temp);
	pthread_mutex_unlock(&ep->lock);

	memset(&param, 0, sizeof(param));
	param.srq = 1;
	param.initiator_depth = param.responder_resources = 16;
	param.rnr_retry_count = 7; /* infinite retry */
	param.private_data = &header;
	param.private_data_len = sizeof(header);
	ret = rdma_connect(vconn->id, &param);
	if (ret == -1) {
		ret = errno;
		pthread_mutex_lock(&ep->lock);
		TAILQ_REMOVE(&vep->active, vconn, temp);
		pthread_mutex_unlock(&ep->lock);
		goto out;
	}

	debug(CCI_DB_CONN, "connecting to %s %s\n",
		node, service);

out:
	/* TODO
	 * if (ret)
	 *	free memory
	 */
	if (node)
		free(node);
	CCI_EXIT;
	return ret;
}


static int
verbs_disconnect(cci_connection_t *connection)
{
	CCI_ENTER;
	CCI_EXIT;
	return CCI_ERR_NOT_IMPLEMENTED;
}


static int
verbs_set_opt(cci_opt_handle_t *handle,
		cci_opt_level_t level,
		cci_opt_name_t name, const void* val, int len)
{
	CCI_ENTER;
	CCI_EXIT;
	return CCI_ERR_NOT_IMPLEMENTED;
}


static int
verbs_get_opt(cci_opt_handle_t *handle,
		cci_opt_level_t level,
		cci_opt_name_t name, void** val, int *len)
{
	CCI_ENTER;
	CCI_EXIT;
	return CCI_ERR_NOT_IMPLEMENTED;
}


static int
verbs_arm_os_handle(cci_endpoint_t *endpoint, int flags)
{
	CCI_ENTER;
	CCI_EXIT;
	return CCI_ERR_NOT_IMPLEMENTED;
}

/* A peer is trying to connect. Always accept and let them send the full
 * connect request as a regular message.
 */
static int
verbs_handle_conn_request(cci__ep_t *ep, struct rdma_cm_event *cm_evt)
{
	int			ret	= CCI_SUCCESS;
	verbs_ep_t		*vep	= ep->priv;
	cci__conn_t		*conn	= NULL;
	verbs_conn_t		*vconn	= NULL;
	struct rdma_cm_id	*peer	= NULL;
	struct ibv_qp_init_attr	attr;
	struct rdma_conn_param	*param	= NULL;
	uint32_t		header;

	peer = cm_evt->id;
	assert(cm_evt->status == 0);

	memset(&attr, 0, sizeof(attr));
	attr.qp_type = IBV_QPT_RC;
	attr.send_cq = vep->cq;
	attr.recv_cq = vep->cq;
	attr.srq = vep->srq;
	attr.cap.max_send_wr = VERBS_EP_TX_CNT;
	attr.cap.max_send_sge = 1;
	attr.cap.max_recv_sge = 1;

	ret = rdma_create_qp(peer, vep->pd, &attr);
	if (ret == -1) {
		ret = errno;
		goto out;
	}

	param = &cm_evt->param.conn;
	param->srq = 1;
	param->qp_num = peer->qp->qp_num;

	header = ntohl(*((uint32_t *)param->private_data));
	assert((header & 0xF) == VERBS_MSG_CONN_REQUEST);

	conn = calloc(1, sizeof(*conn));
	if (!conn) {
		ret = CCI_ENOMEM;
		goto out;
	}

	conn->priv = calloc(1, sizeof(*vconn));
	if (!conn->priv) {
		ret = CCI_ENOMEM;
		goto out;
	}
	vconn = conn->priv;
	vconn->conn = conn;
	vconn->id = peer;
	vconn->id->context = conn;
	vconn->state = VERBS_CONN_PASSIVE;
	TAILQ_INIT(&vconn->remotes);

	conn->connection.endpoint = &ep->endpoint;

	pthread_mutex_lock(&ep->lock);
	TAILQ_INSERT_TAIL(&vep->passive, vconn, temp);
	pthread_mutex_unlock(&ep->lock);

	ret = rdma_accept(peer, param);
	if (ret == -1) {
		ret = errno;
		pthread_mutex_lock(&ep->lock);
		TAILQ_REMOVE(&vep->passive, vconn, temp);
		pthread_mutex_unlock(&ep->lock);
		goto out;
	}

out:
	CCI_EXIT;
	return ret;
}


static const char *
verbs_conn_state_str(verbs_conn_state_t state)
{
	char *str;
	switch (state) {
		case VERBS_CONN_CLOSED:
			str = "closed";
		case VERBS_CONN_CLOSING:
			str = "closing";
		case VERBS_CONN_INIT:
			str = "init";
		case VERBS_CONN_ACTIVE:
			str = "active";
		case VERBS_CONN_PASSIVE:
			str = "passive";
		case VERBS_CONN_ESTABLISHED:
			str = "established";
	}
	return str;
}

static int
verbs_conn_est_active(cci__ep_t *ep, struct rdma_cm_event *cm_evt)
{
	int			ret	= CCI_SUCCESS;
	cci__conn_t		*conn	= NULL;
	verbs_conn_t		*vconn	= NULL;
	verbs_conn_request_t	*cr	= NULL;
	verbs_tx_t		*tx	= NULL;
	uint32_t		header	= 0;

	CCI_ENTER;

	conn = cm_evt->id->context;
	vconn = conn->priv;
	cr = vconn->conn_req;

	verbs_vconn_set_mss(vconn);
	conn->connection.max_send_size = vconn->mss;

	tx = verbs_get_tx(ep);
	if (!tx) {
		ret = CCI_ENOBUFS;
		goto out;
	}

	tx->evt.event.type = CCI_EVENT_NONE; /* never hand to application */
	tx->evt.conn = conn;

	/* if application has a conn request payload, send it */
	if (cr && cr->len)
		memcpy(tx->buffer, cr->ptr, cr->len);

	header = VERBS_MSG_CONN_PAYLOAD;
	header |= (cr->attr & 0xF) << 4;	/* magic number */
	if (cr && cr->len)
		header |= (cr->len & 0xFFF) << 8;	/* magic number */

	ret = verbs_post_send(conn, (uintptr_t) tx, tx->buffer, cr->len, header);
out:
	CCI_EXIT;
	return ret;
}

static int
verbs_conn_est_passive(cci__ep_t *ep, struct rdma_cm_event *cm_evt)
{
	int		ret	= CCI_SUCCESS;

	CCI_ENTER;

	CCI_EXIT;
	return ret;
}

static int
verbs_handle_conn_established(cci__ep_t *ep, struct rdma_cm_event *cm_evt)
{
	int		ret	= CCI_SUCCESS;
	cci__conn_t	*conn	= NULL;
	verbs_conn_t	*vconn	= NULL;

	CCI_ENTER;

	conn = cm_evt->id->context;
	assert(conn);
	vconn = conn->priv;
	assert(vconn);
	assert(vconn->state == VERBS_CONN_ACTIVE || vconn->state == VERBS_CONN_PASSIVE);

	switch (vconn->state) {
		case VERBS_CONN_ACTIVE:
			ret = verbs_conn_est_active(ep, cm_evt);
			break;
		case VERBS_CONN_PASSIVE:
			ret = verbs_conn_est_passive(ep, cm_evt);
			break;
		default:
			debug(CCI_DB_INFO, "%s: incorrect conn state %s", __func__,
				verbs_conn_state_str(vconn->state));
			break;
	}

	CCI_EXIT;
	return ret;
}

static int
verbs_get_cm_event(cci__ep_t *ep)
{
	int			ret	= CCI_EAGAIN;
	verbs_ep_t		*vep	= ep->priv;
	struct rdma_cm_event	*cm_evt	= NULL;

	CCI_ENTER;

	pthread_mutex_lock(&ep->lock);
	if (ep->closing || !vep) {
		pthread_mutex_unlock(&ep->lock);
		goto out;
	}
	pthread_mutex_unlock(&ep->lock);

	ret = rdma_get_cm_event(vep->channel, &cm_evt);
	if (ret == -1) {
		ret = errno;
		goto out;
	}

	switch (cm_evt->event) {
		case RDMA_CM_EVENT_CONNECT_REQUEST:
			ret = verbs_handle_conn_request(ep, cm_evt);
			break;
		case RDMA_CM_EVENT_ESTABLISHED:
			ret = verbs_handle_conn_established(ep, cm_evt);
			break;
		default:
			debug(CCI_DB_CONN, "ignoring %s event",
				rdma_event_str(cm_evt->event));
	}

	ret = rdma_ack_cm_event(cm_evt);
	if (ret == -1)
		ret = errno;
out:
	CCI_EXIT;
	return ret;
}

static int
verbs_handle_conn_payload(cci__ep_t *ep, struct ibv_wc wc)
{
	int		ret		= CCI_SUCCESS;
	uint32_t	header		= 0;
	uint32_t	len		= 0;
	cci__conn_t	*conn		= NULL;
	verbs_conn_t	*vconn		= NULL;
	verbs_conn_t	*vc		= NULL;
	verbs_ep_t	*vep		= ep->priv;
	verbs_rx_t	*rx		= NULL;

	CCI_ENTER;

	/* find the passive conn waiting for this message */
	pthread_mutex_lock(&ep->lock);
	TAILQ_FOREACH(vc, &vep->passive, temp) {
		if (vc->id->qp->qp_num == wc.qp_num) {
			vconn = vc;
			conn = vconn->conn;
			assert(conn == vc->id->context);
			TAILQ_REMOVE(&vep->passive, vconn, temp);
			break;
		}
	}
	pthread_mutex_unlock(&ep->lock);

	if (!vconn) {
		debug(CCI_DB_WARN, "%s: no conn found for message from qp_num %u",
			__func__, wc.qp_num);
		goto out;
	}

	header = ntohl(wc.imm_data);
	conn->connection.attribute = (header >> 4) & 0xF;
	len = (header >> 8) & 0xFFF;
	if (len != wc.byte_len)
		debug(CCI_DB_WARN, "%s: len %u != wc.byte_len %u",
			__func__, len, wc.byte_len);

	rx = (verbs_rx_t *) (uintptr_t) wc.wr_id;
	rx->evt.conn = conn;
	rx->evt.event.type = CCI_EVENT_CONNECT_REQUEST;
	rx->evt.event.request.attribute = conn->connection.attribute;
	*((uint32_t *) &rx->evt.event.request.data_len) = len;
	if (len)
		*((void **) &rx->evt.event.request.data_ptr) = vep->rx_buf + rx->offset;
	else
		*((void **) &rx->evt.event.request.data_ptr) = NULL;

	pthread_mutex_lock(&ep->lock);
	TAILQ_INSERT_TAIL(&ep->evts, &rx->evt, entry);
	pthread_mutex_unlock(&ep->lock);
out:
	CCI_EXIT;
	return ret;
}

static int
verbs_handle_conn_reply(cci__ep_t *ep, struct ibv_wc wc)
{
	int		ret		= CCI_SUCCESS;
	uint32_t	header		= 0;
	cci__conn_t	*conn		= NULL;
	verbs_conn_t	*vconn		= NULL;
	verbs_conn_t	*vc		= NULL;
	verbs_ep_t	*vep		= ep->priv;
	verbs_rx_t	*rx		= NULL;

	CCI_ENTER;

	/* find the active conn waiting for this message */
	pthread_mutex_lock(&ep->lock);
	TAILQ_FOREACH(vc, &vep->active, temp) {
		if (vc->id->qp->qp_num == wc.qp_num) {
			vconn = vc;
			conn = vconn->conn;
			assert(conn == vc->id->context);
			TAILQ_REMOVE(&vep->passive, vconn, temp);
			break;
		}
	}
	pthread_mutex_unlock(&ep->lock);

	if (!vconn) {
		debug(CCI_DB_WARN, "%s: no conn found for message from qp_num %u",
			__func__, wc.qp_num);
		goto out;
	}

	header = ntohl(wc.imm_data);

	rx = (verbs_rx_t *) (uintptr_t) wc.wr_id;
	rx->evt.event.type = (header >> 4) & 0xF;	/* magic number */
	if (rx->evt.event.type == CCI_EVENT_CONNECT_ACCEPTED) {
		rx->evt.event.accepted.context = vconn->conn_req ?
			vconn->conn_req->context : NULL;
		rx->evt.event.accepted.connection = &conn->connection;
		pthread_mutex_lock(&ep->lock);
		TAILQ_INSERT_TAIL(&vep->conns, vconn, entry);
		pthread_mutex_unlock(&ep->lock);
	} else if (rx->evt.event.type == CCI_EVENT_CONNECT_REJECTED) {
		rx->evt.event.rejected.context = vconn->conn_req ?
			vconn->conn_req->context : NULL;
	} else {
		debug(CCI_DB_WARN, "%s: invalid reply %u", __func__, rx->evt.event.type);
	}

	pthread_mutex_lock(&ep->lock);
	TAILQ_INSERT_TAIL(&ep->evts, &rx->evt, entry);
	pthread_mutex_unlock(&ep->lock);

out:
	CCI_EXIT;
	return ret;
}

static int
verbs_handle_msg(cci__ep_t *ep, struct ibv_wc wc)
{
	int		ret		= CCI_SUCCESS;
	cci__conn_t	*conn		= NULL;
	verbs_conn_t	*vconn		= NULL;
	verbs_conn_t	*vc		= NULL;
	verbs_ep_t	*vep		= ep->priv;
	verbs_rx_t	*rx		= NULL;

	CCI_ENTER;

	/* find the conn for this message */
	pthread_mutex_lock(&ep->lock);
	TAILQ_FOREACH(vc, &vep->conns, temp) {
		if (vc->id->qp->qp_num == wc.qp_num) {
			vconn = vc;
			conn = vconn->conn;
			assert(conn == vc->id->context);
			break;
		}
	}
	pthread_mutex_unlock(&ep->lock);

	if (!vconn) {
		debug(CCI_DB_WARN, "%s: no conn found for message from qp_num %u",
			__func__, wc.qp_num);
		goto out;
	}

	rx = (verbs_rx_t *) (uintptr_t) wc.wr_id;
	rx->evt.conn = conn;
	rx->evt.event.type = CCI_EVENT_RECV;
	rx->evt.event.recv.connection = &conn->connection;
	*((uint32_t *) &rx->evt.event.recv.len) = wc.byte_len;
	if (rx->evt.event.recv.len)
		*((void **) &rx->evt.event.request.data_ptr) = vep->rx_buf + rx->offset;
	else
		*((void **) &rx->evt.event.request.data_ptr) = NULL;

	pthread_mutex_lock(&ep->lock);
	TAILQ_INSERT_TAIL(&ep->evts, &rx->evt, entry);
	pthread_mutex_unlock(&ep->lock);
out:
	CCI_EXIT;
	return ret;
}

static int
verbs_handle_remote_request(cci__ep_t *ep, struct ibv_wc wc)
{
	int		ret		= CCI_SUCCESS;
	cci__conn_t	*conn		= NULL;
	verbs_conn_t	*vconn		= NULL;
	verbs_conn_t	*vc		= NULL;
	verbs_ep_t	*vep		= ep->priv;
	verbs_tx_t	*tx		= NULL;

	CCI_ENTER;

	/* find the conn for this message */
	pthread_mutex_lock(&ep->lock);
	TAILQ_FOREACH(vc, &vep->conns, temp) {
		if (vc->id->qp->qp_num == wc.qp_num) {
			vconn = vc;
			conn = vconn->conn;
			assert(conn == vc->id->context);
			break;
		}
	}
	pthread_mutex_unlock(&ep->lock);

	if (!vconn) {
		debug(CCI_DB_WARN, "%s: no conn found for message from qp_num %u",
			__func__, wc.qp_num);
		ret = CCI_ERR_NOT_FOUND;
		goto out;
	}

	tx = verbs_get_tx(ep);
	if (!tx) {
		CCI_EXIT;
		ret = CCI_ENOBUFS;
		goto out;
	}

	tx->msg_type = VERBS_MSG_RMA_REMOTE_REPLY;
	memset(&tx->evt, 0, sizeof(tx->evt));
	tx->evt.conn = conn;
	tx->evt.event.type = CCI_EVENT_NONE;

out:
	CCI_EXIT;
	return ret;
}

static int
verbs_handle_recv(cci__ep_t *ep, struct ibv_wc wc)
{
	int			ret	= CCI_SUCCESS;
	uint32_t		header	= 0;
	verbs_msg_type_t	type	= 0;

	CCI_ENTER;

	header = ntohl(wc.imm_data);
	debug(CCI_DB_INFO, "recv'd header 0x%x", header);
	type = header & 0xF; /* magic number */

	switch (type) {
	case VERBS_MSG_CONN_PAYLOAD:
		ret = verbs_handle_conn_payload(ep, wc);
		break;
	case VERBS_MSG_CONN_REPLY:
		ret = verbs_handle_conn_reply(ep, wc);
		break;
	case VERBS_MSG_SEND:
		ret = verbs_handle_msg(ep, wc);
		break;
	case VERBS_MSG_RMA_REMOTE_REQUEST:
		ret = verbs_handle_remote_request(ep, wc);
		break;
	default:
		debug(CCI_DB_INFO, "%s: ignoring %s msg",
			__func__, verbs_msg_type_str(type));
		break;
	}

	CCI_EXIT;
	return ret;
}

static int
verbs_complete_send_msg(cci__ep_t *ep, struct ibv_wc wc)
{
	int		ret	= CCI_SUCCESS;
	verbs_tx_t	*tx	= (verbs_tx_t *)(uintptr_t) wc.wr_id;

	CCI_ENTER;

	if (wc.status != IBV_WC_SUCCESS) {
		uint32_t status	= 0;

		switch (wc.status) {
		case IBV_WC_RETRY_EXC_ERR:
			status = CCI_ETIMEDOUT;	/* FIXME: is this correct? */
			break;
		case IBV_WC_RNR_RETRY_EXC_ERR:
			status = CCI_ERR_RNR;	/* FIXME: is this correct? */
			break;
		default:
			debug(CCI_DB_MSG, "%s: send completed with %s",
				__func__, ibv_wc_status_str(wc.status));
			status = CCI_ERROR;
		}
		tx->evt.event.send.status = status;
	}
	pthread_mutex_lock(&ep->lock);
	TAILQ_INSERT_TAIL(&ep->evts, &tx->evt, entry);
	pthread_mutex_unlock(&ep->lock);

	CCI_EXIT;
	return ret;
}

static int
verbs_complete_send(cci__ep_t *ep, struct ibv_wc wc)
{
	int			ret	= CCI_SUCCESS;
	verbs_msg_type_t	type	= VERBS_MSG_INVALID;
	verbs_tx_t		*tx	= (verbs_tx_t *) (uintptr_t) wc.wr_id;
	verbs_ep_t		*vep	= ep->priv;

	CCI_ENTER;

	if (tx)
		type = tx->msg_type;

	//debug(CCI_DB_ALL, "%s: imm set %u", __func__, wc.wc_flags & IBV_WC_WITH_IMM);
	switch (type) {
	case VERBS_MSG_SEND:
		ret = verbs_complete_send_msg(ep, wc);
		break;
	case VERBS_MSG_CONN_REQUEST:
		break;
	case VERBS_MSG_CONN_PAYLOAD:
		break;
	case VERBS_MSG_CONN_REPLY:
		break;
	default:
		debug(CCI_DB_MSG, "%s: ignoring send completion for msg type %d",
			__func__, type);
		break;
	}
	if (ret) {
		pthread_mutex_lock(&ep->lock);
		TAILQ_INSERT_HEAD(&vep->idle_txs, tx, entry);
		pthread_mutex_unlock(&ep->lock);
	}

	CCI_EXIT;
	return ret;
}

static int
verbs_handle_send_completion(cci__ep_t *ep, struct ibv_wc wc)
{
	int			ret	= CCI_SUCCESS;
	uint32_t		header	= 0;
	verbs_msg_type_t	type	= 0;
	verbs_tx_t		*tx	= (verbs_tx_t *) (uintptr_t) wc.wr_id;

	CCI_ENTER;

	//debug(CCI_DB_ALL, "%s: imm set %u", __func__, IBV_WC_WITH_IMM & wc.wc_flags);
	header = ntohl(wc.imm_data);
	type = header & 0xF; /* magic number */
#if 0
	debug(CCI_DB_ALL, "%s: completing %s send header 0x%x msg_type %s", __func__,
		verbs_msg_type_str(type), header, tx ? verbs_msg_type_str(tx->msg_type): "null");
#endif
	if (tx)
		type = tx->msg_type;

	switch (type) {
	case VERBS_MSG_CONN_PAYLOAD:
		debug(CCI_DB_CONN, "%s: send completed of conn_payload", __func__);
		break;
	case VERBS_MSG_CONN_REPLY:
		debug(CCI_DB_CONN, "%s: send completed of conn_reply", __func__);
		break;
	case VERBS_MSG_SEND:
		debug(CCI_DB_CONN, "%s: send completed", __func__);
		ret = verbs_complete_send(ep, wc);
		break;
	default:
		debug(CCI_DB_INFO, "%s: ignoring %s msg",
			__func__, verbs_msg_type_str(type));
		break;
	}

	CCI_EXIT;
	return ret;
}

#define VERBS_WC_CNT	8

static int
verbs_get_cq_event(cci__ep_t *ep)
{
	int		ret	= CCI_EAGAIN;
	int		i	= 0;
	int		found	= 0;
	struct ibv_wc	wc[VERBS_WC_CNT];
	verbs_ep_t	*vep	= ep->priv;

	CCI_ENTER;

	ret = ibv_poll_cq(vep->cq, VERBS_WC_CNT, wc);
	if (ret == -1) {
		ret = errno;
		goto out;
	}

	found = ret;

	for (i = 0; i < found; i++) {
		if (wc[i].status != IBV_WC_SUCCESS) {
			debug(CCI_DB_INFO, "%s wc returned with status %s",
				wc[i].opcode & IBV_WC_RECV ? "recv" : "send",
				ibv_wc_status_str(wc[i].status));
			/* TODO do what? */
		}
		if (wc[i].opcode & IBV_WC_RECV) {
			ret = verbs_handle_recv(ep, wc[i]);
		} else if (wc[i].opcode == IBV_WC_SEND) {
			ret = verbs_handle_send_completion(ep, wc[i]);
		} else {
			debug(CCI_DB_ALL, "%s: missed opcode %u status %s wr_id 0x%"PRIx64,
				__func__, wc[i].opcode, ibv_wc_status_str(wc[i].status),
				wc[i].wr_id);
		}
	}

out:
	CCI_EXIT;
	return ret;
}

#define VERBS_CM_EVT 0
#define VERBS_CQ_EVT 1

static void
verbs_progress_ep(cci__ep_t *ep)
{
	int		ret	= CCI_SUCCESS;
	static int	which	= 0;
	int		try	= 0;

	CCI_ENTER;

again:
	try++;
	switch (which) {
		case VERBS_CM_EVT:
			ret = verbs_get_cm_event(ep);
			break;
		case VERBS_CQ_EVT:
			ret = verbs_get_cq_event(ep);
			break;
	}
	which = !which;
	if (ret == CCI_EAGAIN && try == 1)
		goto again;

	CCI_EXIT;
	return;
}

static int
verbs_get_event(cci_endpoint_t *endpoint,
		cci_event_t ** const event)
{
	int		ret	= CCI_SUCCESS;
	cci__ep_t	*ep	= NULL;
	cci__evt_t	*e	= NULL;
	cci__evt_t	*ev	= NULL;

	CCI_ENTER;

	ep = container_of(endpoint, cci__ep_t, endpoint);
	verbs_progress_ep(ep);

	pthread_mutex_lock(&ep->lock);
	TAILQ_FOREACH(e, &ep->evts, entry) {
		if (e->event.type == CCI_EVENT_SEND) {
			/* NOTE: if it is blocking, skip it since sendv()
			 *       is waiting on it
			 */
			verbs_tx_t *tx = container_of(e, verbs_tx_t, evt);
			if (tx->flags & CCI_FLAG_BLOCKING) {
				continue;
			} else {
				ev = e;
				break;
			}
		} else {
			ev = e;
			break;
		}
	}

	if (ev)
		TAILQ_REMOVE(&ep->evts, ev, entry);
	else
		ret = CCI_EAGAIN;

	pthread_mutex_unlock(&ep->lock);

	/* TODO drain fd so that they can block again */

	*event = &ev->event;

	CCI_EXIT;
	return ret;
}

static int
verbs_return_conn_request(cci_event_t *event)
{
	int		ret	= CCI_SUCCESS;
	cci__evt_t	*evt	= container_of(event, cci__evt_t, event);
	verbs_rx_t	*rx	= container_of(evt, verbs_rx_t, evt);
	cci__conn_t	*conn	= evt->conn;
	verbs_conn_t	*vconn	= conn->priv;
	cci__ep_t	*ep	= evt->ep;

	CCI_ENTER;

	if (vconn->conn_req) {
		if (vconn->conn_req->len) {
			assert(vconn->conn_req->ptr);
			free(vconn->conn_req->ptr);
		}
		free(vconn->conn_req);
		vconn->conn_req = NULL;
	}

	ret = verbs_post_rx(ep, rx);

	CCI_EXIT;
	return ret;
}

static int
verbs_return_event(cci_event_t *event)
{
	int		ret	= CCI_SUCCESS;

	CCI_ENTER;

	switch (event->type) {
	case CCI_EVENT_CONNECT_REQUEST:
		ret = verbs_return_conn_request(event);
		break;
	case CCI_EVENT_CONNECT_ACCEPTED:
	case CCI_EVENT_RECV:
	{
		cci__evt_t	*evt	= container_of(event, cci__evt_t, event);
		cci__ep_t	*ep	= evt->ep;
		verbs_rx_t	*rx	= container_of(evt, verbs_rx_t, evt);

		ret = verbs_post_rx(ep, rx);
		if (ret) {
			ret = errno;
			debug(CCI_DB_MSG, "%s: post_rx() returned %s", __func__, strerror(ret));
		}
	}
		break;
	case CCI_EVENT_SEND:
	{
		cci__evt_t	*evt	= container_of(event, cci__evt_t, event);
		cci__ep_t	*ep	= evt->ep;
		verbs_ep_t	*vep	= ep->priv;
		verbs_tx_t	*tx	= container_of(evt, verbs_tx_t, evt);

		pthread_mutex_lock(&ep->lock);
		TAILQ_INSERT_HEAD(&vep->idle_txs, tx, entry);
		pthread_mutex_unlock(&ep->lock);
	}
		break;
	default:
		debug(CCI_DB_WARN, "%s: ignoring %d event",
			__func__, event->type);
		break;
	}

	CCI_EXIT;
	return ret;
}

static int
verbs_send_common(cci_connection_t *connection, struct iovec *iov, uint32_t iovcnt,
		void *context, int flags, verbs_rma_op_t *rma_op)
{
	int		ret		= CCI_SUCCESS;
	int		i		= 0;
	int		is_reliable	= 0;
	uint32_t	len		= 0;
	cci_endpoint_t	*endpoint	= connection->endpoint;
	cci__conn_t	*conn		= NULL;
	cci__ep_t	*ep		= NULL;
	verbs_conn_t	*vconn		= NULL;
	verbs_ep_t	*vep		= NULL;
	verbs_tx_t	*tx		= NULL;
	uint32_t	header		= VERBS_MSG_SEND;

	CCI_ENTER;

	if (!vglobals) {
		CCI_EXIT;
		return CCI_ENODEV;
	}

	for (i = 0; i < iovcnt; i++)
		len += (uint32_t) iov[i].iov_len;

	if (len > connection->max_send_size) {
		debug(CCI_DB_MSG, "length %u > connection->max_send_size %u",
				len, connection->max_send_size);
		CCI_EXIT;
		return CCI_EMSGSIZE;
	}

	ep = container_of(endpoint, cci__ep_t, endpoint);
	vep = ep->priv;
	conn = container_of(connection, cci__conn_t, connection );
	vconn = conn->priv;

	is_reliable = cci_conn_is_reliable(conn);

	/* get a tx */
	tx = verbs_get_tx(ep);
	if (!tx) {
		debug(CCI_DB_MSG, "%s: no txs", __func__);
		CCI_EXIT;
		return CCI_ENOBUFS;
	}

	/* tx bookkeeping */
	tx->msg_type = VERBS_MSG_SEND;
	tx->flags = flags;
	tx->rma_op = NULL; /* only set if RMA completion msg */

	/* setup generic CCI event */
	tx->evt.conn = conn;
	tx->evt.ep = ep;
	tx->evt.event.type = CCI_EVENT_SEND;
	tx->evt.event.send.connection = connection;
	tx->evt.event.send.context = context;
	tx->evt.event.send.status = CCI_SUCCESS; /* for now */

	/* always copy into tx's buffer */
	if (len) {
		uint32_t	offset	= 0;

		for (i = 0; i < iovcnt; i++) {
			memcpy(tx->buffer + offset, iov[i].iov_base, iov[i].iov_len);
			offset += iov[i].iov_len;
		}
	}
	tx->len = len;

	ret = verbs_post_send(conn, (uintptr_t) tx, tx->buffer, tx->len, header);
	if (ret) {
		debug(CCI_DB_CONN, "%s: unable to send", __func__);
		goto out;
	}

	if (flags & CCI_FLAG_BLOCKING && is_reliable) {
		cci__evt_t *e, *evt = NULL;
		do {
			pthread_mutex_lock(&ep->lock);
			TAILQ_FOREACH(e, &ep->evts, entry) {
				if (&tx->evt == e) {
					evt = e;
					TAILQ_REMOVE(&ep->evts, evt, entry);
					ret = evt->event.send.status;
				}
			}
			pthread_mutex_unlock(&ep->lock);
		} while (evt == NULL);
		/* if successful, queue the tx now,
		 * if not, queue it below */
		if (ret == CCI_SUCCESS) {
			pthread_mutex_lock(&ep->lock);
			TAILQ_INSERT_HEAD(&vep->idle_txs, tx, entry);
			pthread_mutex_unlock(&ep->lock);
		}
	}

out:
	if (ret) {
		pthread_mutex_lock(&ep->lock);
		TAILQ_INSERT_HEAD(&vep->idle_txs, tx, entry);
		pthread_mutex_unlock(&ep->lock);
	}
	CCI_EXIT;
	return ret;
}

static int
verbs_send(cci_connection_t *connection, /* magic number */
		void *msg_ptr, uint32_t msg_len,
		void *context, int flags)
{
	int		ret	= CCI_SUCCESS;
	uint32_t	iovcnt	= 0;
	struct iovec	iov = { NULL, 0};

	CCI_ENTER;

	if (msg_ptr && msg_len > 0) {
		iovcnt = 1;
		iov.iov_base = msg_ptr;
		iov.iov_len = msg_len;
	}

	ret = verbs_send_common(connection, &iov, iovcnt, context, flags, NULL);

	CCI_EXIT;
	return ret;
}


static int
verbs_sendv(cci_connection_t *connection,
		struct iovec *data, uint32_t iovcnt,
		void *context, int flags)
{
	int		ret	= CCI_SUCCESS;

	CCI_ENTER;

	ret = verbs_send_common(connection, data, iovcnt, context, flags, NULL);

	CCI_EXIT;
	return ret;
}


static int
verbs_rma_register(cci_endpoint_t *endpoint,
			cci_connection_t *connection,
			void *start, uint64_t length,
			uint64_t *rma_handle)
{
	int			ret	= CCI_SUCCESS;
	cci__ep_t		*ep	= container_of(endpoint, cci__ep_t, endpoint);
	verbs_ep_t		*vep	= ep->priv;
	verbs_rma_handle_t	*handle	= NULL;

	CCI_ENTER;

	if (!vglobals) {
		CCI_EXIT;
		return CCI_ENODEV;
	}

	handle = calloc(1, sizeof(*handle));
	if (!handle) {
		debug(CCI_DB_INFO, "no memory for rma handle");
		CCI_EXIT;
		return CCI_ENOMEM;
	}

	handle->ep = ep;

	handle->mr = ibv_reg_mr(vep->pd, start, (size_t) length,
			IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_WRITE |
			IBV_ACCESS_REMOTE_READ);
	if (!handle->mr) {
		free(handle);
		CCI_EXIT;
		return CCI_ERROR;
	}

	pthread_mutex_lock(&ep->lock);
	TAILQ_INSERT_TAIL(&vep->handles, handle, entry);
	pthread_mutex_unlock(&ep->lock);

	*rma_handle = (uint64_t)(uintptr_t)handle;

	CCI_EXIT;
	return ret;
}


static int
verbs_rma_deregister(uint64_t rma_handle)
{
	CCI_ENTER;
	CCI_EXIT;
	return CCI_ERR_NOT_IMPLEMENTED;
}

static verbs_rma_remote_t *
verbs_conn_get_remote(cci__conn_t *conn, uint64_t remote_handle)
{
	cci__ep_t		*ep	= NULL;
	verbs_conn_t		*vconn	= conn->priv;
	verbs_rma_remote_t	*rem	= NULL;
	verbs_rma_remote_t	*tmp	= NULL;

	CCI_ENTER;

	ep = container_of(conn->connection.endpoint, cci__ep_t, endpoint);

	pthread_mutex_lock(&ep->lock);
	TAILQ_FOREACH(tmp, &vconn->remotes, entry) {
		if (tmp->remote_handle == remote_handle) {
			rem = tmp;
			/* keep list in LRU order */
			if (TAILQ_FIRST(&vconn->remotes) != rem) {
				TAILQ_REMOVE(&vconn->remotes, rem, entry);
				TAILQ_INSERT_HEAD(&vconn->remotes, rem, entry);
			}
			break;
		}
	}
	pthread_mutex_unlock(&ep->lock);

	CCI_EXIT;
	return rem;
}

typedef union verbs_u64 {
	uint64_t	ull;
	uint32_t	ul[2];
} verbs_u64_t;

#if 0
static uint64_t
verbs_ntohll(uint64_t val)
{
	verbs_u64_t	net = { .ull = val };
	verbs_u64_t	host;

	host.ul[0] = ntohl(net.ul[1]);
	host.ul[1] = ntohl(net.ul[0]);

	return host.ull;
}
#endif

static uint64_t
verbs_htonll(uint64_t val)
{
	verbs_u64_t	host = { .ull = val };
	verbs_u64_t	net;

	net.ul[0] = htonl(host.ul[1]);
	net.ul[1] = htonl(host.ul[0]);

	return net.ull;
}

static int
verbs_conn_request_remote(verbs_rma_op_t *rma_op, uint64_t remote_handle)
{
	int			ret	= CCI_SUCCESS;
	cci__ep_t		*ep	= NULL;
	cci__conn_t		*conn	= rma_op->evt.conn;
	verbs_tx_t		*tx	= NULL;
	uint64_t		header	= VERBS_MSG_RMA_REMOTE_REQUEST;

	CCI_ENTER;

	ep = container_of(conn->connection.endpoint, cci__ep_t, endpoint);

	tx = verbs_get_tx(ep);
	if (!tx) {
		CCI_EXIT;
		return CCI_ENOBUFS;
	}

	/* tx bookkeeping */
	tx->msg_type = VERBS_MSG_RMA_REMOTE_REQUEST;
	tx->flags = 0;
	tx->rma_op = rma_op;
	tx->len = sizeof(rma_op->remote_handle);

	memset(&tx->evt, 0, sizeof(cci__evt_t));

	/* put in network byte order */
	memcpy(tx->buffer, (void *)(uintptr_t) verbs_htonll(rma_op->remote_handle), tx->len);

	ret = verbs_post_send(conn, (uintptr_t) rma_op, tx->buffer, tx->len, header);

	CCI_EXIT;
	return ret;
}

static int
verbs_post_rma(verbs_rma_op_t *rma_op)
{
	int			ret	= CCI_SUCCESS;
	cci__conn_t		*conn	= rma_op->evt.conn;
	verbs_conn_t		*vconn	= conn->priv;
	verbs_rma_handle_t	*local	= (verbs_rma_handle_t *)(uintptr_t)rma_op->local_handle;
	struct ibv_sge		list;
	struct ibv_send_wr	wr, *bad_wr;

	CCI_ENTER;

	memset(&list, 0, sizeof(list));
	list.addr = (uintptr_t) local->mr->addr + (uintptr_t) rma_op->local_offset;
	list.length = rma_op->len;
	list.lkey = local->mr->lkey;

	memset(&wr, 0, sizeof(wr));
	wr.wr_id = (uintptr_t) rma_op;
	wr.sg_list = &list;
	wr.num_sge = 1;
	wr.opcode = rma_op->flags & CCI_FLAG_WRITE ? IBV_WR_RDMA_WRITE : IBV_WR_RDMA_READ;
	wr.send_flags = IBV_SEND_SIGNALED;
	if (rma_op->flags & CCI_FLAG_FENCE)
		wr.send_flags |= IBV_SEND_FENCE;
	wr.wr.rdma.remote_addr = rma_op->remote->remote_addr;
	wr.wr.rdma.rkey = rma_op->remote->rkey;

	ret = ibv_post_send(vconn->id->qp, &wr, &bad_wr);
	if (ret == -1)
		ret = errno;

	CCI_EXIT;
	return ret;
}

static int
verbs_rma(cci_connection_t *connection,
		void *msg_ptr, uint32_t msg_len,
		uint64_t local_handle, uint64_t local_offset,
		uint64_t remote_handle, uint64_t remote_offset,
		uint64_t data_len, void *context, int flags)
{
	int			ret	= CCI_SUCCESS;
	cci__ep_t		*ep	= NULL;
	cci__conn_t		*conn	= NULL;
	verbs_ep_t		*vep	= NULL;
	verbs_conn_t		*vconn	= NULL;
	verbs_rma_handle_t	*local	= (verbs_rma_handle_t *)(uintptr_t)local_handle;
	verbs_rma_op_t		*rma_op	= NULL;

	CCI_ENTER;

	if (!vglobals) {
		CCI_EXIT;
		return CCI_ENODEV;
	}

	conn = container_of(connection, cci__conn_t, connection);
	vconn = conn->priv;
	ep = container_of(connection->endpoint, cci__ep_t, endpoint);
	vep = ep->priv;

	if (!local || local->ep != ep) {
		CCI_EXIT;
		return CCI_EINVAL;
	}

	rma_op = calloc(1, sizeof(*rma_op));
	if (!rma_op) {
		CCI_EXIT;
		return CCI_ENOMEM;
	}

	rma_op->local_handle = local_handle;
	rma_op->local_offset = local_offset;
	rma_op->remote_handle = remote_handle;
	rma_op->remote_offset = remote_offset;
	rma_op->len = data_len;
	rma_op->context = context;
	rma_op->flags = flags;
	rma_op->msg_len = msg_len;
	rma_op->msg_ptr = msg_ptr;

	rma_op->evt.event.type = CCI_EVENT_SEND;
	rma_op->evt.event.send.connection = connection;
	rma_op->evt.event.send.context = context;
	rma_op->evt.event.send.status = CCI_SUCCESS; /* for now */
	rma_op->evt.ep = ep;
	rma_op->evt.conn = conn;
	rma_op->evt.priv = rma_op;

	pthread_mutex_lock(&ep->lock);
	TAILQ_INSERT_TAIL(&vep->rma_ops, rma_op, entry);
	pthread_mutex_unlock(&ep->lock);

	/* Do we have this remote handle info?
	 * If not, request it from the peer */
	rma_op->remote = verbs_conn_get_remote(conn, remote_handle);
	if (rma_op->remote)
		ret = verbs_post_rma(rma_op);
	else
		ret = verbs_conn_request_remote(rma_op, remote_handle);
	if (ret) {
		/* FIXME clean up? */
	}

	CCI_EXIT;
	return ret;
}
