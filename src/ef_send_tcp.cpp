#include <etherfabric/vi.h>
#include <etherfabric/pd.h>
#include <etherfabric/memreg.h>
#include <etherfabric/capabilities.h>
#include "utils.h"
#include "pkt_headers.hpp"
#include <iostream>
#include <tuple>

#define PKT_BUF_SIZE 2048                                            // Size of each packet buffer
#define RX_DMA_OFF ROUND_UP(sizeof(struct pkt_buf), EF_VI_DMA_ALIGN) // Offset of the RX DMA address
#define RX_RING_SIZE 512                                             // Maximum number of receive requests in the RX ring
#define TX_RING_SIZE 2048                                            // Maximum number of transmit requests in the TX ring
#define REFILL_BATCH_SIZE 64                                         // Minimum number of buffers to refill the ring

struct pkt_buf
{
    ef_addr rx_ef_addr;
    ef_addr tx_ef_addr;
    int id;
    struct pkt_buf *next;
};

struct pkt_bufs
{
    void *mem;
    size_t mem_size;
    int num;
    struct pkt_buf *free_pool;
    int free_pool_n;
};

struct vi
{
    ef_driver_handle dh;
    ef_pd pd;
    ef_vi vi;
    ef_memreg memreg;
    unsigned int tx_outstanding;
    uint64_t n_pkts;
};

static struct vi vi;
static struct pkt_bufs pbs;
/*
    This function returns a pointer to the packet buffer at index pkt_buf_i.
    It casts the memory pointer to a pointer to a pkt_buf struct.
    It then returns the pointer to the pkt_buf struct.
    id -> pkt_buf struct
*/
static inline struct pkt_buf *pkt_buf_from_id(int pkt_buf_i)
{
    assert((unsigned)pkt_buf_i < (unsigned)pbs.num);
    return (struct pkt_buf *)((char *)pbs.mem + (size_t)pkt_buf_i * PKT_BUF_SIZE);
}
/*
    This function returns the offset of the DMA address from the packet buffer.
    It returns the offset of the DMA address from the packet buffer.
    pkt_buf_i -> offset (not entirely important)
*/
static inline int addr_offset_from_id(int pkt_buf_i)
{
    return (pkt_buf_i % 2) * EF_VI_DMA_ALIGN;
}
/*
    This function refills the RX ring.
    It checks if the RX ring has enough space to refill the ring.
    It also checks if there are enough free buffers to refill the ring.
    If it does, it refills the ring.
    If it doesn't, it returns.
*/
static void vi_refill_rx_ring(void)
{
    ef_vi *vi_ptr = &vi.vi;
    struct pkt_buf *pkt_buf;
    int i;

    if (ef_vi_receive_space(vi_ptr) < REFILL_BATCH_SIZE ||
        pbs.free_pool_n < REFILL_BATCH_SIZE)
        return;

    for (i = 0; i < REFILL_BATCH_SIZE; ++i)
    {
        pkt_buf = pbs.free_pool;
        pbs.free_pool = pbs.free_pool->next;
        --pbs.free_pool_n;
        ef_vi_receive_init(vi_ptr, pkt_buf->rx_ef_addr, pkt_buf->id);
    }
    ef_vi_receive_push(vi_ptr);
}
/*
    This function frees a packet buffer.
    It adds the packet buffer to the free pool.
    pkt_buf -> free pool
*/
static inline void pkt_buf_free(struct pkt_buf *pkt_buf)
{
    pkt_buf->next = pbs.free_pool;
    pbs.free_pool = pkt_buf;
    ++pbs.free_pool_n;
}
/*
    This function initializes the packet buffers.
    It sets the number of packet buffers to the sum of the RX and TX ring sizes.
    It then sets the memory size to the number of packet buffers times the size of each packet buffer.
    It then maps the memory to the packet buffers.
    It then initializes the packet buffers.
*/
static int init_pkts_memory(void)
{
    int i;
    pbs.num = RX_RING_SIZE + TX_RING_SIZE;
    pbs.mem_size = pbs.num * PKT_BUF_SIZE;
    pbs.mem_size = ROUND_UP(pbs.mem_size, huge_page_size);

    pbs.mem = mmap(NULL, pbs.mem_size, PROT_READ | PROT_WRITE,
                   MAP_ANONYMOUS | MAP_PRIVATE | MAP_HUGETLB, -1, 0);
    if (pbs.mem == MAP_FAILED)
    {
        fprintf(stderr, "mmap() failed. Are huge pages configured?\n");
        TEST(posix_memalign(&pbs.mem, huge_page_size, pbs.mem_size) == 0);
    }

    for (i = 0; i < pbs.num; ++i)
    {
        struct pkt_buf *pkt_buf = pkt_buf_from_id(i);
        pkt_buf->id = i;
        pkt_buf_free(pkt_buf);
    }
    return 0;
}
/*
    This function initializes the virtual interface.
    It sets the flags to the default flags.
    It then opens the driver.
    It then allocates the PD.
    It then allocates the VI.
    It then allocates the memory register.
    It then initializes the packet buffers.
    It then sets the filters to receive TCP packets.
    It then returns 0.
*/
static int init()
{
    int i;
    unsigned int vi_flags = EF_VI_FLAGS_DEFAULT;

    TRY(ef_driver_open(&vi.dh));
    TRY(ef_pd_alloc_by_name(&vi.pd, vi.dh, "enp1s0f1", EF_PD_DEFAULT));
    TRY(ef_vi_alloc_from_pd(&vi.vi, vi.dh, &vi.pd, vi.dh, -1,
                            RX_RING_SIZE, TX_RING_SIZE, NULL, -1,
                            (enum ef_vi_flags)vi_flags));

    TRY(ef_memreg_alloc(&vi.memreg, vi.dh, &vi.pd, vi.dh,
                        pbs.mem, pbs.mem_size));

    for (i = 0; i < pbs.num; ++i)
    {
        struct pkt_buf *pkt_buf = pkt_buf_from_id(i);
        pkt_buf->rx_ef_addr = ef_memreg_dma_addr(&vi.memreg, i * PKT_BUF_SIZE) +
                              RX_DMA_OFF + addr_offset_from_id(i);
        pkt_buf->tx_ef_addr = ef_memreg_dma_addr(&vi.memreg, i * PKT_BUF_SIZE) +
                              RX_DMA_OFF + ef_vi_receive_prefix_len(&vi.vi) +
                              addr_offset_from_id(i);
    }

    assert(ef_vi_receive_capacity(&vi.vi) == RX_RING_SIZE - 1);
    assert(ef_vi_transmit_capacity(&vi.vi) == TX_RING_SIZE - 1);

    while (ef_vi_receive_space(&vi.vi) > REFILL_BATCH_SIZE)
        vi_refill_rx_ring();

    // Set up filters to receive all TCP packets
    ef_filter_spec fs;
    ef_filter_spec_init(&fs, EF_FILTER_FLAG_NONE);
    TRY(ef_filter_spec_set_ip4_full(&fs, IPPROTO_TCP, htonl(0xc0a80d15), htons(1234), htonl(0xc0a80d0a), htons(12345)));
    TRY(ef_vi_filter_add(&vi.vi, vi.dh, &fs, NULL));

    return 0;
}

