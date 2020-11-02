#include "arp.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "ethernet.h"
#include "ip.h"
#include "net.h"
#include "util.h"

#define ARP_HRD_ETHERNET 0x0001

#define ARP_OP_REQUEST 1
#define ARP_OP_REPLY 2

#define ARP_TABLE_SIZE 4096
#define ARP_TABLE_TIMEOUT_SEC 300

struct arp_hdr {
    uint16_t hrd;
    uint16_t pro;
    uint8_t hln;
    uint8_t pln;
    uint16_t op;
};

struct arp_ethernet {
    struct arp_hdr hdr;
    uint8_t sha[ETHERNET_ADDR_LEN];
    ip_addr_t spa;
    uint8_t tha[ETHERNET_ADDR_LEN];
    ip_addr_t tpa;
} __attribute__((packed));

struct arp_entry {
    unsigned char used;
    ip_addr_t pa;
    uint8_t ha[ETHERNET_ADDR_LEN];
    time_t timestamp;
    pthread_cond_t cond;
    void *data;
    size_t len;
    struct netif *netif;
};

static struct arp_entry arp_table[ARP_TABLE_SIZE];
static time_t timestamp;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static char *arp_opcode_ntop(uint16_t opcode) {
    switch (ntoh16(opcode)) {
        case ARP_OP_REQUEST:
            return "REQUEST";
        case ARP_OP_REPLY:
            return "REPLY";
        default:
            return "UNKNOWN";
    }
}

static void arp_dump(uint8_t *packet, size_t plen) {
    struct arp_ethernet *message;
    char addr[128];

    message = (struct arp_ethernet *)packet;
    fprintf(stderr, " hrd: 0x%04x\n", ntoh16(message->hdr.hrd));
    fprintf(stderr, " pro: 0x%04x\n", ntoh16(message->hdr.pro));
    fprintf(stderr, " hln: %u\n", message->hdr.hln);
    fprintf(stderr, " pln: %u\n", message->hdr.pln);
    fprintf(stderr, "  op: %u (%s)\n", ntoh16(message->hdr.op), arp_opcode_ntop(message->hdr.op));
    fprintf(stderr, " sha: %s\n", ethernet_addr_ntop(message->sha, addr, sizeof(addr)));
    fprintf(stderr, " spa: %s\n", ip_addr_ntop(&message->spa, addr, sizeof(addr)));
    fprintf(stderr, " tha: %s\n", ethernet_addr_ntop(message->tha, addr, sizeof(addr)));
    fprintf(stderr, " tpa: %s\n", ip_addr_ntop(&message->tpa, addr, sizeof(addr)));
}

/*
 * CONTROL ARP TABLE ENTRY
 */

static struct arp_entry *arp_table_freespace(void) {
    struct arp_entry *entry;
    int i;

    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        entry = &arp_table[i];
        if (!entry->used) {
            return entry;
        }
    }
    // TODO: remove unused entry with LRU
    return NULL;
}

static int arp_table_insert(const ip_addr_t *pa, const uint8_t *ha) {
    struct arp_entry *entry;

    entry = arp_table_freespace();
    if (!entry) {
        return -1;
    }
    entry->used = 1;
    entry->pa = *pa;
    memcpy(entry->ha, ha, ETHERNET_ADDR_LEN);
    time(&entry->timestamp);
    pthread_cond_broadcast(&entry->cond);
    return 0;
}

static struct arp_entry *arp_table_select(const ip_addr_t *pa) {
    struct arp_entry *entry;
    int i;

    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        entry = &arp_table[i];
        if (entry->used && entry->pa == *pa) {
            return entry;
        }
    }
    return NULL;
}

