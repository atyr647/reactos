import subprocess,re,sys,collections
sets={'nvme':['drivers/nvme/host/nvme-core.o','drivers/nvme/host/nvme.o'],
'r8169':['drivers/net/ethernet/realtek/r8169.o'],
'xhci':['drivers/usb/host/xhci-hcd.o','drivers/usb/host/xhci-pci.o'],
'igc':['drivers/net/ethernet/intel/igc/igc.o']}
have=set(open('../need_cur.txt').read().split())  # symbols still needed by gpu set (informational)
def cat(s):
    for p,c in [(r'^(blk_|bio_|__blk|disk_|set_capacity|__alloc_disk|put_disk|device_add_disk|del_gendisk|blkdev|submit_bio|queue_)', 'block'),
                (r'^(netif_|napi_|__napi|skb|__skb|netdev|alloc_etherdev|register_netdev|unregister_netdev|eth_|ethtool|phy_|mdiobus|mdio|__netdev|dev_kfree_skb|net_|ptp_|rtnl|page_pool|xdp|__page_pool|dql|netlink|sk_buff|__dev_|flow_|devlink|napi)', 'net'),
                (r'^(usb_|__usb|hcd|hub_|urb|usb)', 'usb'),
                (r'^(pci_|pcim|__pci|dma_|dmam|__dma|ioremap|iounmap|request_|free_irq|devm_|__devm|irq_|synchronize_irq|msi|enable_irq|disable_irq)', 'pci/dma/irq'),
                (r'^(kmalloc|kfree|__kmalloc|kmem|vmalloc|vfree|__vmalloc|krealloc|kvfree|kzalloc|__kmem|memcpy|memset|memcmp|strlen|strcmp|strncmp|strscpy|snprintf|sprintf|vsnprintf|printk|_printk|__warn|panic|mutex|__mutex|_raw_spin|__raw|queue_work|queue_delayed|mod_timer|del_timer|msleep|usleep|udelay|ktime|jiffies|schedule|wait|__wake|wake_up|complete|init_completion|cancel|flush|destroy_workqueue|alloc_workqueue|__init_waitqueue|add_timer|timer)', 'core/util')]:
        if re.match(p,s): return c
    return 'other'
for name,objs in sets.items():
    U=set();D=set()
    for o in objs:
        for l in subprocess.run(['nm',o],capture_output=True,text=True).stdout.split('\n'):
            p=l.split()
            if len(p)==2 and p[0]=='U': U.add(p[1])
            elif len(p)==3 and p[1].isupper() and p[1] in 'TDBRWVC': D.add(p[2])
    ext=sorted(U-D)
    c=collections.Counter(cat(s) for s in ext)
    print(name,len(ext),dict(c))
    open('../need_%s.txt'%name,'w').write('\n'.join(ext))