void dump_buffer(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (i % 16 == 0)
            printf("%04zx: ", i);
        printf("%02x ", buf[i]);
        if (i % 16 == 15 || i == len - 1)
            printf("\n");
    }
}
/**
 * @brief Send a packet with the given payload, payload length, flags, sequence number, and acknowledgment number and frees the buffer
 * Note: seq and ack are numbers to be sent with the packet
 * @param payload
 * @param payload_len
 * @param flags
 * @param seq
 * @param ack
 */

void send_packet(char *payload, int payload_len, uint8_t flags, uint32_t seq, uint32_t ack)
{
    // initialize packet buffer
    struct pkt_buf *pkt_buf = pbs.free_pool;
    pbs.free_pool = pbs.free_pool->next;
    --pbs.free_pool_n;
    std::cout << "Sending packet with ID: " << pkt_buf->id << std::endl;
    // build packet
    build_tcp_packet(payload, payload_len, flags, seq, ack, (char *)pkt_buf + RX_DMA_OFF + addr_offset_from_id(pkt_buf->id) + ef_vi_receive_prefix_len(&vi.vi));
    // initialize transmit
    int rc = ef_vi_transmit(&vi.vi, pkt_buf->tx_ef_addr, RX_DMA_OFF + addr_offset_from_id(pkt_buf->id) + ef_vi_receive_prefix_len(&vi.vi) + payload_len + sizeof(struct pkt_hdr), pkt_buf->id);
    if (rc != 0)
    {
        throw std::runtime_error("Failed to transmit");
        return;
    }
    pkt_buf_free(pkt_buf);
    return;
}
/*
 * Receive a packet and verify the seq, ack, and flags are as expected
 */
