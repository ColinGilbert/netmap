/*
 * Copyright (C) 2013 Universita` di Pisa. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "bsd_glue.h"
#include <linux/file.h>   /* fget(int fd) */

#include <net/netmap.h>
#include <dev/netmap/netmap_kern.h>
#include <dev/netmap/netmap_mem2.h>
#include <net/netmap_user.h>


/* =========================== MITIGATION SUPPORT ============================= */

/*
 * The generic driver calls netmap once per received packet.
 * This is inefficient so we implement a mitigation mechanism,
 * as follows:
 * - the first packet on an idle receiver triggers a notification
 *   and starts a timer;
 * - subsequent incoming packets do not cause a notification
 *   until the timer expires;
 * - when the timer expires and there are pending packets,
 *   a notification is sent up and the timer is restarted.
 */
enum hrtimer_restart
generic_timer_handler(struct hrtimer *t)
{
    struct netmap_generic_adapter *gna =
	container_of(t, struct netmap_generic_adapter, mit_timer);
    struct netmap_adapter *na = (struct netmap_adapter *)gna;
    u_int work_done;

    if (!gna->mit_pending) {
        return HRTIMER_NORESTART;
    }

    /* Some work arrived while the timer was counting down:
     * Reset the pending work flag, restart the timer and send
     * a notification.
     */
    gna->mit_pending = 0;
    /* below is a variation of netmap_generic_irq */
    if (na->ifp->if_capenable & IFCAP_NETMAP)
        netmap_common_irq(na->ifp, 0, &work_done);
    // IFRATE(rate_ctx.new.rxirq++);
    netmap_mitigation_restart(gna);

    return HRTIMER_RESTART;
}