static int arp_table_update(struct netdev *dev, const ip_addr_t *pa, const uint8_t *ha) {
    struct arp_entry *entry;

    // find entry;
    entry = arp_table_select(pa);
    if (!entry) {
        return -1;
    }

    // set resolved
    memcpy(entry->ha, ha, ETHERNET_ADDR_LEN);
    time(&entry->timestamp);

    // send saved packet data with resolved hardware address
    if (entry->data) {
        if (entry->netif->dev != dev) {
            fprintf(stderr, "[warning] receive response from unintended device\n");
            dev = entry->netif->dev;
        }
        dev->ops->tx(dev, ETHERNET_TYPE_IP, (uint8_t *)entry->data, entry->len, entry->ha);
        free(entry->data);
        entry->data = NULL;
        entry->len = 0;
    }

    pthread_cond_broadcast(&entry->cond);
    return 0;
}

static void arp_entry_clear(struct arp_entry *entry) {
    entry->used = 0;
    entry->pa = 0;
    memset(entry->ha, 0, ETHERNET_ADDR_LEN);
    entry->timestamp = 0;
    if (entry->data) {
        free(entry->data);
        entry->data = NULL;
        entry->len = 0;
    }

    entry->netif = NULL;
}

/*
 * ARP COMMUNICATION
 */

static int arp_send_request(struct netif *netif, const ip_addr_t *tpa) {
    struct arp_ethernet request;

    if (!tpa) {
        return -1;
    }
    // set arp request parameters
    request.hdr.hrd = hton16(ARP_HRD_ETHERNET);
    request.hdr.pro = hton16(ETHERNET_TYPE_IP);
    request.hdr.hln = ETHERNET_ADDR_LEN;
    request.hdr.pln = IP_ADDR_LEN;
    request.hdr.op = hton16(ARP_OP_REQUEST);
    memcpy(request.sha, netif->dev->addr, ETHERNET_ADDR_LEN);
    request.spa = ((struct netif_ip *)netif)->unicast;
    memset(request.tha, 0, ETHERNET_ADDR_LEN);
    request.tpa = *tpa;

#ifdef DEBUG
    fprintf(stderr, ">>> arp_send_request <<<\n");
    arp_dump((uint8_t *)&request, sizeof(request));
#endif

    if (netif->dev->ops->tx(netif->dev, ETHERNET_TYPE_ARP, (uint8_t *)&request, sizeof(request), ETHERNET_ADDR_BROADCAST) == -1) {
        return -1;
    }
    return 0;
}

static int arp_send_reply(struct netif *netif, const uint8_t *tha, const ip_addr_t *tpa, const uint8_t *dst) {
    struct arp_ethernet reply;

    if (!tha || !tpa) {
        return -1;
    }
    reply.hdr.hrd = hton16(ARP_HRD_ETHERNET);
    reply.hdr.pro = hton16(ETHERNET_TYPE_IP);
    reply.hdr.hln = ETHERNET_ADDR_LEN;
    reply.hdr.pln = IP_ADDR_LEN;
    reply.hdr.op = hton16(ARP_OP_REPLY);
    memcpy(reply.sha, netif->dev->addr, ETHERNET_ADDR_LEN);
    reply.spa = ((struct netif_ip *)netif)->unicast;
    memcpy(reply.tha, tha, ETHERNET_ADDR_LEN);
    reply.tpa = *tpa;

#ifdef DEBUG
    fprintf(stderr, ">>> arp_send_reply <<<\n");
    arp_dump((uint8_t *)&reply, sizeof(reply));
#endif

    if (netif->dev->ops->tx(netif->dev, ETHERNET_TYPE_ARP, (uint8_t *)&reply, sizeof(reply), dst) < 0) {
        return -1;
    }
    return 0;
}

/*
 * ARP INTERFACES
 */

static void arp_table_patrol(void) {
    struct arp_entry *entry;
    int i;

    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        entry = &arp_table[i];
        if (entry->used && timestamp - entry->timestamp > ARP_TABLE_TIMEOUT_SEC) {
            arp_entry_clear(entry);
            pthread_cond_broadcast(&entry->cond);
        }
    }
}

