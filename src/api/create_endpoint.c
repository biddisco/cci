/*
 * Copyright (c) 2010 Cisco Systems, Inc.  All rights reserved.
 * Copyright © 2010-2011 UT-Battelle, LLC. All rights reserved.
 * Copyright © 2010-2011 Oak Ridge National Labs.  All rights reserved.
 * Copyright © 2012 inria.  All rights reserved.
 *
 * See COPYING in top-level directory
 *
 * $COPYRIGHT$
 *
 */

#include "cci/config.h"

#include <stdio.h>
#include <stdlib.h>

#include "cci.h"
#include "cci_lib_types.h"
#include "plugins/core/core.h"

int cci_create_endpoint(cci_device_t * device,
			int flags,
			cci_endpoint_t ** endpoint, cci_os_handle_t * fd)
{
	int ret;
	cci__ep_t *ep;
	cci__dev_t *dev;

	if (NULL == device) {
		/* walk list of devs to find default device */
		TAILQ_FOREACH(dev, &globals->devs, entry) {
			if (dev->is_default) {
				device = &dev->device;
				break;
			}
		}
		if (!device && !TAILQ_EMPTY(&globals->devs)) {
			/* no default found, use first (highest priority) device? */
			dev = TAILQ_FIRST(&globals->devs);
			device = &dev->device;
		}
		if (!device)
			return CCI_ENODEV;
	} else {
		/* use given device */
		dev = container_of(device, cci__dev_t, device);
	}

	if (dev->is_up == 0)
		return CCI_ENODEV;

	ep = calloc(1, sizeof(*ep));
	if (!ep)
		return CCI_ENOMEM;

	TAILQ_INIT(&ep->evts);
	pthread_mutex_init(&ep->lock, NULL);
	ep->dev = dev;
	*endpoint = &ep->endpoint;

	ret = dev->plugin->create_endpoint(device, flags, endpoint, fd);

	ep->plugin = dev->plugin;
	pthread_mutex_lock(&dev->lock);
	/* TODO check dev's state */
	TAILQ_INSERT_TAIL(&dev->eps, ep, entry);
	pthread_mutex_unlock(&dev->lock);

	return ret;
}