void netmap_mitigation_init(struct netmap_generic_adapter *gna)
{
    hrtimer_init(&gna->mit_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
    gna->mit_timer.function = &generic_timer_handler;
    gna->mit_pending = 0;
}


void netmap_mitigation_start(struct netmap_generic_adapter *gna)
{
    hrtimer_start(&gna->mit_timer, ktime_set(0, netmap_generic_mit), HRTIMER_MODE_REL);
}

void netmap_mitigation_restart(struct netmap_generic_adapter *gna)
{
    hrtimer_forward_now(&gna->mit_timer, ktime_set(0, netmap_generic_mit));
}

int netmap_mitigation_active(struct netmap_generic_adapter *gna)
{
    return hrtimer_active(&gna->mit_timer);
}

void netmap_mitigation_cleanup(struct netmap_generic_adapter *gna)
{
    hrtimer_cancel(&gna->mit_timer);
}



/* ========================= GENERIC ADAPTER SUPPORT =========================== */

/*
 * This handler is registered within the attached net_device
 * in the Linux RX subsystem, so that every mbuf passed up by
 * the driver can be stolen to the network stack.
 * Stolen packets are put in a queue where the
 * generic_netmap_rxsync() callback can extract them.
 */
rx_handler_result_t linux_generic_rx_handler(struct mbuf **pm)
{
    generic_rx_handler((*pm)->dev, *pm);

    return RX_HANDLER_CONSUMED;
}

/* Ask the Linux RX subsystem to intercept (or stop intercepting)
 * the packets incoming from the interface attached to 'na'.
 */
int
netmap_catch_rx(struct netmap_adapter *na, int intercept)
{
    struct ifnet *ifp = na->ifp;

    if (intercept) {
        return netdev_rx_handler_register(na->ifp,
                &linux_generic_rx_handler, na);
    } else {
        netdev_rx_handler_unregister(ifp);
        return 0;
    }
}

static u16 generic_ndo_select_queue(struct ifnet *ifp, struct mbuf *m)
{
    return skb_get_queue_mapping(m);
}

/* Must be called under rtnl. */
void netmap_catch_packet_steering(struct netmap_generic_adapter *gna, int enable)
{
    struct netmap_adapter *na = &gna->up.up;
    struct ifnet *ifp = na->ifp;

    if (enable) {
        /*
         * Save the old pointer to the netdev_op
         * create an updated netdev ops replacing the
         * ndo_select_queue function with our custom one,
         * and make the driver use it.
         */
        na->if_transmit = (void *)ifp->netdev_ops;
        gna->generic_ndo = *ifp->netdev_ops;  /* Copy */
        gna->generic_ndo.ndo_select_queue = &generic_ndo_select_queue;
        ifp->netdev_ops = &gna->generic_ndo;
    } else {
	/* Restore the original netdev_ops. */
        ifp->netdev_ops = (void *)na->if_transmit;
    }
}

/* Transmit routine used by generic_netmap_txsync(). Returns 0 on success
   and -1 on error (which may be packet drops or other errors). */
int generic_xmit_frame(struct ifnet *ifp, struct mbuf *m,
	void *addr, u_int len, u_int ring_nr)
{
    netdev_tx_t ret;

    /* Empty the sk_buff. */
    skb_trim(m, 0);

    /* TODO Support the slot flags (NS_FRAG, NS_INDIRECT). */
    skb_copy_to_linear_data(m, addr, len); // skb_store_bits(m, 0, addr, len);
    skb_put(m, len);
    NM_ATOMIC_INC(&m->users);
    m->dev = ifp;
    m->priority = 100;
    skb_set_queue_mapping(m, ring_nr);

    ret = dev_queue_xmit(m);

    if (likely(ret == NET_XMIT_SUCCESS)) {
        return 0;
    }
    if (unlikely(ret != NET_XMIT_DROP)) {
        /* If something goes wrong in the TX path, there is nothing intelligent
           we can do (for now) apart from error reporting. */
        RD(5, "dev_queue_xmit failed: HARD ERROR %d", ret);
    }
    return -1;
}

/* Use ethtool to find the current NIC rings lengths, so that the netmap rings can
   have the same lengths. */
int
generic_find_num_desc(struct ifnet *ifp, unsigned int *tx, unsigned int *rx)
{
    struct ethtool_ringparam rp;

    if (ifp->ethtool_ops && ifp->ethtool_ops->get_ringparam) {
        ifp->ethtool_ops->get_ringparam(ifp, &rp);
        *tx = rp.tx_pending;
        *rx = rp.rx_pending;
    }

    return 0;
}

/* Fills in the output arguments with the number of hardware TX/RX queues. */
void
generic_find_num_queues(struct ifnet *ifp, u_int *txq, u_int *rxq)
{
    *txq = ifp->real_num_tx_queues;
    *rxq = 1; /* TODO ifp->real_num_rx_queues */
}


/* =========================== SOCKET SUPPORT ============================ */

struct netmap_sock {
	struct sock sk;
	struct socket sock;
	struct socket_wq wq;
	void (*saved_nm_dtor)(struct netmap_adapter *);
	void *owner;
	struct netmap_adapter *na;
};

static int netmap_socket_sendmsg(struct kiocb *iocb, struct socket *sock,
                                 struct msghdr *m, size_t total_len)
{
    struct netmap_sock *nm_sock = container_of(sock, struct netmap_sock, sock);
    struct netmap_adapter *na = nm_sock->na;
    struct netmap_ring *ring;
    unsigned i, last;
    unsigned avail;
    unsigned j;
    unsigned nm_buf_size;
    struct iovec *iov = m->msg_iov;
    size_t iovcnt = m->msg_iovlen;
    unsigned slot_flags = NS_MOREFRAG | NS_VNET_HDR;

    ND("message_len %d, %p", (int)total_len, na_sock);

    if (unlikely(na == NULL)) {
        RD(5, "Null netmap adapter");
        return total_len;
    }

    /* Grab the netmap ring normally used from userspace. */
    ring = na->tx_rings[0].ring;
    nm_buf_size = ring->nr_buf_size;

    i = last = ring->cur;
    avail = ring->avail;
    ND("A) cur=%d avail=%d, hwcur=%d, hwavail=%d\n", i, avail, na->tx_rings[0].nr_hwcur,
                                                               na->tx_rings[0].nr_hwavail);
    if (avail < iovcnt) {
        /* Not enough netmap slots. */
        return 0;
    }

    for (j=0; j<iovcnt; j++) {
        uint8_t *iov_frag = iov[j].iov_base;
        unsigned iov_frag_size = iov[j].iov_len;
        unsigned offset = 0;
#if 0
        unsigned k = 0;
        uint8_t ch;

        printk("len=%d: ", iov_frag_size);
        for (k=0; k<iov_frag_size && k<36; k++) {
            if(copy_from_user(&ch, iov_frag + k, 1)) {
                D("failed");
            }
            printk("%02x:", ch);
        }printk("\n");
#endif
        while (iov_frag_size) {
            unsigned nm_frag_size = min(iov_frag_size, nm_buf_size);
            uint8_t *dst;

            if (unlikely(avail == 0)) {
                return 0;
            }

            dst = BDG_NMB(na, &ring->slot[i]);

            ring->slot[i].len = nm_frag_size;
            ring->slot[i].flags = slot_flags;
            slot_flags &= ~NS_VNET_HDR;
            if (copy_from_user(dst, iov_frag + offset, nm_frag_size)) {
                D("copy_from_user() error");
            }

            last = i;
            i = NETMAP_RING_NEXT(ring, i);
            avail--;

            offset += nm_frag_size;
            iov_frag_size -= nm_frag_size;
        }
    }

    ring->slot[last].flags &= ~NS_MOREFRAG;

    ring->cur = i;
    ring->avail = avail;

    na->nm_txsync(na, 0, 0);
    ND("B) cur=%d avail=%d, hwcur=%d, hwavail=%d\n", i, avail, na->tx_rings[0].nr_hwcur,
                                                               na->tx_rings[0].nr_hwavail);

    return total_len;
}

static int netmap_socket_recvmsg(struct kiocb *iocb, struct socket *sock,
		struct msghdr *m, size_t total_len, int flags)
{
	struct netmap_sock *nm_sock = container_of(sock, struct netmap_sock, sock);
	struct netmap_adapter *na = nm_sock->na;
	struct netmap_ring *ring;
	/* netmap variables */
	unsigned i, avail;
	bool morefrag;
	unsigned nm_frag_size;
	unsigned nm_frag_ofs;
	uint8_t *src;
	/* iovec variables */
	unsigned j;
	struct iovec *iov = m->msg_iov;
	size_t iovcnt = m->msg_iovlen;
	uint8_t *dst;
	unsigned iov_frag_size;
	unsigned iov_frag_ofs;
	/* counters */
	unsigned copy_size;
	unsigned copied;

	/* The caller asks for 'total_len' bytes. */
	ND("recvmsg %d, %p", (int)total_len, nm_sock);

	if (unlikely(na == NULL)) {
		RD(5, "Null netmap adapter");
		return total_len;
	}

	/* Total bytes actually copied. */
	copied = 0;

	/* Grab the netmap RX ring normally used from userspace. */
	ring = na->rx_rings[0].ring;
	i = ring->cur;
	avail = ring->avail;

	/* Index into the input iovec[]. */
	j = 0;

	/* Spurious call: Do nothing. */
	if (avail == 0)
		return 0;

	/* init netmap variables */
	morefrag = (ring->slot[i].flags & NS_MOREFRAG);
	nm_frag_ofs = 0;
	nm_frag_size = ring->slot[i].len;
	src = BDG_NMB(na, &ring->slot[i]);

	/* init iovec variables */
	iov_frag_ofs = 0;
	iov_frag_size = iov[j].iov_len;
	dst = iov[j].iov_base;

	/* Copy from the netmap scatter-gather to the caller
	 * scatter-gather.
	 */
	while (copied < total_len) {
		copy_size = min(nm_frag_size, iov_frag_size);
		copy_to_user(dst + iov_frag_ofs, src + nm_frag_ofs,
			     copy_size);
		nm_frag_ofs += copy_size;
		nm_frag_size -= copy_size;
		iov_frag_ofs += copy_size;
		iov_frag_size -= copy_size;
		copied += copy_size;
		if (nm_frag_size == 0) {
			/* Netmap slot exhausted. If this was the
			 * last slot, or no more slots ar available,
			 * we've done.
			 */
			if (!morefrag || !avail)
				break;
			/* Take the next slot. */
			i = NETMAP_RING_NEXT(ring, i);
			avail--;
			morefrag = (ring->slot[i].flags & NS_MOREFRAG);
			nm_frag_ofs = 0;
			nm_frag_size = ring->slot[i].len;
			src = BDG_NMB(na, &ring->slot[i]);
		}
		if (iov_frag_size == 0) {
			/* The current iovec fragment is exhausted.
			 * Since we enter here, there must be more
			 * to read from the netmap slots (otherwise
			 * we would have exited the loop in the
			 * above branch).
			 * If this was the last fragment, it means
			 * that there is not enough space in the input
			 * iovec[].
			 */
			j++;
			if (unlikely(j >= iovcnt)) {
				break;
			}
			/* Take the next iovec fragment. */
			iov_frag_ofs = 0;
			iov_frag_size = iov[j].iov_len;
			dst = iov[j].iov_base;
		}
	}

	if (unlikely(!avail && morefrag)) {
		RD(5, "Error: ran out of slots, with a pending"
				"incomplete packet\n");
	}

	ring->cur = i;
	ring->avail = avail;

	D("read %d bytes using %d iovecs", copied, j);

	return copied;
}

static struct proto netmap_socket_proto = {
        .name = "netmap",
        .owner = THIS_MODULE,
        .obj_size = sizeof(struct netmap_sock),
};

static struct proto_ops netmap_socket_ops = {
        .sendmsg = netmap_socket_sendmsg,
        .recvmsg = netmap_socket_recvmsg,
};

static void netmap_sock_write_space(struct sock *sk)
{
    wait_queue_head_t *wqueue;

    if (!sock_writeable(sk) ||
        !test_and_clear_bit(SOCK_ASYNC_NOSPACE, &sk->sk_socket->flags)) {
            return;
    }

    wqueue = sk_sleep(sk);
    if (wqueue && waitqueue_active(wqueue)) {
        wake_up_interruptible_poll(wqueue, POLLOUT | POLLWRNORM | POLLWRBAND);
    }
}

static void netmap_sock_teardown(struct netmap_adapter *na)
{
	struct netmap_sock *nm_sock = na->na_private;

    if (nm_sock) {
	/* Restore the saved destructor. */
	na->nm_dtor = nm_sock->saved_nm_dtor;

	/* Drain the receive queue, which sould contain
	   the fake skb only. */
	skb_queue_purge(&nm_sock->sk.sk_receive_queue);

        sock_put(&nm_sock->sk);
        /* XXX What?
           kfree(nm_sock);
           sk_release_kernel(&nm_sock->sk);
           */
        sk_free(&nm_sock->sk);
        na->na_private = NULL;
        D("socket support freed for (%p)", na);
    }
}

static void netmap_socket_nm_dtor(struct netmap_adapter *na)
{
	netmap_sock_teardown(na);
	/* Call the saved destructor, if any. */
	if (na->nm_dtor)
		na->nm_dtor(na);
}

static struct netmap_sock *netmap_sock_setup(struct netmap_adapter *na, struct file *filp)
{
        struct netmap_sock *nm_sock;
	struct sk_buff *skb;

        na->na_private = nm_sock = (struct netmap_sock *)sk_alloc(&init_net, AF_UNSPEC,
                                                        GFP_KERNEL, &netmap_socket_proto);
        if (!nm_sock) {
            return NULL;
        }

	nm_sock->sock.wq = &nm_sock->wq;   /* XXX rcu? */
        init_waitqueue_head(&nm_sock->wq.wait);
        nm_sock->sock.file = filp;
        nm_sock->sock.ops = &netmap_socket_ops;
        sock_init_data(&nm_sock->sock, &nm_sock->sk);
        nm_sock->sk.sk_write_space = &netmap_sock_write_space;

	/* Create a fake skb. */
	skb = alloc_skb(1800, GFP_ATOMIC);
	if (!skb) {
		D("fake skbuff allocation failed");
		sk_free(&nm_sock->sk);
		na->na_private = NULL;

		return NULL;
	}
	skb_queue_tail(&nm_sock->sk.sk_receive_queue, skb);

        sock_hold(&nm_sock->sk);

        /* Set the backpointer to the netmap_adapter parent structure. */
        nm_sock->na = na;

	nm_sock->owner = current;

	nm_sock->saved_nm_dtor = na->nm_dtor;
	na->nm_dtor = &netmap_socket_nm_dtor;

        D("socket support OK for (%p)", na);

        return nm_sock;
}


/* ========================= FILE OPERATIONS SUPPORT =========================== */

struct net_device *
ifunit_ref(const char *name)
{
	return dev_get_by_name(&init_net, name);
}

void if_rele(struct net_device *ifp)
{
	dev_put(ifp);
}



/*
 * Remap linux arguments into the FreeBSD call.
 * - pwait is the poll table, passed as 'dev';
 *   If pwait == NULL someone else already woke up before. We can report
 *   events but they are filtered upstream.
 *   If pwait != NULL, then pwait->key contains the list of events.
 * - events is computed from pwait as above.
 * - file is passed as 'td';
 */
static u_int
linux_netmap_poll(struct file * file, struct poll_table_struct *pwait)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,28)
	int events = POLLIN | POLLOUT; /* XXX maybe... */
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3,4,0)
	int events = pwait ? pwait->key : POLLIN | POLLOUT | POLLERR;
