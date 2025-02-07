#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>

#define X280_MAGIC 0x58323830 /* "X280" in ASCII hex */
#define MAX_PACKET_SIZE ETH_FRAME_LEN
#define NUM_PACKETS 650

static const uint64_t REGS = 0x00002ff10000UL;

struct packet {
	uint32_t len;
	uint8_t data[MAX_PACKET_SIZE];
};

struct x280_shmem_layout {
	uint64_t magic;
	struct packet x280_tx[NUM_PACKETS]; /* X280 -> Host */
	struct packet x280_rx[NUM_PACKETS]; /* Host -> X280 */
	uint32_t x280_tx_head; /* Written by X280 */
	uint32_t x280_tx_tail; /* Written by Host */
	uint32_t x280_rx_head; /* Written by Host */
	uint32_t x280_rx_tail; /* Written by X280 */
};

static_assert(sizeof(struct x280_shmem_layout) < (1 << 21), "Shared memory size mismatch");

struct x280_net_dev {
	void __iomem *shmem;
	void __iomem *regs;
	size_t shmem_size;
	struct net_device *ndev;
	struct napi_struct napi;
#ifdef X280_POLLING_MODE
	struct timer_list poll_timer;
#endif
	int irq;
};

static irqreturn_t x280_irq_handler(int irq, void *data)
{
	struct x280_net_dev *priv = data;

	u32 irq_status = ioread32(priv->regs + 0x404);
	iowrite32(irq_status & ~(1 << 27), priv->regs + 0x404);

	netif_wake_queue(priv->ndev);

	if (napi_schedule_prep(&priv->napi)) {
		__napi_schedule(&priv->napi);
	}

	return IRQ_HANDLED;
}

#ifdef X280_POLLING_MODE
static void x280_timer_cb(struct timer_list *t)
{
	struct x280_net_dev *priv = from_timer(priv, t, poll_timer);
	if (napi_schedule_prep(&priv->napi)) {
		__napi_schedule(&priv->napi);
	}

	mod_timer(&priv->poll_timer, jiffies + msecs_to_jiffies(10));
}
#endif

static netdev_tx_t x280_net_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct x280_net_dev *priv = netdev_priv(dev);
	struct x280_shmem_layout *shmem = priv->shmem;
	uint32_t next_head;

	if (skb->len > MAX_PACKET_SIZE) {
		pr_info("Packet too large: %d\n", skb->len);
		dev_kfree_skb(skb);
		dev->stats.tx_dropped++;
		return NETDEV_TX_OK;
	}

	next_head = (shmem->x280_tx_head + 1) % NUM_PACKETS;
	if (next_head == shmem->x280_tx_tail) {
		/* Ring full */
		pr_info("Ring full\n");
		netif_stop_queue(dev);
		return NETDEV_TX_BUSY;
	}

	/* Copy the packet */
	shmem->x280_tx[shmem->x280_tx_head].len = skb->len;
	memcpy(shmem->x280_tx[shmem->x280_tx_head].data, skb->data, skb->len);

	/* Update head after data is written */
	smp_wmb();
	shmem->x280_tx_head = next_head;

	dev->stats.tx_packets++;
	dev->stats.tx_bytes += skb->len;

	dev_kfree_skb(skb);
	return NETDEV_TX_OK;
}

static int x280_net_poll(struct napi_struct *napi, int budget)
{
	struct x280_net_dev *priv = container_of(napi, struct x280_net_dev, napi);
	struct net_device *dev = priv->ndev;
	struct x280_shmem_layout *shmem = priv->shmem;
	struct sk_buff *skb;
	int work_done = 0;

	while (work_done < budget && shmem->x280_rx_tail != shmem->x280_rx_head) {
		struct packet *pkt = &shmem->x280_rx[shmem->x280_rx_tail];

		if (pkt->len == 0 || pkt->len > MAX_PACKET_SIZE)
			break;

		skb = netdev_alloc_skb(dev, pkt->len);
		if (!skb)
			break;

		memcpy(skb_put(skb, pkt->len), pkt->data, pkt->len);
		skb->protocol = eth_type_trans(skb, dev);
		napi_gro_receive(napi, skb);

		dev->stats.rx_packets++;
		dev->stats.rx_bytes += pkt->len;

		shmem->x280_rx_tail = (shmem->x280_rx_tail + 1) % NUM_PACKETS;
		work_done++;
	}

	if (work_done < budget)
		napi_complete_done(napi, work_done);

	return work_done;
}

