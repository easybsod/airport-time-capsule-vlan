/* Fixed inert L2 broadcasts for the LAN3 downlink isolation test.
 * Native and Guest probes must NOT reach LAN3. A separate upstream host
 * supplies the VLAN1002 positive control, which must arrive untagged.
 * No SRAB, interface configuration, IP traffic, or arbitrary packet CLI.
 */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <net/if.h>
#include <net/bpf.h>
#include <net/dlt.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const unsigned vids[] = {0, 1003};
    const unsigned char mac[] = {2, 0x52, 0x45, 0x53, 0x49, 2};
    struct ifreq ifr;
    unsigned char frame[96];
    unsigned dlt = 0, enable = 1, off, i;
    int fd;
    if (argc != 2 || strcmp(argv[1], "send-native-guest")) return 2;
    fd = open("/dev/bpf", O_RDWR);
    if (fd < 0) { perror("bpf"); return 1; }
    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "bcmeth0");
    if (ioctl(fd, BIOCSETIF, &ifr) || ioctl(fd, BIOCGDLT, &dlt) ||
        dlt != DLT_EN10MB || ioctl(fd, BIOCSHDRCMPLT, &enable)) {
        perror("BPF setup"); close(fd); return 1;
    }
    for (i = 0; i < sizeof(vids) / sizeof(vids[0]); i++) {
        memset(frame, 0, sizeof(frame));
        memset(frame, 255, 6); memcpy(frame + 6, mac, 6);
        off = 12;
        if (vids[i]) {
            frame[off++] = 0x81; frame[off++] = 0;
            frame[off++] = vids[i] >> 8; frame[off++] = vids[i];
        }
        frame[off++] = 0x88; frame[off++] = 0xb5;
        snprintf((char *)frame + off, sizeof(frame) - off,
                 "AIRPORT-DOWNLINK-%s", vids[i] ? "GUEST1003" : "NATIVE18");
        if (write(fd, frame, sizeof(frame)) != sizeof(frame)) {
            perror("BPF write"); close(fd); return 1;
        }
        /* This NetBSD6 SDK's stdout linkage is unreliable; match the other
         * AirPort helpers and write diagnostics to stderr. */
        fprintf(stderr, "DOWNLINK_PROBE_SENT domain=%s bytes=%u\n",
               vids[i] ? "GUEST1003" : "NATIVE18", (unsigned)sizeof(frame));
    }
    return close(fd) ? 1 : 0;
}