#else /* in 3.4.0 field 'key' was renamed to '_key' */
	int events = pwait ? pwait->_key : POLLIN | POLLOUT | POLLERR;
#endif
	return netmap_poll((void *)pwait, events, (void *)file);
}


static int
linux_netmap_mmap(struct file *f, struct vm_area_struct *vma)
{
	int error = 0;
	unsigned long off, va;
	vm_ooffset_t pa;
	struct netmap_priv_d *priv = f->private_data;
	/*
	 * vma->vm_start: start of mapping user address space
	 * vma->vm_end: end of the mapping user address space
	 * vma->vm_pfoff: offset of first page in the device
	 */

	// XXX security checks

	error = netmap_get_memory(priv);
	ND("get_memory returned %d", error);
	if (error)
	    return -error;

	if ((vma->vm_start & ~PAGE_MASK) || (vma->vm_end & ~PAGE_MASK)) {
		ND("vm_start = %lx vm_end = %lx", vma->vm_start, vma->vm_end);
		return -EINVAL;
	}

	for (va = vma->vm_start, off = vma->vm_pgoff;
	     va < vma->vm_end;
	     va += PAGE_SIZE, off++)
	{
		pa = netmap_mem_ofstophys(priv->np_mref, off << PAGE_SHIFT);
		if (pa == 0) 
			return -EINVAL;
	
		ND("va %lx pa %p", va, pa);	
		error = remap_pfn_range(vma, va, pa >> PAGE_SHIFT, PAGE_SIZE, vma->vm_page_prot);
		if (error) 
			return error;
	}
	return 0;
}