static void arp_rx(uint8_t *packet, size_t plen, struct netdev *dev) {
    struct arp_ethernet *message;
    time_t now;
    int merge = 0;
    struct netif *netif;

    // validate length
    if (plen < sizeof(struct arp_ethernet)) {
        return;
    }

    // validate ARP message
    message = (struct arp_ethernet *)packet;
    if (ntoh16(message->hdr.hrd) != ARP_HRD_ETHERNET) {
        return;
    }
    if (ntoh16(message->hdr.pro) != ETHERNET_TYPE_IP) {
        return;
    }
    if (message->hdr.hln != ETHERNET_ADDR_LEN) {
        return;
    }
    if (message->hdr.pln != IP_ADDR_LEN) {
        return;
    }

#ifdef DEBUG
    fprintf(stderr, ">>> arp_rx <<<\n");
    arp_dump(packet, plen);
#endif

    pthread_mutex_lock(&mutex);
    time(&now);
    if (now - timestamp > 10) {
        timestamp = now;
        arp_table_patrol();
    }

    // update arp table entry
    merge = (arp_table_update(dev, &message->spa, message->sha) == 0) ? 1: 0;
    pthread_mutex_unlock(&mutex);

    // save arp message if target is this machine
    netif = netdev_get_netif(dev, NETIF_FAMILY_IPV4);
    if (netif && ((struct netif_ip *)netif)->unicast == message->tpa) {
        if (!merge) {
            pthread_mutex_lock(&mutex);
            // TODO: if there is no space
            // TODO: Resilient for DoS attack
            arp_table_insert(&message->spa, message->sha);
            pthread_mutex_unlock(&mutex);
        }
        if (ntoh16(message->hdr.op) == ARP_OP_REQUEST) {
            arp_send_reply(netif, message->sha, &message->spa, message->sha);
        }
    }
    return;
}

int arp_resolve(struct netif *netif, const ip_addr_t *pa, uint8_t *ha, const void *data, size_t len) {
    struct timeval now;
    struct timespec timeout;
    struct arp_entry *entry;
    int ret;

    pthread_mutex_lock(&mutex);

    // set timeout
    gettimeofday(&now, NULL);
    timeout.tv_sec = now.tv_sec + 1;
    timeout.tv_nsec = now.tv_usec * 1000;

    entry = arp_table_select(pa);
    if (entry) {
        if (memcmp(entry->ha, ETHERNET_ADDR_ANY, ETHERNET_ADDR_LEN) == 0) {
            // arp request has already sent. wait for reply
            // resend arp request for the case packet loss
            arp_send_request(netif, pa);
            do {
                // wait until reply come with timeout
                ret = pthread_cond_timedwait(&entry->cond, &mutex, &timeout);
            } while (ret == EINTR);
            if (!entry->used || ret == ETIMEDOUT) {
                if (entry->used) {
                    arp_entry_clear(entry);
                }
                pthread_mutex_unlock(&mutex);
                return ARP_RESOLVE_ERROR;
            }
        }
        memcpy(ha, entry->ha, ETHERNET_ADDR_LEN);
        pthread_mutex_unlock(&mutex);
        return ARP_RESOLVE_FOUND;
    }

    // create arp table entry
    entry = arp_table_freespace();
    if (!entry) {
        pthread_mutex_unlock(&mutex);
        return ARP_RESOLVE_ERROR;
    }

    // save data
    if (data) {
        entry->data = malloc(len);
        if (!entry->data) {
            pthread_mutex_unlock(&mutex);
            return ARP_RESOLVE_ERROR;
        }
        memcpy(entry->data, data, len);
        entry->len = len;
    }

    // set arp entry
    entry->used = 1;
    entry->pa = *pa;
    time(&entry->timestamp);
    entry->netif = netif;

    // send arp query request
    arp_send_request(netif, pa);

    pthread_mutex_unlock(&mutex);
    return ARP_RESOLVE_QUERY;
}

int arp_init(void) {
    int i;

    time(&timestamp);
    for (i = 0; i < ARP_TABLE_SIZE; i++) {
        pthread_cond_init(&arp_table[i].cond, NULL);
    }
    netdev_proto_register(NETDEV_PROTO_ARP, arp_rx);
    return 0;
}
