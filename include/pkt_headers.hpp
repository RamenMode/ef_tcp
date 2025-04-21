#include <stdint.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <net/ethernet.h>

/* Ethernet header */
struct eth_hdr {
    uint8_t dst_mac[ETH_ALEN] = {0x00, 0x0f, 0x53, 0x59, 0xa5, 0xe0};  /* Destination MAC address */
    uint8_t src_mac[ETH_ALEN] = {0x00, 0x0f, 0x53, 0x4b, 0xe6, 0xb1};  /* Source MAC address */
    uint16_t ether_type = ETH_P_IP;        /* EtherType (e.g., ETH_P_IP) */
} __attribute__((packed));

/* IP header */
struct ip_hdr {
    uint8_t  version_ihl = 0x45;      /* Version and IHL 4 indicates ipv4, 5 indicates 32*5 = 160 bits*/
    uint8_t  dscp_ecn = 0x00;
    uint16_t tot_len;          /* Total Length */ // # MODIFICATION
    uint16_t id = 0x0000;               /* Identification */ // MODIFICATION
    uint16_t flags_frag_off = 0x0000;   /* Flags and Fragment Offset, assuming no fragmentation */
    uint8_t  ttl = 0x40;              /* Time to Live */
    uint8_t  protocol = IPPROTO_TCP;         /* Protocol (e.g., IPPROTO_TCP) */
    uint16_t check = 0;            /* Header Checksum */ // MODIFICATION
    uint32_t src_addr = 0xc0a80c14;         /* Source Address */
    uint32_t dst_addr = 0xc0a80c0a;         /* Destination Address */
} __attribute__((packed));

/* TCP header */
struct tcp_hdr {
    uint16_t src_port;         /* Source Port */ // MODIFICATION
    uint16_t dst_port;         /* Destination Port */ // MODIFICATION
    uint32_t seq_num;          /* Sequence Number */ // MODIFICATION
    uint32_t ack_num;          /* Acknowledgment Number */ // MODIFICATION
    uint8_t  data_off_reserved = 0b01010000; /* Data Offset and Reserved */
    uint8_t  flags;            /* TCP Flags */ // MODIFICATION
    uint16_t window = UINT16_MAX;           /* Window */ // MODIFICATION
    uint16_t check = 0;            /* Checksum */ // MODIFICATION
    uint16_t urg_ptr = 0;          /* Urgent Pointer */
} __attribute__((packed));


struct pkt_hdr {
    struct eth_hdr eth;
    struct ip_hdr  ip;
    struct tcp_hdr tcp;
} __attribute__((packed));

/* Calculate IP header checksum */
static inline uint16_t ip_checksum(const struct ip_hdr *ip) {
    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)ip;
    size_t len = sizeof(struct ip_hdr);
    
    while (len > 1) {
        sum += *p++;
        len -= 2;
    }
    
    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    
    return htons(~sum);
}

static inline uint16_t tcp_checksum(struct pkt_hdr *pkt, size_t payload_len) {
    uint32_t sum = 0;
    const uint16_t *p = (const uint16_t *)pkt;
    size_t len = sizeof(struct pkt_hdr);

    while (len > 1) {
        sum += *p++;
        len -= 2;
    }

    len = payload_len;

    while (len > 1) {
        sum += *p++;
        len -= 2;
    }

    sum += (pkt->ip.src_addr >> 16) & 0xFFFF;
    sum += pkt->ip.src_addr & 0xFFFF;

    sum += (pkt->ip.dst_addr >> 16) & 0xFFFF;
    sum += pkt->ip.dst_addr & 0xFFFF;

    sum += pkt->ip.protocol;
    
    /* Add TCP length (header + payload) */
    uint16_t tcp_len = sizeof(struct tcp_hdr) + payload_len;
    sum += tcp_len;

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    
    return htons(~sum);
}

/**
 * Builds a TCP packet with the given payload and payload length.
 * The packet is built in the buffer passed as argument. The passed buffer is populated with the complete packet.
 * 
 * @param pkt_hdr: Pointer to the packet header.
 * @param payload: Pointer to the payload.
 * @param payload_len: Length of the payload.
 * @param buffer: Buffer to store the packet.
 * */
static inline void build_tcp_packet(struct pkt_hdr *pkt_hdr, const char *payload, size_t payload_len, char* buffer) {
    pkt_hdr = (struct pkt_hdr *) buffer;
    memcpy(buffer + sizeof(struct pkt_hdr), payload, payload_len);

    pkt_hdr->ip.tot_len = htons( (uint16_t) (sizeof(struct ip_hdr) + sizeof(struct tcp_hdr) + payload_len));
    pkt_hdr->ip.id = 0x0000; // MANUAL
    pkt_hdr->ip.check = ip_checksum(&pkt_hdr->ip);

    pkt_hdr->tcp.src_port = htons(1234); // MANUAL
    pkt_hdr->tcp.dst_port = htons(12345); // MANUAL
    pkt_hdr->tcp.seq_num = htons(1); // MANUAL
    pkt_hdr->tcp.ack_num = 0; // MANUAL
    pkt_hdr->tcp.flags = 0b00000010; // MANUAL
    pkt_hdr->tcp.check = tcp_checksum(pkt_hdr, payload_len);

    return;
}