/*
 * This one is probably already protected by the netif lock XXX
 */
netdev_tx_t
linux_netmap_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	netmap_transmit(dev, skb);
	return (NETDEV_TX_OK);
}


#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,37)	// XXX was 38
#define LIN_IOCTL_NAME	.ioctl
int
linux_netmap_ioctl(struct inode *inode, struct file *file, u_int cmd, u_long data /* arg */)
#else
#define LIN_IOCTL_NAME	.unlocked_ioctl
long
linux_netmap_ioctl(struct file *file, u_int cmd, u_long data /* arg */)
#endif
{
	int ret;
	struct nmreq nmr;
	bzero(&nmr, sizeof(nmr));

        if (cmd == NIOCTXSYNC || cmd == NIOCRXSYNC) {
            data = 0;       /* no argument required here */
        }
	if (data && copy_from_user(&nmr, (void *)data, sizeof(nmr) ) != 0)
		return -EFAULT;
	ret = netmap_ioctl(NULL, cmd, (caddr_t)&nmr, 0, (void *)file);
	if (data && copy_to_user((void*)data, &nmr, sizeof(nmr) ) != 0)
		return -EFAULT;
	return -ret;
}


static int
linux_netmap_release(struct inode *inode, struct file *file)
{
	(void)inode;	/* UNUSED */
	if (file->private_data)
		netmap_dtor(file->private_data);
	return (0);
}


