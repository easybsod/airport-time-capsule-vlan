/* Inert Ethernet probes from an untagged Linux endpoint on LAN3.
 * No interface/VLAN/IP/route changes. One frame per test, fixed local MAC.
 * Upstream VLAN1002 should see only untagged and same-VID1002 tests;
 * native VID18, Guest VID1003, and unknown VID4094 must be dropped.
 */
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stddef.h>

/* Observe only our fixed downlink test MAC, for ten seconds. Seeing the
 * WAN1002 positive control untagged is required before interpreting absence
 * of the native/Guest probes as evidence of isolation. */
static int receive_downlink(int fd)
{
    const unsigned char source[] = {2, 0x52, 0x45, 0x53, 0x49, 2};
    unsigned char frame[2048];
    unsigned char control[CMSG_SPACE(sizeof(struct tpacket_auxdata))];
    time_t deadline = time(NULL) + 10;
    int n, enabled = 1;
    /* Linux may remove an 802.1Q header before AF_PACKET sees the frame.
     * The original VLAN is then in ancillary data, not frame[12:14]. */
    if (setsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &enabled, sizeof(enabled))) {
        perror("PACKET_AUXDATA"); return 1;
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    puts("DOWNLINK_CAPTURE_READY seconds=10 auxdata=enabled");
    while (time(NULL) < deadline) {
        fd_set fds;
        struct timeval timeout = {1, 0};
        FD_ZERO(&fds); FD_SET(fd, &fds);
        n = select(fd + 1, &fds, NULL, NULL, &timeout);
        if (n < 0) { perror("select"); return 1; }
        if (!n) continue;
        struct iovec iov = {frame, sizeof(frame)};
        struct msghdr message;
        struct cmsghdr *cmsg;
        struct tpacket_auxdata aux;
        unsigned vlan_valid = 0, vlan_tci = 0, vlan_tpid = 0;
        memset(&message, 0, sizeof(message));
        memset(&aux, 0, sizeof(aux));
        message.msg_iov = &iov; message.msg_iovlen = 1;
        message.msg_control = control; message.msg_controllen = sizeof(control);
        n = recvmsg(fd, &message, 0);
        if (n < 14 || memcmp(frame + 6, source, 6)) continue;
        if (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
            fputs("TRUNCATED_FRAME_OR_AUXDATA\n", stderr); return 1;
        }
        for (cmsg = CMSG_FIRSTHDR(&message); cmsg;
             cmsg = CMSG_NXTHDR(&message, cmsg)) {
            if (cmsg->cmsg_level == SOL_PACKET && cmsg->cmsg_type == PACKET_AUXDATA &&
                cmsg->cmsg_len >= CMSG_LEN(offsetof(struct tpacket_auxdata, tp_vlan_tci) + 2)) {
                size_t count = cmsg->cmsg_len - CMSG_LEN(0);
                if (count > sizeof(aux)) count = sizeof(aux);
                memcpy(&aux, CMSG_DATA(cmsg), count);
                vlan_valid = !!(aux.tp_status & TP_STATUS_VLAN_VALID) || !!aux.tp_vlan_tci;
                vlan_tci = aux.tp_vlan_tci;
                vlan_tpid = (aux.tp_status & TP_STATUS_VLAN_TPID_VALID) ? aux.tp_vlan_tpid :
                            (vlan_valid ? 0x8100 : 0);
            }
        }
        printf("DOWNLINK_FRAME bytes=%d ethertype=%02x%02x aux_vlan_valid=%u "
               "aux_vlan_tci=%u aux_vlan_tpid=%04x hex=", n,
               frame[12], frame[13], vlan_valid, vlan_tci, vlan_tpid);
        for (int i = 0; i < n; i++) printf("%02x", frame[i]);
        putchar('\n');
    }
    puts("DOWNLINK_CAPTURE_COMPLETE");
    return 0;
}

int main(int argc, char **argv)
{
    const unsigned vids[] = {0, 18, 1003, 1002, 4094};
    const unsigned char mac[] = {2, 0x52, 0x45, 0x53, 0x49, 1};
    unsigned char frame[96];
    struct sockaddr_ll addr;
    unsigned i, off;
    int fd;
    if (argc != 2 || (strcmp(argv[1], "send-five") &&
                      strcmp(argv[1], "receive-downlink"))) return 2;
    memset(&addr, 0, sizeof(addr));
    addr.sll_family = AF_PACKET;
    addr.sll_protocol = htons(3);
    addr.sll_ifindex = if_nametoindex("bridge");
    if (!addr.sll_ifindex) { perror("bridge"); return 1; }
    fd = socket(AF_PACKET, SOCK_RAW, htons(3));
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        perror("packet socket"); return 1;
    }
    if (!strcmp(argv[1], "receive-downlink")) {
        int rc = receive_downlink(fd);
        close(fd);
        return rc;
    }
    for (i = 0; i < sizeof(vids) / sizeof(vids[0]); i++) {
        memset(frame, 0, sizeof(frame));
        memset(frame, 255, 6);
        memcpy(frame + 6, mac, 6);
        off = 12;
        if (vids[i]) {
            frame[off++] = 0x81; frame[off++] = 0;
            frame[off++] = vids[i] >> 8; frame[off++] = vids[i];
        }
        frame[off++] = 0x88; frame[off++] = 0xb5;
        snprintf((char *)frame + off, sizeof(frame) - off,
                 "AIRPORT-ISOLATION-PROBE-%u", vids[i]);
        if (write(fd, frame, sizeof(frame)) != sizeof(frame)) {
            perror("send"); close(fd); return 1;
        }
        fprintf(stderr, "PROBE_SENT vid=%u bytes=%u\n", vids[i],
                (unsigned)sizeof(frame));
    }
    return close(fd) ? 1 : 0;
}
