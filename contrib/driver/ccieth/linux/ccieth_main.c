/*
 * CCI over Ethernet
 * Copyright © INRIA 2011
 */

#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#include <linux/pci.h>
#include <linux/idr.h>
#include <linux/rcupdate.h>

#include <ccieth_io.h>
#include <ccieth_common.h>

static struct idr ccieth_ep_idr;
static spinlock_t ccieth_ep_idr_lock;

struct ccieth_endpoint {
	struct net_device *ifp;
	int id;
	char * recvq;
	__u32 last_busy_slot_offset; /* offset of the last filled slot.
				      * its next_busy_offset must be -1.
				      * it will be changed when a new free slot is used.
				      */
	__u32 last_free_slot_offset; /* offset of the last freed slot.
				      * its next_free_offset must be -1.
				      * it will be changed when a new busy slot is returned.
				      */
	__u32 first_free_slot_offset; /* offset of the next freed slot to use. */
};

/*
 * endpoint init:
 * - set last_busy_slot_offset to first slot
 * - set first_free_slot_offset to second slot
 * - queue all other slots in the free list
 * - set last_free_slot_offset to the last one
 * - set next_free_offset to -1 in last slot
 * - set next_busy_offset to -1 in first slot
 * - queue a OK event in first slot
 *
 * new event:
 * - if first_free_slot_offset is -1, event queue full
 * - take first_free_slot_offset and make it its successor
 * - set slot next_busy_slot to -1
 * - fill event slot
 * - set last_busy_slot_offset next_busy_offset to new slot
 * - set last_busy_slot_offset to new slot
 *
 * return event:
 * - set next_free_slot_offset to -1
 * - if first_free_slot_offset is -1, make it the new slot
 * - otherwise make last_free_slot_offset next_free_offset the new slot
 *
 * userspace:
 * - remind last slot offset, poll on its next_busy_offset
 */

static void
ccieth_destroy_endpoint(struct ccieth_endpoint *ep)
{
	spin_lock(&ccieth_ep_idr_lock);
	idr_remove(&ccieth_ep_idr, ep->id);
	spin_unlock(&ccieth_ep_idr_lock);
	dev_put(ep->ifp);
	if (ep->recvq) {
		vfree(ep->recvq);
	}
	kfree(ep);
}

static struct ccieth_endpoint *
ccieth_create_endpoint(struct ccieth_ioctl_create_endpoint *arg)
{
	struct ccieth_endpoint *ep;
	struct net_device *ifp;
	int id;
	int err;

	rcu_read_lock();
	ifp = dev_getbyhwaddr_rcu(&init_net, ARPHRD_ETHER, (const char *) &arg->addr);
	if (!ifp) /* allow loopback to ease development */
		ifp = dev_getbyhwaddr_rcu(&init_net, ARPHRD_LOOPBACK, (const char *) &arg->addr);
	if (!ifp) {
		rcu_read_unlock();
		err = -ENODEV;
		goto out;
	}
	dev_hold(ifp);
	rcu_read_unlock();

	if (!idr_pre_get(&ccieth_ep_idr, GFP_KERNEL)) {
		err = -ENOMEM;
		goto out_with_ifp;
	}
	spin_lock(&ccieth_ep_idr_lock);
	err = idr_get_new(&ccieth_ep_idr, ep, &id);
	spin_unlock(&ccieth_ep_idr_lock);
	if (err)
		goto out_with_ifp;

	ep = kmalloc(sizeof(struct ccieth_endpoint), GFP_KERNEL);
	if (!ep) {
		err = -ENOMEM;
		goto out_with_id;
	}
	ep->ifp = ifp;
	arg->id = ep->id = id;
	ep->recvq = NULL;

	return ep;

out_with_id:
	spin_lock(&ccieth_ep_idr_lock);
	idr_remove(&ccieth_ep_idr, id);
	spin_unlock(&ccieth_ep_idr_lock);
out_with_ifp:
	dev_put(ifp);
out:
	return ERR_PTR(err);
}

static int
ccieth_miscdev_open(struct inode * inode, struct file * file)
{
	file->private_data = NULL;
	return 0;
}

static int
ccieth_miscdev_release(struct inode * inode, struct file * file)
{
	struct ccieth_endpoint *ep = file->private_data;
	if (ep) {
		file->private_data = NULL;
		ccieth_destroy_endpoint(ep);
	}
	return 0;
}

