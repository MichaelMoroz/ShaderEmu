// SPDX-License-Identifier: GPL-2.0
/*
 * ShaderEmu network device (docs/lan.md in the ShaderEmu repository): bare IP packets between
 * machines. Packets leave through a window of plain memory that the host reads, and arrive in
 * a ring the host fills; both are polled in the timer tick. There is no hardware address and
 * no ARP: the host gives the machine a number, and the address is 10.0.H.L of it.
 */
#include <linux/if_arp.h>
#include <linux/in.h>
#include <linux/inetdevice.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ip.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/timer.h>
#include <linux/rtnetlink.h>
#include <linux/workqueue.h>
#include <net/ip_fib.h>
#include <net/net_namespace.h>

#define NET_REGS	0x87000000UL
#define NET_GUEST	0x3e0	/* ours: packets sent, packets taken from the ring */
#define NET_HOST	0x3f0	/* the host's: packets it took, packets it delivered, our number */
#define NET_PHYS	0x876b8000UL
#define NET_SLOT	640	/* a packet's place: its length, its number, 8 bytes unused, 576 of data */
#define NET_RING	0x400	/* the ring's first slot, from NET_PHYS; the 640 bytes at 0 are what we send */
#define NET_SLOTS	8
#define NET_MTU		576
#define NET_NETWORK	0x0a000000	/* 10.0.0.0/16 */
#define NET_MASK	0xffff0000

static void __iomem *net_regs, *net_mem;
static struct net_device *net_dev;
static struct timer_list net_timer;
static struct work_struct net_work;
static u32 net_tail, net_id, net_fill;
static bool net_lo_up;

static netdev_tx_t shaderemu_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	u32 sent = readl(net_regs + NET_GUEST), need;
	void __iomem *place;

	if (skb->len > NET_MTU || skb_linearize(skb)) {
		dev->stats.tx_dropped++;
		dev_kfree_skb(skb);
		return NETDEV_TX_OK;
	}
	/*
	 * Packets go one after the other into the window, each on 16 bytes: from its start
	 * again once the host has taken all of them, and none while the next does not fit.
	 */
	need = 16 + ALIGN(skb->len, 16);
	if (readl(net_regs + NET_HOST) == sent)
		net_fill = 0;
	if (net_fill + need > NET_SLOT) {
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}
	place = net_mem + net_fill;
	memcpy_toio(place + 16, skb->data, skb->len);
	writel(skb->len, place);
	writel(sent + 1, place + 4);
	writel(sent + 1, net_regs + NET_GUEST);
	net_fill += need;
	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;
	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static int shaderemu_net_open(struct net_device *dev)
{
	/* what the ring holds from before is not ours to read */
	net_tail = readl(net_regs + NET_HOST + 4);
	writel(net_tail, net_regs + NET_GUEST + 4);
	netif_start_queue(dev);
	mod_timer(&net_timer, jiffies + 1);
	return 0;
}

static int shaderemu_net_stop(struct net_device *dev)
{
	netif_stop_queue(dev);
	return 0;
}

static const struct net_device_ops shaderemu_net_ops = {
	.ndo_open	= shaderemu_net_open,
	.ndo_stop	= shaderemu_net_stop,
	.ndo_start_xmit	= shaderemu_net_xmit,
};

static void shaderemu_net_setup(struct net_device *dev)
{
	dev->netdev_ops = &shaderemu_net_ops;
	dev->type = ARPHRD_NONE;
	dev->hard_header_len = 0;
	dev->addr_len = 0;
	dev->mtu = NET_MTU;
	dev->min_mtu = 68;
	dev->max_mtu = NET_MTU;
	dev->tx_queue_len = 32;
	dev->flags = IFF_NOARP | IFF_BROADCAST | IFF_MULTICAST;
	dev->needs_free_netdev = true;
}