std::tuple<struct pkt_hdr *, uint32_t, uint8_t> receive_packet(uint8_t flags, uint32_t seq, uint32_t ack)
{
    ef_event evs[EF_VI_EVENT_POLL_MIN_EVS];
    while (true)
    {
        int n_ev = ef_eventq_poll(&vi.vi, evs, sizeof(evs) / sizeof(evs[0]));
        for (int i = 0; i < n_ev; ++i)
        {
            switch (EF_EVENT_TYPE(evs[i]))
            {
            case EF_EVENT_TYPE_TX:
                std::cout << "Transmit completed successfully" << std::endl;
                break;
            case EF_EVENT_TYPE_TX_WITH_TIMESTAMP:
                std::cout << "Transmit completed successfully with timestamp" << std::endl;
                break;
            case EF_EVENT_TYPE_TX_ERROR:
                throw std::runtime_error("Transmit failed");
            case EF_EVENT_TYPE_RX:
            {
                std::cout << "Received packet" << std::endl;
                auto id = EF_EVENT_RX_RQ_ID(evs[i]);
                std::cout << "ID: " << id << std::endl;
                struct pkt_buf *pkt_buf = pkt_buf_from_id(id);
                dump_buffer((const uint8_t*) pkt_buf, PKT_BUF_SIZE);
                char *tcp_pkt = (char *)pkt_buf + RX_DMA_OFF + addr_offset_from_id(pkt_buf->id) + ef_vi_receive_prefix_len(&vi.vi);
                dump_buffer((const uint8_t*) tcp_pkt, sizeof(struct pkt_hdr));
                struct pkt_hdr *hdr = (struct pkt_hdr *)tcp_pkt;
                if ((flags & (uint8_t)TCP_FLAGS::SYN) && (hdr->tcp.flags & (uint8_t)TCP_FLAGS::SYN) == 0)
                {
                    throw std::runtime_error("SYN not received when expected");
                }
                if ((flags & (uint8_t)TCP_FLAGS::ACK) && (hdr->tcp.flags & (uint8_t)TCP_FLAGS::ACK) == 0)
                {
                    throw std::runtime_error("ACK not received when expected");
                }
                std::cout << "Receiveddsd packet" << std::endl;
                dump_buffer((const uint8_t*) hdr, (size_t) sizeof(struct pkt_hdr));
                std::cout << "Payload length: " << ntohs(hdr->ip.tot_len) << " - " << (uint32_t)((hdr->ip.version_ihl & 0x0F) * 4) << " - " <<  (uint32_t)(((hdr->tcp.data_off_reserved & 0xF0) >> 4) * 4) << std::endl;
                return std::make_tuple(hdr, (uint32_t) ntohs(hdr->ip.tot_len) - (uint32_t)((hdr->ip.version_ihl & 0x0F) * 4) - (uint32_t)((hdr->tcp.data_off_reserved & 0xF0) * 4), id);
            }
            default:
                std::cerr << "Unexpected event type: " << EF_EVENT_TYPE(evs[i]) << std::endl;
                break;
            }
        }
    }
}

void send_connection_handshake()
{
    // Send SYN packet

    char *payload = NULL;
    uint32_t payload_len = 0;
    uint8_t flags = 0b00000010;
    uint32_t seq = 1;
    uint32_t ack = 0;
    send_packet(payload, payload_len, flags, seq, ack);

    // handle SYN-ACK
    flags = (uint8_t)TCP_FLAGS::SYN & (uint8_t)TCP_FLAGS::ACK;
    uint32_t s_seq = seq + 1;
    uint32_t s_ack = 0; // don't care
    auto [tcp_pkt, len, id] = receive_packet(flags, seq, ack);
    seq = ntohl(tcp_pkt->tcp.seq_num); // this is the seq number of the server
    dump_buffer((const uint8_t*) tcp_pkt, sizeof(struct pkt_hdr));
    std::cout << "Server seq: " << seq << std::endl;

    // send ACK
    flags = (uint8_t)TCP_FLAGS::ACK;
    uint32_t new_seq = s_seq;
    uint32_t new_ack = seq + 1;
    std::cout << "Sending ack" << new_ack << std::endl;
    payload = NULL;
    payload_len = 0;
    send_packet(payload, payload_len, flags, new_seq, new_ack);

    flags = (uint8_t)TCP_FLAGS::ACK;
    payload = "Hello World\n";
    payload_len = strlen(payload);
    send_packet(payload, payload_len, flags, new_seq, new_ack);

}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        fprintf(stderr, "usage: %s <interface>\n", argv[0]);
        return 1;
    }

    TRY(init_pkts_memory());
    TRY(init());

    // Get a buffer from the free pool
    for (int i = 0; i < 1; i++)
    {
        send_connection_handshake();
    }

    return 0;
}

/*
For use with generalized event driven rx handling
if (EF_EVENT_TYPE(evs[i]) == EF_EVENT_TYPE_RX) {
            auto id = EF_EVENT_RX_RQ_ID(evs[i]);
            char* pkt = pkt_bufs + id * BUF_SIZE; // need to use the other function
            size_t len = EF_EVENT_RX_BYTES(evs[i]); 
            handle_packet(pkt, len);
            ef_vi_receive_init(&vi, ef_memreg_dma_addr(&mr, id * BUF_SIZE), id);
        }

*/