static int
linux_netmap_open(struct inode *inode, struct file *file)
{
	struct netmap_priv_d *priv;
	(void)inode;	/* UNUSED */

	priv = malloc(sizeof(struct netmap_priv_d), M_DEVBUF,
			      M_NOWAIT | M_ZERO);
	if (priv == NULL)
		return -ENOMEM;

	file->private_data = priv;

	return (0);
}


static struct file_operations netmap_fops = {
    .owner = THIS_MODULE,
    .open = linux_netmap_open,
    .mmap = linux_netmap_mmap,
    LIN_IOCTL_NAME = linux_netmap_ioctl,
    .poll = linux_netmap_poll,
    .release = linux_netmap_release,
};


struct socket *get_netmap_socket(int fd)
{
	struct file *filp = fget(fd);
	struct netmap_priv_d *priv;
	struct netmap_adapter *na;
	struct netmap_sock *nm_sock;

	if (!filp)
		return ERR_PTR(EBADF);

	if (filp->f_op != &netmap_fops)
		return ERR_PTR(EINVAL);

	priv = (struct netmap_priv_d *)filp->private_data;
	if (!priv)
		return ERR_PTR(EBADF);

	NMG_LOCK();
	na = priv->np_na;
	if (na == NULL) {
		NMG_UNLOCK();
		return ERR_PTR(EBADF);
	}

