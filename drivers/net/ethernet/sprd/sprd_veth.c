/*
 * Copyright (C) 2012 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/wakelock.h>
#include <linux/workqueue.h>
#include <linux/ipv6.h>
#include <linux/ip.h>
#include <asm/byteorder.h>
#include <linux/tty.h>
#include <linux/if_ether.h>
#include <linux/platform_device.h>
#include <linux/sprdmux.h>

/* Log Macros */
#define VETH_INFO(fmt...)	pr_err("VETH: " fmt)
#define VETH_DEBUG(fmt...)	pr_err("VETH: " fmt)
#define VETH_ERR(fmt...)	pr_err("VETH: Error: " fmt)

#define VETH_IPV4_HLEN 20
#define VETH_IPV6_HLEN 40
#define VETH_ETH_HLEN  ETH_HLEN

#define VETH_MUX_TYPE_LEN 64
#define VETH_MUX_TYPE_NUM 2
#define VETH_DEV_NUM 3
#define VETH_MUX_START_INDEX 12

#define VETH_IPV4_VERSION 0x04
#define VETH_IPV6_VERSION 0x06

#define VETH_DEV_STATUS_ON 	1
#define VETH_DEV_STATUS_OFF 	0

enum {
	VETH_STATE_COMPLETE_PACKET = 0,
	VETH_STATE_INCOMPLETE_DATA,
	VETH_STATE_INCOMPLETE_HEADER,
};

enum {
	MUX_EVENT_RECV_DATA = SPRDMUX_EVENT_COMPLETE_READ,
	MUX_EVENT_SENT_DATA = SPRDMUX_EVENT_COMPLETE_WRITE,
	MUX_EVENT_TTY_OPEN,
	MUX_EVENT_TTY_CLOSE,
};

/* virtual ethernet packet header information */
struct veth_header_info {
	struct ethhdr 	eth_hdr;
	union ip_header{
		struct iphdr 	ipv4_hdr;
		struct ipv6hdr 	ipv6_hdr;
	} ip_hdr;
};

/* virtual ethernet received packet data structure*/
struct veth_rx_packet {
	uint8_t  	proto; 	/* IP type */
	uint32_t 	plen; 	/* total length of packet */
	uint32_t 	pdlen; 	/* length of past reveived data */
	int 		state; 	/* veth state */
	int 		error; 	/* invalid packet */
	struct sk_buff 	*skb; 	/* pointer of socket buffer */
	struct veth_header_info hdrinfo; /* packet header info */
};

/* virtual ethernet device data struction */
struct veth_device {
	struct veth_init_data *pdata;
	struct net_device 	*netdev;
	struct veth_rx_packet 	*rxpkt;
	int 	status;
	struct net_device_stats stats;
};

struct veth_init_data {
	int index;
	int inst_id;
	char *name;
};


static int veth_prepare_skb(struct veth_device *veth, uint8_t *data, uint32_t len)
{
	struct sk_buff *skb = NULL;
	struct veth_rx_packet *vpkt = NULL;
	uint32_t size;
	uint8_t *ptr = NULL;

	if (!veth)
	{
		VETH_ERR("veth_prepare_skb: veth is not ready");
		return -ENODEV;
	}

	vpkt = veth->rxpkt;

	/* lost the front part of the current rx packet, so dropped the whole packet. */
	if (vpkt->error) {
		VETH_ERR("dropped the whole packet\n ");
		return -EINVAL;
	}


	VETH_DEBUG("%s rx packet info: state=%d, proto=%u, plen=%u, pdlen=%u, skb=0x%0x\n",
		veth->pdata->name, vpkt->state, vpkt->proto,
		vpkt->plen, vpkt->pdlen, vpkt->skb);

	size = vpkt->plen;
	skb = vpkt->skb;

	if (!skb) {
		skb = dev_alloc_skb(size + NET_IP_ALIGN);
		if (!skb) {
			veth->stats.rx_dropped++;
			vpkt->error = 1;
			VETH_ERR("failed to dev_alloc_skb\n");
			return -ENOMEM;
		}
		skb_reserve(skb, NET_IP_ALIGN);
		skb->dev = veth->netdev;
		vpkt->skb = skb;
	}

	VETH_DEBUG("%s prepared sk buff: skb=0x%x, plen=%u, data=0x%x, len=%u\n",
		veth->pdata->name, (uint32_t)skb, size, (uint32_t)data, len);
	ptr = skb_put(skb, len);
	memcpy(ptr, data, len);

	return 0;
}

