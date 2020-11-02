#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <unistd.h>

#define NETDEV_TYPE_ETHERNET (0x0001)

#define NETDEV_FLAG_BROADCAST (0x0001)

#include "ethernet.h"
#define NETDEV_PROTO_IP ETHERNET_TYPE_IP

#define NETIF_FAMILY_IPV4 (0x02)


#ifndef IFNAMSIZ
#define IFNAMSIZ (16)
#endif

struct netdev;

struct netif {
    struct netif *next;
    uint8_t family;
    struct netdev *dev;
};

struct netdev_ops {
    int (*open)(struct netdev *dev, int opt);
    int (*close)(struct netdev *dev);
    int (*run)(struct netdev *dev);
    int (*stop)(struct netdev *dev);
    ssize_t (*tx)(struct netdev *dev, uint16_t type, uint8_t *packet, size_t size, const void *dst);
};

struct netdev_def {
    uint16_t type;
    uint16_t mtu;
    uint16_t flags;
    uint16_t hlen;
    uint16_t alen;
    struct netdev_ops *ops;
};

struct netdev {
    struct netdev *next;
    struct netif *ifs;
    char name[IFNAMSIZ];
    uint16_t type;
    uint16_t mtu;
    uint16_t flags;
    uint16_t hlen;
    uint16_t alen;
    uint8_t addr[16];
    uint8_t peer[16];
    uint8_t broadcast[16];
    void (*rx_handler)(struct netdev *dev, uint16_t type, uint8_t *packet, size_t plen);
    struct netdev_ops *ops;
    void *priv;
};

int netdev_driver_register(struct netdev_def *def);
int netdev_proto_register(unsigned short type, void (*handler)(uint8_t *packet, size_t plen, struct netdev *dev));

struct netdev *netdev_alloc(uint16_t type);
int netdev_add_netif(struct netdev *dev, struct netif *netif);
struct netif *netdev_get_netif(struct netdev *dev, int family);

#endif
