#include <cassert>
#include "ef.hpp"
using namespace std;

#define BUF_SIZE 2048  // size of your DMA buffer
#define PAGE_ALIGN 4096 // page alignment for DMA so we start on a page boundary
#define MEM_REG_SIZE PAGE_ALIGN
#define NIC_INDEX 6    // index of enp1s0f1
#define RX_DMA_OFF ROUND_UP(sizeof(struct pkt_buf), EF_VI_DMA_ALIGN)
#define RX_RING_SIZE 512
#define TX_RING_SIZE 2048

struct pkt_buf {
    ef_addr rx_ef_addr[2];
    ef_addr tx_ef_addr[2];
    int id;
    struct pkt_buf *next;
};

struct pkt_bufs {
    /* Memory for packet buffers */
    void *mem;
    size_t mem_size;

    /* Number of packet buffers allocated */
    int num;

    /* pool of free packet buffers (LIFO to minimise working set) */
    struct pkt_buf *free_pool;
    int free_pool_n;

};

struct vi {
    /* handle for accessing the driver */
    ef_driver_handle dh;
    /* protection domain */
    ef_pd pd;
    /* virtual interface (rxq + txq + evq) */
    ef_vi vi;
    /* registered memory for DMA */
    ef_memreg memreg;
    /* number of TX waiting to be pushed (in '-x' mode) */
    unsigned int tx_outstanding;
    /* statistics */
    uint64_t n_pkts;
};

static struct vi vis[2]; // 2 virtual interfaces
static struct pkt_bufs pbs; // packet buffers (can allow us to get all packet buffer locations)
static inline struct pkt_buf* pkt_buf_from_id(int pkt_buf_i) // get packet buffer metadata info at start of the buffer
{
  assert((unsigned) pkt_buf_i < (unsigned) pbs.num);
  return (struct pkt_buf*) ((char*) pbs.mem + (size_t) pkt_buf_i * BUF_SIZE);
}

/*
  Get the offset of the packet buffer in the DMA memory region in the page
*/
static inline int addr_offset_from_id(int pkt_buf_i) {
    return ( pkt_buf_i % 2 ) * PAGE_ALIGN;
}








// MCAST_IP = 239.1.3.37
// MCAST_PORT = 12345
// INTER_IP = 192.168.13.21
// INTERFACE = enp1s0f1

int main(int argc, char *argv[])
{

    ef_driver_handle dh; // allocate the file descriptor to communicate with the NIC Solarflare driver
    ef_pd pd;               // (FOR PD) allocate the protection domain for the NIC Solarflare driver
    ef_vi vi;               // (FOR VI) allocate the virtual interface for the NIC Solarflare driver

    ef_memreg mr;           // (FOR MEM) allocate the memory region for the NIC Solarflare driver

    // Open protection domain driver
    if (ef_driver_open(&dh) < 0)
    {
        perror("ef_driver_open");
        return 1;
    }

    // Allocate Protection Domain (memory space that can only be accessed by the corresponding VI)
    if (ef_pd_alloc(&pd, dh, NIC_INDEX, EF_PD_DEFAULT) < 0)
    {
        perror("ef_pd_alloc");
        return 1;
    }


    // Allocate VI
    if (ef_vi_alloc_from_pd(&vi, dh, &pd, dh, -1, -1, -1, nullptr, dh, EF_VI_FLAGS_DEFAULT) < 0)
    {
        perror("ef_vi_alloc_from_pd");
        return 1;
    }

    // Allocate aligned memory for DMA for buffer of size 2048 bytes
    void *dma_mem;
    if (posix_memalign(&dma_mem, PAGE_ALIGN, MEM_REG_SIZE) != 0)
    {
        perror("posix_memalign");
        return 1;
    }
    memset(dma_mem, 0, MEM_REG_SIZE);

    // Register buffer with NIC
    if (ef_memreg_alloc(&mr, dh, &pd, dh, dma_mem, MEM_REG_SIZE) < 0)
    {
        perror("ef_memreg_alloc");
        return 1;
    }

    // Buffer is now ready for use, e.g., post for receive/send

    std::cout << "VI and buffer set up successfully." << std::endl;
    std::cout << "ef_vi buffer len" << ef_vi_receive_buffer_len(&vi) << std::endl;

    // Cleanup
    ef_memreg_free(&mr, dh);
    ef_vi_free(&vi, dh);
    ef_pd_free(&pd, dh);
    ef_driver_close(dh);
    free(dma_mem);
    return 0;
}