static int veth_rx_data(struct veth_device *veth, uint8_t *data, uint32_t len)
{
	struct veth_rx_packet *vpkt = NULL;
	struct sk_buff *skb = NULL;
	int rval;

	if (!veth)
	{
		VETH_ERR("veth is NULL\n");
		return -ENODEV;
	}

	if (!data || len == 0)
	{
		VETH_ERR("reveice data is invalid, data 0x%0x, len %d\n", data, len);
		return -EINVAL;
	}

	vpkt = veth->rxpkt;

	rval = veth_prepare_skb(veth, data, len);
	if (rval) {
		VETH_ERR("failed to receive data\n");
		return rval;
	}

	skb = vpkt->skb;
	if (!skb)
	{
		VETH_ERR("skb is NULL\n");
		return -ENOMEM;
	}

	skb->protocol = eth_type_trans(skb, veth->netdev);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	veth->stats.rx_bytes += skb->len;
	veth->stats.rx_packets++;

	netif_rx(skb);

	return 0;
}

static int veth_perform_packet(struct veth_device *veth, uint8_t *data, uint32_t len)
{
	struct veth_rx_packet *vpkt = NULL;
	uint32_t left = len, offset = 0;
	uint32_t pnum, rval;
	uint8_t ver;
	uint8_t *hdr = NULL;

	if (!veth)
	{
		VETH_ERR("veth_perform_packet: veth is not ready\n");
		return -ENODEV;
	}

	vpkt = veth->rxpkt;

	while (left) {
		VETH_DEBUG("%s perform packet, data=0x%x, offset=%u, left=%u, len=%u\n",
			veth->pdata->name, (uint32_t)data, offset, left, len);
		VETH_DEBUG("%s rx packet info: state=%d, proto=%u, plen=%u, pdlen=%u, hdr=0x%x\n",
			veth->pdata->name, veth->rxpkt->state, veth->rxpkt->proto,
			veth->rxpkt->plen, veth->rxpkt->pdlen, (uint32_t)&veth->rxpkt->hdrinfo);
		pnum = 0;
		switch (vpkt->state) {
			case VETH_STATE_COMPLETE_PACKET:
				/* left < HLEN */
				if (left <= VETH_ETH_HLEN) {
					pnum = left;
					vpkt->pdlen = pnum;
					memcpy((uint8_t *)(&vpkt->hdrinfo), data + offset, pnum);
					vpkt->state = VETH_STATE_INCOMPLETE_HEADER;
					return 0;
				}
				/* abstract ip version */
				ver = (data[offset + VETH_ETH_HLEN] & 0xf0) >> 4;
				if (ver == VETH_IPV4_VERSION) {
					vpkt->proto = ver;
					/* incomplete ipv4 header  */
					if (left < VETH_ETH_HLEN + VETH_IPV4_HLEN)
					{
						pnum = left;
						vpkt->pdlen = pnum;
						memcpy((uint8_t*)(&vpkt->hdrinfo),
							data + offset, pnum);
						vpkt->state = VETH_STATE_INCOMPLETE_HEADER;
						return 0;
					}

					hdr = data + offset + VETH_ETH_HLEN;
					vpkt->plen = ntohs(((struct iphdr*)hdr)->tot_len) +
						VETH_ETH_HLEN;

				} else if (ver == VETH_IPV6_VERSION) {
					vpkt->proto = ver;
					/* incomplete ipv6 header */
					if (left < VETH_ETH_HLEN + VETH_IPV6_HLEN) {
						pnum = left;
						vpkt->pdlen = pnum;
						memcpy((uint8_t*)(&vpkt->hdrinfo),
							data + offset, pnum);
						vpkt->state = VETH_STATE_INCOMPLETE_HEADER;
						return 0;
					}

					hdr = data + offset + VETH_ETH_HLEN;
					vpkt->plen = ntohs(((struct ipv6hdr *)hdr)->payload_len) +
						VETH_IPV6_HLEN + VETH_ETH_HLEN;
				} else {
					return -EINVAL;
				}

				if (vpkt->plen > left) {
					/* HLEN < left < PLEN  */
					rval = veth_prepare_skb(veth, data + offset, left);
					vpkt->pdlen = left;
					pnum = left;
					vpkt->state = VETH_STATE_INCOMPLETE_DATA;
					return 0;
				}

				/* left >= PLEN */
				pnum = vpkt->plen;
				rval = veth_rx_data(veth, data + offset, pnum);
				memset((uint8_t *)vpkt, 0, sizeof(struct veth_rx_packet));
				vpkt->state = VETH_STATE_COMPLETE_PACKET;

				break;
			case VETH_STATE_INCOMPLETE_DATA:
				if (vpkt->pdlen + left < vpkt->plen) {
					/* left + PDLEN < PLEN*/
					pnum = left;
					rval = veth_prepare_skb(veth, data + offset, pnum);
					vpkt->pdlen += pnum;
					return 0;
				}

				/* left + PDLEN >= PLEN */
				pnum = vpkt->plen - vpkt->pdlen;
				rval = veth_rx_data(veth, data + offset, pnum);
				memset((uint8_t *)vpkt, 0, sizeof(struct veth_rx_packet));
				vpkt->state = VETH_STATE_COMPLETE_PACKET;

				break;
			case VETH_STATE_INCOMPLETE_HEADER:
				/* incomplete IP header */
				if (vpkt->pdlen <= VETH_ETH_HLEN) {
					if (vpkt->pdlen + left <= VETH_ETH_HLEN) {
						pnum = left;
						memcpy((uint8_t *)(&vpkt->hdrinfo),
								data + offset, pnum);
						vpkt->pdlen += pnum;
						return 0;
					}

					hdr = data + offset + VETH_ETH_HLEN - vpkt->pdlen;
					ver = (hdr[0] & 0xf0) >> 4;
				} else {
					ver = vpkt->proto;
				}

				if (ver == VETH_IPV4_VERSION) {
					vpkt->proto = ver;
					if (left + vpkt->pdlen < VETH_ETH_HLEN + VETH_IPV4_HLEN) {
						pnum = left;
						memcpy((uint8_t *)(&vpkt->hdrinfo), data + offset,
							pnum);
						vpkt->pdlen += pnum;
						return 0;
					}
					pnum = VETH_ETH_HLEN + VETH_IPV4_HLEN - vpkt->pdlen;
					memcpy((uint8_t *)(&vpkt->hdrinfo), data + offset, pnum);
					hdr = (uint8_t *)(&(vpkt->hdrinfo.ip_hdr.ipv4_hdr));
					vpkt->plen = ntohs(((struct iphdr *)hdr)->tot_len) +
						VETH_ETH_HLEN;
				} else if (ver == VETH_IPV6_VERSION) {
					vpkt->proto = ver;
					if (left +vpkt->pdlen < VETH_ETH_HLEN + VETH_IPV6_HLEN) {
						pnum = left;
						memcpy((uint8_t *)(&vpkt->hdrinfo), data + offset,
							pnum);
						vpkt->pdlen += pnum;
						return 0;
					}
					pnum = VETH_ETH_HLEN + VETH_IPV6_HLEN - vpkt->pdlen;
					memcpy((uint8_t *)(&vpkt->hdrinfo), data + offset, pnum);
					hdr = (uint8_t *)(&(vpkt->hdrinfo.ip_hdr.ipv6_hdr));
					vpkt->plen = ntohs(((struct ipv6hdr *)hdr)->payload_len) +
						VETH_IPV6_HLEN + VETH_ETH_HLEN;
				} else {
					return -EINVAL;
				}

				vpkt->pdlen += pnum;
				vpkt->state = VETH_STATE_INCOMPLETE_DATA;
				rval = veth_prepare_skb(veth, (uint8_t *)(&vpkt->hdrinfo),
						vpkt->pdlen);
				break;
			default:
				VETH_ERR("invalid rx packet state\n");
				vpkt->state = VETH_STATE_COMPLETE_PACKET;
				break;
		}
		left -= pnum;
		offset += pnum;
		VETH_DEBUG("%s perform packet: left=%u, offset=%u, pnum=%u\n",
			veth->pdata->name, left, offset, pnum);
		VETH_DEBUG("%s rx packet info: state=%d, proto=%u ,plen=%u, pdlen=%u, hdr=0x%x\n",
			veth->pdata->name, veth->rxpkt->state, veth->rxpkt->proto,
			veth->rxpkt->plen, veth->rxpkt->pdlen, (uint32_t)&veth->rxpkt->hdrinfo);

	}
	return 0;
}

