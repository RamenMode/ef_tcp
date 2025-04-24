#include <stdint.h>
#include <string.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <net/ethernet.h>
#include <iostream>

/* Ethernet header */

enum class TCP_FLAGS: uint8_t {
    SYN = 0b00000010,
    ACK = 0b00010000,
    FIN = 0b00000001,
    RST = 0b00000100,
};

struct eth_hdr
{
    // swap the mac addresses
    uint8_t dst_mac[ETH_ALEN] = {0x00, 0x0f, 0x53, 0x4b, 0xe6, 0xb1}; /* Destination MAC address */
    uint8_t src_mac[ETH_ALEN] = {0x00, 0x0f, 0x53, 0x59, 0xa5, 0xe1}; /* Source MAC address */
    uint16_t ether_type = htons(ETH_P_IP);                            /* EtherType (e.g., ETH_P_IP) */
} __attribute__((packed));

/* IP header */
struct ip_hdr
{
    uint8_t version_ihl = 0x45; /* Version and IHL 4 indicates ipv4, 5 indicates 32*5 = 160 bits*/
    uint8_t dscp_ecn = 0x00;
    uint16_t tot_len; /* Total Length */              // # MODIFICATION
    uint16_t id = htons(0x00fc); /* Identification */ // No fragmentation, so don't care
    uint16_t flags_frag_off = htons(0x0000);          /* Flags and Fragment Offset, assuming no fragmentation */
    uint8_t ttl = 0x40;                               /* Time to Live */
    uint8_t protocol = IPPROTO_TCP;                   /* Protocol (e.g., IPPROTO_TCP) */
    uint16_t check = 0; /* Header Checksum */         // MODIFICATION
    uint32_t src_addr = htonl(0xc0a80d15);            /* Source Address */
    uint32_t dst_addr = htonl(0xc0a80d0a);            /* Destination Address */
} __attribute__((packed));

/* TCP header */
struct tcp_hdr
{
    uint16_t src_port; /* Source Port */              // MODIFICATION
    uint16_t dst_port; /* Destination Port */         // MODIFICATION
    uint32_t seq_num; /* Sequence Number */           // MODIFICATION
    uint32_t ack_num; /* Acknowledgment Number */     // MODIFICATION
    uint8_t data_off_reserved = 0b01010000;           /* Data Offset and Reserved */
    uint8_t flags; /* TCP Flags */                    // MODIFICATION
    uint16_t window = htons(UINT16_MAX); /* Window */ // MODIFICATION
    uint16_t check = 0; /* Checksum */                // MODIFICATION
    uint16_t urg_ptr = 0;                             /* Urgent Pointer */
} __attribute__((packed));

struct pkt_hdr
{
    struct eth_hdr eth;
    struct ip_hdr ip;
    struct tcp_hdr tcp;
} __attribute__((packed));

/* Compute checksum for count bytes starting at addr, using one's complement of one's complement sum*/
static unsigned short compute_checksum(unsigned short *addr, unsigned int count)
{
    uint32_t sum = 0;

    while (count > 1)
    {
        sum += *addr++;
        count -= 2;
    }

    // Handle leftover byte
    if (count > 0)
    {
        sum += *((uint8_t *)addr) << 8; // pad high byte
    }

    // Fold to 16 bits
    while (sum >> 16)
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (unsigned short)(~sum);
}

void compute_ip_checksum(struct ip_hdr *ip_hdr)
{
    ip_hdr->check = 0;
    ip_hdr->check = compute_checksum((unsigned short *)ip_hdr, 20);
}

static inline uint16_t tcp_checksum(struct pkt_hdr *pkt, size_t payload_len, const char *payload)
{
    uint32_t sum = 0;
    const uint16_t *p;
    size_t len;

    // Step 1: Zero the checksum field
    ((struct tcp_hdr *)&pkt->tcp)->check = 0;

    // Step 2: Sum TCP header
    p = (const uint16_t *)&pkt->tcp;
    len = sizeof(struct tcp_hdr);
    while (len > 1)
    {
        sum += *p++;
        len -= 2;
    }

    // Step 3: Sum payload
    p = (const uint16_t *)payload;
    len = payload_len;
    while (len > 1)
    {
        sum += *p++;
        len -= 2;
    }
    if (len == 1)
    {
        sum += *((const uint8_t *)p); // pad last byte if payload is odd-length
    }

    // Step 4: Sum pseudo-header
    uint32_t src = ntohl(pkt->ip.src_addr);
    uint32_t dst = ntohl(pkt->ip.dst_addr);

    sum += (src >> 16) & 0xFFFF;
    sum += src & 0xFFFF;
    sum += (dst >> 16) & 0xFFFF;
    sum += dst & 0xFFFF;

    sum += htons(pkt->ip.protocol);                     // protocol is 8 bits, promoted to 16
    sum += htons(sizeof(struct tcp_hdr) + payload_len); // TCP length

    // Step 5: Fold 32-bit sum to 16-bit
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return htons(~(uint16_t)sum);
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
static inline void build_tcp_packet(const char *payload, size_t payload_len, uint8_t flags, uint32_t seq, uint32_t ack, char *buffer)
{
    struct pkt_hdr pkt_hdr;
    std::cout << "ack: " << ack << std::endl;
    pkt_hdr.ip.tot_len = htons((uint16_t)(sizeof(struct ip_hdr) + sizeof(struct tcp_hdr) + payload_len));
    std::cout << "IP TOTAL LEN: " << payload_len << std::endl;
    compute_ip_checksum(&pkt_hdr.ip);

    pkt_hdr.tcp.src_port = htons(1234);  // MANUAL
    pkt_hdr.tcp.dst_port = htons(12345); // MANUAL
    pkt_hdr.tcp.seq_num = htonl(seq);    // MANUAL
    pkt_hdr.tcp.ack_num = htonl(ack);    // MANUAL
    pkt_hdr.tcp.flags = flags;           // MANUAL
    pkt_hdr.tcp.check = tcp_checksum(&pkt_hdr, payload_len, (char *)payload);

    std::cout << "TCP ACK NUM: " << ntohl(pkt_hdr.tcp.ack_num) << std::endl;

    memcpy(buffer, &pkt_hdr, sizeof(struct pkt_hdr));
    if (payload_len > 0)
    {
        memcpy(buffer + sizeof(struct pkt_hdr), payload, payload_len);
    }

    return;
}