	nm_sock = (struct netmap_sock *)(na->na_private);

	if (NETMAP_OWNED_BY_KERN(na) && (!nm_sock || nm_sock->owner != current)) {
		NMG_UNLOCK();
		return ERR_PTR(EBUSY);
	}

	if (!nm_sock)
		nm_sock = netmap_sock_setup(na, filp);
	NMG_UNLOCK();

	ND("na_private %p, nm_sock %p", na->na_private, nm_sock);

	/* netmap_sock_setup() may fail because of OOM */
	if (!nm_sock)
		return ERR_PTR(ENOMEM);

	return &nm_sock->sock;
}
EXPORT_SYMBOL(get_netmap_socket);


struct miscdevice netmap_cdevsw = { /* same name as FreeBSD */
	MISC_DYNAMIC_MINOR,
	"netmap",
	&netmap_fops,
};


static int linux_netmap_init(void)
{
        /* Errors have negative values on linux. */
	return -netmap_init();
}


static void linux_netmap_fini(void)
{
        netmap_fini();
}


module_init(linux_netmap_init);
module_exit(linux_netmap_fini);

/* export certain symbols to other modules */
EXPORT_SYMBOL(netmap_attach);		/* driver attach routines */
EXPORT_SYMBOL(netmap_detach);		/* driver detach routines */
EXPORT_SYMBOL(netmap_ring_reinit);	/* ring init on error */
EXPORT_SYMBOL(netmap_buffer_lut);
EXPORT_SYMBOL(netmap_total_buffers);	/* index check */
EXPORT_SYMBOL(netmap_buffer_base);
EXPORT_SYMBOL(netmap_reset);		/* ring init routines */
EXPORT_SYMBOL(netmap_buf_size);
EXPORT_SYMBOL(netmap_rx_irq);	        /* default irq handler */
EXPORT_SYMBOL(netmap_no_pendintr);	/* XXX mitigation - should go away */
#ifdef WITH_VALE
EXPORT_SYMBOL(netmap_bdg_ctl);		/* bridge configuration routine */
EXPORT_SYMBOL(netmap_bdg_learning);	/* the default lookup function */
#endif /* WITH_VALE */
EXPORT_SYMBOL(netmap_disable_all_rings);
EXPORT_SYMBOL(netmap_enable_all_rings);
EXPORT_SYMBOL(netmap_krings_create);


MODULE_AUTHOR("http://info.iet.unipi.it/~luigi/netmap/");
MODULE_DESCRIPTION("The netmap packet I/O framework");
MODULE_LICENSE("Dual BSD/GPL"); /* the code here is all BSD. */