static int veth_resume(struct veth_device *veth)
{
	if (!veth)
	{
		VETH_ERR("veth_resume, veth is NULL\n");
		return -ENODEV;
	}
	netif_wake_queue(veth->netdev);
	return 0;
}

static int veth_tty_open(struct veth_device *veth)
{
	if (!veth)
	{
		VETH_ERR("veth_tty_open, veth is NULL\n");
		return -ENODEV;
	}

	if (veth->status == VETH_DEV_STATUS_OFF) {
		veth->status = VETH_DEV_STATUS_ON;
	}
	if (!netif_carrier_ok(veth->netdev)) {
		netif_carrier_on(veth->netdev);
	}
	netif_wake_queue(veth->netdev);
	return 0;
}

static int veth_tty_close(struct veth_device *veth)
{
	if (!veth)
	{
		VETH_ERR("veth_tty_close, veth is NULL\n");
		return -ENODEV;
	}

	if (veth->status == VETH_DEV_STATUS_ON){
		veth->status = VETH_DEV_STATUS_OFF;
	}
	if (netif_carrier_ok(veth->netdev)) {
		netif_carrier_off(veth->netdev);
	}
	netif_stop_queue(veth->netdev);
	return 0;
}

static int veth_notify_handler(int index, int event, uint8_t * data, uint32_t len, void *priv_data)
{
	struct veth_device *veth = (struct veth_device *)priv_data;
	VETH_DEBUG("veth_notify_handler event %d\n", event);
	switch(event) {
		case MUX_EVENT_RECV_DATA:
			VETH_DEBUG("%s handle receive data, veth=0x%x, data=0x%x, len=%u\n",
				veth->pdata->name, (uint32_t)veth, (uint32_t)data, len);
			veth_perform_packet(veth, data, len);
			break;
		case MUX_EVENT_SENT_DATA:
			VETH_DEBUG("%s handle sent data\n", veth->pdata->name);
			veth_resume(veth);
			break;
		case MUX_EVENT_TTY_OPEN:
			veth_tty_open(veth);
			break;
		case MUX_EVENT_TTY_CLOSE:
			veth_tty_close(veth);
			break;
		default:
			break;
	}
	return 0;
}