static int
ccieth_miscdev_mmap(struct file * file, struct vm_area_struct * vma)
{
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
        unsigned long size = vma->vm_end - vma->vm_start;
	struct ccieth_endpoint *ep = file->private_data;
	void *buffer;
	int ret;

	if (!ep || offset != CCIETH_MMAP_RECVQ_OFFSET) {
                ret = -EINVAL;
                goto out;
        }
	if (vma->vm_flags & (VM_WRITE/*|VM_MAYWRITE*/)) {
		/* FIXME: VM_MAYWRITE automatically added if file is open RW, open RO instead? */
		ret = -EACCES;
		goto out;
	}

        buffer = vmalloc_user(size);
        if (!buffer) {
                ret = -ENOMEM;
                goto out;
        }

        ret = remap_vmalloc_range(vma, buffer, 0);
        if (ret < 0) {
                goto out_with_buffer;
        }

	/* FIXME: allow multiple mmap'ed buffers for recvq resizing */
	if (cmpxchg(&ep->recvq, NULL, buffer)) {
		ret = -EBUSY;
		goto out_with_buffer;
	}

	return 0;

out_with_buffer:
	vfree(buffer);
out:
	return ret;
}

static long
ccieth_miscdev_ioctl(struct file *file, unsigned cmd, unsigned long arg)
{
	int ret;

	switch (cmd) {
	case CCIETH_IOCTL_GET_INFO: {
		/* get a sockaddr_ll from userspace */
		struct ccieth_ioctl_get_info gi_arg;
		struct net_device *ifp;

		ret = copy_from_user(&gi_arg, (const __user void *) arg, sizeof(gi_arg));
		if (ret)
			return -EFAULT;

		gi_arg.max_send_size = -1;
		gi_arg.pci_domain = -1;
		gi_arg.pci_bus = -1;
		gi_arg.pci_dev = -1;
		gi_arg.pci_func = -1;
		gi_arg.rate = 0;

		rcu_read_lock();
		ifp = dev_getbyhwaddr_rcu(&init_net, ARPHRD_ETHER, (const char *) &gi_arg.addr);
		if (ifp) {
			struct device *dev = ifp->dev.parent;

			if (ifp->mtu == 9000)
				gi_arg.max_send_size = 8192;
			else if (ifp->mtu == 1500)
				gi_arg.max_send_size = 1024;

			if (dev && dev->bus == &pci_bus_type) {
				struct pci_dev *pdev = to_pci_dev(dev);
				gi_arg.pci_domain = pci_domain_nr(pdev->bus);
				gi_arg.pci_bus = pdev->bus->number;
				gi_arg.pci_dev = PCI_SLOT(pdev->devfn);
				gi_arg.pci_func = PCI_FUNC(pdev->devfn);
			}

			if (ifp->ethtool_ops && ifp->ethtool_ops->get_settings) {
				struct ethtool_cmd cmd;
				if (ifp->ethtool_ops->get_settings(ifp, &cmd) >= 0) {
					u32 speed = ethtool_cmd_speed(&cmd);
					if (speed != -1)
						gi_arg.rate = ((u64) speed) * 1000000;
				}
			}
		} else
			ret = -ENODEV;
		rcu_read_unlock();

		ret = copy_to_user((__user void *) arg, &gi_arg, sizeof(gi_arg));
		if (ret)
			return -EFAULT;


		return 0;
	}

	case CCIETH_IOCTL_CREATE_ENDPOINT: {
		struct ccieth_ioctl_create_endpoint ce_arg;
		struct ccieth_endpoint *ep, **epp;

		if (!(file->f_mode & FMODE_WRITE))
			return -EACCES;

		ret = copy_from_user(&ce_arg, (const __user void *) arg, sizeof(ce_arg));
		if (ret)
			return -EFAULT;

		ep = ccieth_create_endpoint(&ce_arg);
		if (IS_ERR(ep))
			return PTR_ERR(ep);

		epp = (struct ccieth_endpoint **) &file->private_data;
		if (cmpxchg(epp, NULL, ep)) {
			ccieth_destroy_endpoint(ep);
			return -EBUSY;
		}

		ret = copy_to_user((__user void *) arg, &ce_arg, sizeof(ce_arg));
		if (ret)
			return -EFAULT;

		return 0;
	}

	default:
		return -EINVAL;
	}
}

static struct file_operations
ccieth_miscdev_fops = {
	.owner = THIS_MODULE,
	.open = ccieth_miscdev_open,
	.release = ccieth_miscdev_release,
	.mmap = ccieth_miscdev_mmap,
        .unlocked_ioctl = ccieth_miscdev_ioctl,
};

static struct miscdevice
ccieth_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ccieth",
	.fops = &ccieth_miscdev_fops,
};

int
ccieth_init(void)
{
	int ret;

	idr_init(&ccieth_ep_idr);
	spin_lock_init(&ccieth_ep_idr_lock);

	ret = misc_register(&ccieth_miscdev);
	if (ret < 0)
		goto out;

	ccieth_recv_init();

	return 0;

out:
	return ret;
}

void
ccieth_exit(void)
{
	ccieth_recv_exit();
	misc_deregister(&ccieth_miscdev);
	idr_destroy(&ccieth_ep_idr);
}

module_init(ccieth_init);
module_exit(ccieth_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Brice Goglin <Brice.Goglin@inria.fr>");
MODULE_VERSION("0.0.1");
MODULE_DESCRIPTION("CCI over Ethernet");