/* The host's number for this machine has changed: 0 is no link, N is 10.0.(N >> 8).(N & 255). */
static void shaderemu_net_configure(struct work_struct *work)
{
	u32 id = readl(net_regs + NET_HOST + 8) & 0xffff;
	struct sockaddr_in *sin;
	struct ifreq ifr;

	if (!net_lo_up) {
		memset(&ifr, 0, sizeof(ifr));
		strcpy(ifr.ifr_name, "lo");
		ifr.ifr_flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
		devinet_ioctl(&init_net, SIOCSIFFLAGS, &ifr);
		net_lo_up = true;
	}
	if (id == net_id)
		return;
	net_id = id;
	memset(&ifr, 0, sizeof(ifr));
	strcpy(ifr.ifr_name, net_dev->name);
	sin = (struct sockaddr_in *)&ifr.ifr_addr;
	if (id) {
		sin->sin_family = AF_INET;
		sin->sin_addr.s_addr = htonl(NET_NETWORK | id);
		devinet_ioctl(&init_net, SIOCSIFADDR, &ifr);
		sin->sin_addr.s_addr = htonl(NET_MASK);
		devinet_ioctl(&init_net, SIOCSIFNETMASK, &ifr);
		memset(&ifr.ifr_addr, 0, sizeof(ifr.ifr_addr));
	}
	ifr.ifr_flags = net_dev->flags & ~IFF_UP;
	if (id)
		ifr.ifr_flags |= IFF_UP;
	devinet_ioctl(&init_net, SIOCSIFFLAGS, &ifr);
	if (id) {
		/* 255.255.255.255 goes out here too: programs that look for others on a "LAN" send there */
		struct fib_config everybody = {
			.fc_table = RT_TABLE_MAIN, .fc_protocol = RTPROT_KERNEL, .fc_scope = RT_SCOPE_LINK,
			.fc_type = RTN_UNICAST, .fc_dst = htonl(INADDR_BROADCAST), .fc_dst_len = 32,
			.fc_oif = net_dev->ifindex, .fc_nlflags = NLM_F_CREATE, .fc_nlinfo.nl_net = &init_net,
		};
		struct fib_table *table;

		rtnl_lock();
		table = fib_new_table(&init_net, RT_TABLE_MAIN);
		if (table)
			fib_table_insert(&init_net, table, &everybody, NULL);
		rtnl_unlock();
	}
	pr_info("shaderemu_net: %s is %s, 10.0.%u.%u\n", net_dev->name, id ? "up" : "down", id >> 8, id & 255);
}

static void shaderemu_net_poll(struct timer_list *t)
{
	struct net_device *dev = net_dev;
	u32 head;

	if ((readl(net_regs + NET_HOST + 8) & 0xffff) != net_id)
		schedule_work(&net_work);
	if (!netif_running(dev)) {
		mod_timer(&net_timer, jiffies + HZ / 4);
		return;
	}
	if (netif_queue_stopped(dev) && readl(net_regs + NET_HOST) == readl(net_regs + NET_GUEST))
		netif_wake_queue(dev);
	head = readl(net_regs + NET_HOST + 4);
	/* a host that started again counts from 0: there is nothing of its to read yet */
	if (head - net_tail > NET_SLOTS) {
		net_tail = head;
		writel(net_tail, net_regs + NET_GUEST + 4);
	}
	if (net_tail != head) {
		for (; net_tail != head; net_tail++) {
			void __iomem *slot = net_mem + NET_RING + NET_SLOT * (net_tail % NET_SLOTS);
			u32 len = readl(slot);
			struct sk_buff *skb;

			if (len < sizeof(struct iphdr) || len > NET_MTU || readl(slot + 4) != net_tail + 1) {
				dev->stats.rx_errors++;
				continue;
			}
			skb = netdev_alloc_skb(dev, len);
			if (!skb) {
				dev->stats.rx_dropped++;
				continue;
			}
			memcpy_fromio(skb_put(skb, len), slot + 16, len);
			skb_reset_mac_header(skb);
			skb->protocol = htons(ETH_P_IP);
			dev->stats.rx_packets++;
			dev->stats.rx_bytes += len;
			netif_rx(skb);
		}
		writel(net_tail, net_regs + NET_GUEST + 4);
	}
	mod_timer(&net_timer, jiffies + 1);
}

static int __init shaderemu_net_init(void)
{
	int ret;

	if (pfn_valid(NET_REGS >> PAGE_SHIFT))
		return -ENODEV;	/* this range is RAM here: not our device tree */
	net_regs = ioremap(NET_REGS, PAGE_SIZE);
	net_mem = ioremap(NET_PHYS, NET_RING + NET_SLOT * NET_SLOTS);
	net_dev = alloc_netdev(0, "lan%d", NET_NAME_UNKNOWN, shaderemu_net_setup);
	if (!net_regs || !net_mem || !net_dev)
		return -ENOMEM;
	ret = register_netdev(net_dev);
	if (ret)
		return ret;
	INIT_WORK(&net_work, shaderemu_net_configure);
	timer_setup(&net_timer, shaderemu_net_poll, 0);
	schedule_work(&net_work);
	mod_timer(&net_timer, jiffies + HZ / 4);
	return 0;
}
device_initcall(shaderemu_net_init);