static int veth_ndo_open (struct net_device *dev)
{
	struct veth_device *veth = netdev_priv(dev);
	struct veth_init_data *pdata = veth->pdata;
	int ret, inst_id, index;
	struct sprdmux_notify notify;

	inst_id = pdata->inst_id;
	index = pdata->index;

	/* Reset stats */
	memset(&veth->stats, 0, sizeof(veth->stats));
	memset(veth->rxpkt, 0, sizeof(struct veth_rx_packet));
	veth->rxpkt->state = VETH_STATE_COMPLETE_PACKET;

	VETH_DEBUG("%s open sprdmux tty-%d-%d\n", dev->name, inst_id, index);
	ret = sprdmux_open(inst_id, index);
	if (ret) {
		VETH_ERR("%s failed to open mux tty-%d-%d\n", dev->name, inst_id, index);
		return -ENODEV;
	}

	VETH_DEBUG("%s register notifier, tty-%d-%d, callback-0x%x, pdata-0x%x\n", dev->name,
		inst_id, index,	(uint32_t)veth_notify_handler, (uint32_t)veth);
	notify.index = index;
	notify.func = veth_notify_handler;
	notify.user_data = (void *)veth;
	ret = sprdmux_register_notify_callback(inst_id, &notify);
	if (ret) {
		VETH_ERR("failed to regitster notify callback, tty-%d-%d", inst_id, index);
		return -EINVAL;
	}

	VETH_DEBUG("%s update veth status\n", dev->name);
	veth->status = VETH_DEV_STATUS_ON;
	if (!netif_carrier_ok(veth->netdev)) {
		netif_carrier_on(veth->netdev);
	}
	//veth_dev->netdev->flags &= ~IFF_MULTICAST;
	//veth_dev->netdev->flags &= ~IFF_BROADCAST;

	netif_start_queue(dev);
	VETH_INFO("%s is started\n", dev->name);

	return 0;
}

static int veth_ndo_stop (struct net_device *dev)
{
	struct veth_device *veth = netdev_priv(dev);
	struct veth_init_data *pdata = veth->pdata;
	int inst_id, index;
	struct sprdmux_notify notify;

	inst_id = pdata->inst_id;
	index = pdata->index;

	netif_stop_queue(dev);
	if (netif_carrier_ok(dev)) {
		netif_carrier_off(dev);
	}

	VETH_DEBUG("%s close sprdmux tty-%d-%d\n", dev->name, inst_id, index);
	sprdmux_close(inst_id, index);

	veth->status = VETH_DEV_STATUS_OFF;
	notify.index = index;
	notify.func = NULL;
	notify.user_data = NULL;
	sprdmux_register_notify_callback(inst_id, &notify);
	VETH_INFO("%s is stopped!\n", dev->name);

	return 0;
}

