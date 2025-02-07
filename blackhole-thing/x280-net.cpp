
#include "blackhole_pcie.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define X280_MAGIC 0x58323830 /* "X280" in ASCII hex */
#define MAX_PACKET_SIZE ETH_FRAME_LEN
#define NUM_PACKETS 650

using namespace tt;
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;
static constexpr size_t L2CPU_X = 8;
static constexpr size_t L2CPU_Y = 3;
static constexpr uint64_t X280_DDR_BASE = 0x4000'3000'0000ULL;
static constexpr uint64_t X280_NET_BUFFERS = 0x4001'2fe0'0000ULL;
static constexpr uint64_t X280_REGS = 0xFFFF'F7FE'FFF1'0000ULL;

struct packet
{
    uint32_t len;
    uint8_t data[MAX_PACKET_SIZE];
};

struct x280_shmem_layout
{
    uint64_t magic;
    struct packet x280_tx[NUM_PACKETS]; /* X280 -> Host */
    struct packet x280_rx[NUM_PACKETS]; /* Host -> X280 */
    uint32_t x280_tx_head;              /* Written by X280 */
    uint32_t x280_tx_tail;              /* Written by Host */
    uint32_t x280_rx_head;              /* Written by Host */
    uint32_t x280_rx_tail;              /* Written by X280 */
};

// To avoid:
// sudo ip link set tap0 up
// sudo ip addr add 192.168.9.1/24 dev tap0
static int setup_tap_interface(const char* dev_name, const char* ip_addr, int prefix_len)
{
    struct ifreq ifr;
    struct sockaddr_in addr;
    int sock;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    // Set IP address
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, dev_name, IFNAMSIZ - 1);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip_addr, &addr.sin_addr);

    memcpy(&ifr.ifr_addr, &addr, sizeof(struct sockaddr));
    if (ioctl(sock, SIOCSIFADDR, &ifr) < 0) {
        perror("ioctl(SIOCSIFADDR)");
        close(sock);
        return -1;
    }

    // Set netmask
    memset(&addr.sin_addr, 0, sizeof(addr.sin_addr));
    for (int i = 0; i < prefix_len; i++) {
        addr.sin_addr.s_addr |= htonl(1 << (31 - i));
    }
    memcpy(&ifr.ifr_netmask, &addr, sizeof(struct sockaddr));
    if (ioctl(sock, SIOCSIFNETMASK, &ifr) < 0) {
        perror("ioctl(SIOCSIFNETMASK)");
        close(sock);
        return -1;
    }

    // Bring interface up
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) < 0) {
        perror("ioctl(SIOCGIFFLAGS)");
        close(sock);
        return -1;
    }
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(sock, SIOCSIFFLAGS, &ifr) < 0) {
        perror("ioctl(SIOCSIFFLAGS)");
        close(sock);
        return -1;
    }

    close(sock);
    return 0;
}

static int tun_alloc(char* dev)
{
    struct ifreq ifr;
    int fd;

    fd = open("/dev/net/tun", O_RDWR);
    if (fd < 0) {
        perror("open(/dev/net/tun)");
        return fd;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TAP | IFF_NO_PI;
    if (dev && *dev)
        strncpy(ifr.ifr_name, dev, IFNAMSIZ - 1);

    if (ioctl(fd, TUNSETIFF, (void*)&ifr) < 0) {
        perror("ioctl(TUNSETIFF)");
        close(fd);
        return -1;
    }

    if (dev)
        strcpy(dev, ifr.ifr_name);

    return fd;
}

int main(int argc, char* argv[])
{
    BlackholePciDevice device("/dev/tenstorrent/0");

    // TODO: stop using 4G windows for this.  I'm using them out of laziness: I
    // don't have an effective way to pick an unused 2M window, and I don't want
    // to steal one from e.g. console.cpp ... nothing I am doing right now needs
    // a 4G window, so this will work for now.  If this were real code...
    auto window = device.map_tlb_4G(L2CPU_X, L2CPU_Y, X280_NET_BUFFERS);
    auto interrupt = device.map_tlb_4G(L2CPU_X, L2CPU_Y, X280_REGS);
    // volatile uint8_t* p = window->as<volatile uint8_t*>();
    auto shmem = window->as<x280_shmem_layout*>();
    // struct x280_shmem_layout *shmem = (struct x280_shmem_layout *)p;
    char tun_name[IFNAMSIZ] = "tap0";
    int tun_fd;
    fd_set readfds;
    char buffer[MAX_PACKET_SIZE];

    if (shmem->magic != X280_MAGIC) {
        fprintf(stderr, "Invalid magic number in shared memory\n");
        return 1;
    }

    tun_fd = tun_alloc(tun_name);
    if (tun_fd < 0)
        return 1;

    if (setup_tap_interface(tun_name, "192.168.9.1", 24) < 0) {
        close(tun_fd);
        return 1;
    }

    printf("Created TAP interface %s\n", tun_name);

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(tun_fd, &readfds);

        struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = 1,
        };

        if (select(tun_fd + 1, &readfds, NULL, NULL, &tv) < 0) {
            perror("select");
            break;
        }

        /* Check for packets from TAP */
        if (FD_ISSET(tun_fd, &readfds)) {
            ssize_t len = read(tun_fd, buffer, sizeof(buffer));
            if (len < 0) {
                perror("read");
                break;
            }

            uint32_t next_head = (shmem->x280_rx_head + 1) % NUM_PACKETS;
            if (next_head != shmem->x280_rx_tail) {
                shmem->x280_rx[shmem->x280_rx_head].len = len;
                memcpy(shmem->x280_rx[shmem->x280_rx_head].data, buffer, len);
                __atomic_thread_fence(__ATOMIC_RELEASE);
                shmem->x280_rx_head = next_head;
                interrupt->write32(0x404, 1 << 27);
            }
        }

        /* Check for packets from X280 */
        while (shmem->x280_tx_tail != shmem->x280_tx_head) {
            struct packet* pkt = &shmem->x280_tx[shmem->x280_tx_tail];
            if (pkt->len > 0 && pkt->len <= MAX_PACKET_SIZE) {
                if (write(tun_fd, pkt->data, pkt->len) < 0) {
                    printf("Failed writing packet\n");
                }
            }
            shmem->x280_tx_tail = (shmem->x280_tx_tail + 1) % NUM_PACKETS;
        }
    }

out:
    close(tun_fd);
    return 0;
}