static int x280_net_open(struct net_device *dev)
{
	struct x280_net_dev *priv = netdev_priv(dev);
	napi_enable(&priv->napi);
	netif_start_queue(dev);
	return 0;
}

static int x280_net_stop(struct net_device *dev)
{
	struct x280_net_dev *priv = netdev_priv(dev);
	netif_stop_queue(dev);
	napi_disable(&priv->napi);
	return 0;
}

static const struct net_device_ops x280_netdev_ops = {
	.ndo_open = x280_net_open,
	.ndo_stop = x280_net_stop,
	.ndo_start_xmit = x280_net_xmit,
	.ndo_set_mac_address = eth_mac_addr,
};

static int x280_net_probe(struct platform_device *pdev)
{
	struct x280_net_dev *priv;
	struct net_device *ndev;
	struct resource *res;
	int ret;

	ndev = alloc_etherdev(sizeof(struct x280_net_dev));
	if (!ndev)
		return -ENOMEM;

	priv = netdev_priv(ndev);
	priv->ndev = ndev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		ret = -ENODEV;
		goto err_free_netdev;
	}

	priv->shmem = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(priv->shmem)) {
		ret = PTR_ERR(priv->shmem);
		goto err_free_netdev;
	}

	priv->regs = devm_ioremap(&pdev->dev, REGS, 0x1000);
	if (IS_ERR(priv->regs)) {
		ret = PTR_ERR(priv->regs);
		goto err_free_netdev;
	}

	priv->shmem_size = resource_size(res);

	struct x280_shmem_layout *shmem = priv->shmem;
	shmem->magic = X280_MAGIC;
	shmem->x280_tx_head = 0;
	shmem->x280_tx_tail = 0;
	shmem->x280_rx_head = 0;
	shmem->x280_rx_tail = 0;

	ndev->netdev_ops = &x280_netdev_ops;
	ndev->flags |= IFF_BROADCAST | IFF_MULTICAST;
	ndev->mtu = ETH_DATA_LEN;

	eth_hw_addr_random(ndev);

	netif_napi_add(ndev, &priv->napi, x280_net_poll);

	SET_NETDEV_DEV(ndev, &pdev->dev);
	platform_set_drvdata(pdev, priv);

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0) {
		dev_err(&pdev->dev, "Failed to get interrupt: %d\n", priv->irq);
		ret = priv->irq;
		goto err_free_netdev;
	}

	ret = devm_request_irq(&pdev->dev, priv->irq, x280_irq_handler,
			       IRQF_SHARED, dev_name(&pdev->dev), priv);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request interrupt: %d\n", ret);
		goto err_free_netdev;
	}

	ret = register_netdev(ndev);
	if (ret)
		goto err_free_netdev;

	dev_info(&pdev->dev, "X280 network driver: %pM\n", ndev->dev_addr);

#ifdef X280_POLLING_MODE
	timer_setup(&priv->poll_timer, x280_timer_cb, 0);
	mod_timer(&priv->poll_timer, jiffies + 1);
#endif

	return 0;

err_free_netdev:
	free_netdev(ndev);
	return ret;
}

static int x280_net_remove(struct platform_device *pdev)
{
	struct x280_net_dev *priv = platform_get_drvdata(pdev);
#ifdef X280_POLLING_MODE
	del_timer_sync(&priv->poll_timer);
#endif
	unregister_netdev(priv->ndev);
	free_netdev(priv->ndev);
	return 0;
}

static const struct of_device_id x280_net_of_match[] = {
	{ .compatible = "tenstorrent,ethernet" },
	{}
};
MODULE_DEVICE_TABLE(of, x280_net_of_match);

static struct platform_driver x280_net_driver = {
	.probe = x280_net_probe,
	.remove = x280_net_remove,
	.driver = {
		.name = "x280-net",
		.of_match_table = x280_net_of_match,
	},
};

module_platform_driver(x280_net_driver);

MODULE_AUTHOR("Your Name");
MODULE_DESCRIPTION("X280 Network Driver");
MODULE_LICENSE("GPL");

/*
insmod tteth.ko
ip link set eth0 up
ip addr add 192.168.10.2/24 dev eth0
*/