static int veth_ndo_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct veth_device *veth = netdev_priv(dev);
	struct veth_init_data *pdata = veth->pdata;
	int ret;

	VETH_DEBUG("%s is ready to send %d byte, proto %x\n",
		dev->name, skb->len,
		(skb->data[VETH_ETH_HLEN] & 0xf0) >> 4);

	/* notify the uplayer to stop sending */
	netif_stop_queue(dev);

	/* invoke mux write interface */
	ret = sprdmux_write(pdata->inst_id, pdata->index, skb->data, skb->len);
	if (ret <= 0){
		VETH_ERR("%s failed to sprdmux write (%d)!!!\n", dev->name, ret);
		veth->stats.tx_fifo_errors++;
		/* TODO, maybe it needs a tx thread to perform the lost skb*/
		dev_kfree_skb_any(skb);
		return NETDEV_TX_BUSY;
	}

	veth->stats.tx_packets++;
	veth->stats.tx_bytes += skb->len;
	dev->trans_start = jiffies;

	ret = NETDEV_TX_OK;
	dev_kfree_skb_any(skb);

	VETH_INFO("%s start xmit done\n", dev->name);
	return ret;
}

static struct net_device_stats * veth_ndo_get_stats(struct net_device *dev)
{
	struct veth_device *veth = netdev_priv(dev);
	return &(veth->stats);
}

static void veth_ndo_tx_timeout(struct net_device *dev)
{
	VETH_INFO ("%s tx timeout\n", dev->name);
	netif_wake_queue(dev);
}


static struct net_device_ops veth_ops = {
	.ndo_open = veth_ndo_open,
	.ndo_stop = veth_ndo_stop,
	.ndo_start_xmit = veth_ndo_start_xmit,
	.ndo_get_stats = veth_ndo_get_stats,
	//.ndo_set_multicast_list = veth_ndo_set_multicast_list,
	.ndo_tx_timeout = veth_ndo_tx_timeout,
};

static int veth_probe(struct platform_device *pdev)
{
	struct veth_init_data *pdata = (struct veth_init_data*)(pdev->dev.platform_data);
	struct veth_device *veth;
	struct net_device *netdev;
	struct veth_rx_packet *vpkt;
	int inst_id;
	int index, ret;
	char name[64] = {0};

	index = pdata->index;
	inst_id = pdata->inst_id;

	sprintf(name, "%s", pdata->name);
	netdev = alloc_netdev(sizeof(struct veth_device), name, ether_setup);
	if (!netdev) {
		VETH_ERR ("%s failed to alloc_netdev\n", pdata->name);
		return -ENOMEM;
	}

	vpkt = kzalloc(sizeof(struct veth_rx_packet), GFP_KERNEL);
	if (!vpkt) {
		VETH_ERR ("%s failed to alloc mem\n", netdev->name);
		free_netdev(netdev);
		return -ENOMEM;
	}

	veth = netdev_priv(netdev);
	veth->netdev = netdev;
	veth->rxpkt = vpkt;
	veth->pdata = pdata;

	netdev->netdev_ops = &veth_ops;
	netdev->watchdog_timeo = 2*HZ;
	netdev->irq = 0;
	netdev->dma = 0;

//	netdev->flags &= ~IFF_MULTICAST;
//	netdev->flags &= ~IFF_BROADCAST;
//	netdev->flags |= IFF_NOARP;

	random_ether_addr(netdev->dev_addr);

	/* register new Ethernet interface */
	if ((ret = register_netdev (netdev))) {
		VETH_ERR ("%s failed to register netdev, %d\n", netdev->name, ret);
		free_netdev(netdev);
		netdev = NULL;
		return ret;
	}

	/* set link as disconnected */
	netif_carrier_off(netdev);

	platform_set_drvdata(pdev, veth);
	VETH_INFO("%s probe done\n", netdev->name);
	return 0;

}

static int veth_remove(struct platform_device *pdev)
{
	struct veth_device *veth = platform_get_drvdata(pdev);

	if (veth->netdev) {
		unregister_netdev(veth->netdev);
		free_netdev(veth->netdev);
	}

	platform_set_drvdata(pdev, NULL);

	return 0;
}

static struct platform_driver veth_driver = {
	.probe = veth_probe,
	.remove = veth_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "sprd_veth",
	},
};

static int __init veth_init(void)
{
	return platform_driver_register(&veth_driver);
}

static void __exit veth_exit(void)
{
	platform_driver_unregister(&veth_driver);
}

module_init (veth_init);
module_exit (veth_exit);

MODULE_AUTHOR("Qiu Yi");
MODULE_DESCRIPTION ("Spreadtrum Virtual Ethernet Device Driver");
MODULE_LICENSE ("GPL");
