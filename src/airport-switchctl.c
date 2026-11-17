/* RESEARCH ONLY -- NOT SAFE FOR PRODUCTION.
 * Critical 2026-11-16 fix: previous helper swapped SRAB ioctl directions!
 * 0x80106d03 is IOC_IN (WRITE); 0xc0106d02 is IOC_INOUT (READ).
 * The old 'read' tests consequently WROTE zero to page0 register0x0b,
 * disabling switch forwarding; this caused remote SSH to disappear.
 * See docs/srab-ioctl-direction.md. Old binaries MUST be removed.
 *
 * BCM5301x SRAB access for Apple AirPort NetBSD 6 (A1470).
 * The ioctl ABI is derived from this unit's model119 firmware; this is NOT
 * Linux's B53 ioctl interface. All actual SRAB access requires an explicit
 * command-line option. No write path is exposed while this is being audited.
 */
#ifndef AIRPORT_EXPLICIT_UNSAFE_RESEARCH
#error "This unverified SRAB helper is disabled: see README.md in this directory"
#endif
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>

/* Confirmed from NetBSD6 <sys/ioccom.h> and Apple's native esconfig:
 * - READ consumes selector and writes the result back into request.value.
 * - WRITE consumes selector+value. No WRITE operation is exposed.
 */
#define SRAB_READ  0xc0106d02UL
#define SRAB_WRITE_FORBIDDEN 0x80106d03UL
#define SRAB_LOCK  0x20006d00UL
#define SRAB_UNLOCK 0x20006d01UL
struct srab_request {
    uint32_t address_and_size;
    uint32_t reserved;
    uint64_t value;
};
typedef char assert_srab_ioctl_size[(sizeof(struct srab_request) == 16) ? 1 : -1];
typedef char assert_srab_value_offset[(offsetof(struct srab_request, value) == 8) ? 1 : -1];

static void checkpoint(const char *stage)
{
    fprintf(stderr, "SRAB_STAGE=%s\n", stage);
    fflush(stderr);
    /* stderr is expected to be a file on Flash when invoked by lab wrapper. */
    (void)fsync(STDERR_FILENO);
}

static int make_selector(unsigned long page, unsigned long reg, unsigned long bits,
                         uint32_t *selector)
{
    unsigned long type;
    if (page > 0xfe || reg > 0xff)
        return -1;
    switch (bits) {
    case 8: type = 0; break;
    case 16: type = 1; break;
    case 32: type = 2; break;
    case 48: type = 3; break;
    case 64: type = 4; break;
    default: return -1;
    }
    *selector = (uint32_t)((page << 24) | (reg << 16) | type);
    return 0;
}

static int access_register(int fd, unsigned long page, unsigned long reg,
                           unsigned long bits, uint64_t *value)
{
    struct srab_request request;
    memset(&request, 0, sizeof(request));
    if (make_selector(page, reg, bits, &request.address_and_size) != 0)
        return -1;
    checkpoint("IOCTL_READ_BEGIN");
    if (ioctl(fd, SRAB_READ, &request) != 0) {
        fprintf(stderr, "SRAB read page=0x%02lx reg=0x%02lx: %s\n",
                page, reg, strerror(errno));
        return -1;
    }
    checkpoint("IOCTL_READ_END");
    *value = request.value;
    return 0;
}

struct audit_probe {
    unsigned long page, reg, bits;
    const char *name;
};

/* All entries are read-only, non-volatile switch configuration registers.
 * Ports 0..3 refer to ASIC hardware port IDs, NOT confirmed RJ45 labels.
 * Apple's esconfig reports logical WAN/LAN labels; the native split profile
 * and observed link bitmap suggest ASIC port3 may be WAN. A real LAN3 cable
 * unplug at 14:15 on 2026-11-16 confirmed port2 is LAN3: link 0x012c->0x0128.
 * Do not generate VLAN WRITE values from an unverified physical mapping.
 * Do NOT add ARLIO VLAN table-operation registers: fetching those entries
 * needs control-register writes to select a VID and start the operation.
 */
static const struct audit_probe vlan_probes[] = {
    {0x00, 0x0b, 8, "switch_mode"},
    {0x34, 0x00, 8, "vlan_control0"},
    {0x34, 0x01, 8, "vlan_control1"},
    {0x34, 0x05, 8, "vlan_control4"},
    {0x34, 0x06, 8, "vlan_control5"},
    {0x34, 0x10, 16, "asic_port0_pvid"},
    {0x34, 0x12, 16, "asic_port1_pvid"},
    {0x34, 0x14, 16, "asic_port2_pvid"},
    {0x34, 0x16, 16, "asic_port3_pvid"},
    {0x31, 0x00, 16, "asic_port0_pvlan_mask"},
    {0x31, 0x02, 16, "asic_port1_pvlan_mask"},
    {0x31, 0x04, 16, "asic_port2_pvlan_mask"},
    {0x31, 0x06, 16, "asic_port3_pvlan_mask"},
    {0x34, 0x1a, 16, "port5_pvid"},
    {0x34, 0x20, 16, "port8_pvid"},
    {0x31, 0x0a, 16, "port5_pvlan_mask"},
    {0x31, 0x10, 16, "port8_pvlan_mask"},
    /* ARL/VTBL *scratch/control* registers: READ only, never start a command. */
    {0x05, 0x80, 8, "vtbl_access_state"},
    {0x05, 0x81, 16, "vtbl_index_state"},
    {0x05, 0x83, 32, "vtbl_entry_latch"},
    /* B53 MIB counters. Page 0x20 + ASIC port; 0x00=TxOctets,
     * 0x50=RxOctets for BCM5301x. No counter reset/clear operations.
     * These are 64-bit reads, printed as two 32-bit words to avoid
     * potentially unreliable varargs of cross-linked NetBSD libc.
     */
    {0x22, 0x00, 64, "asic_port2_tx_octets"},
    {0x22, 0x50, 64, "asic_port2_rx_octets"},
    {0x23, 0x00, 64, "asic_port3_tx_octets"},
    {0x23, 0x50, 64, "asic_port3_rx_octets"},
    {0x25, 0x00, 64, "asic_port5_tx_octets"},
    {0x25, 0x50, 64, "asic_port5_rx_octets"},
    {0x28, 0x00, 64, "asic_port8_tx_octets"},
    {0x28, 0x50, 64, "asic_port8_rx_octets"},
    /* BCM5301x management page mirror registers (read-only preflight). */
    {0x02, 0x10, 16, "mirror_capture_control"},
    {0x02, 0x12, 16, "mirror_ingress_mask"},
    {0x02, 0x1c, 16, "mirror_egress_mask"},
};

static int audit_vlan_registers(int fd, int group)
{
    unsigned long i;
    for (i = 0; i < sizeof(vlan_probes) / sizeof(vlan_probes[0]); i++) {
        const struct audit_probe *p = &vlan_probes[i];
        uint64_t v = 0;
        /* 4- or 5-register bounded batches, preserving Flash checkpoints. */
        if ((group == 1 && (i < 5 || i >= 9)) ||
            (group == 2 && (i < 9 || i >= 13)) ||
            (group == 3 && i >= 5) ||
            (group == 4 && (i < 13 || i >= 17)) ||
            (group == 5 && (i < 17 || i >= 20)) ||
            (group == 6 && (i < 20 || i >= 24)) ||
            (group == 7 && (i < 24 || i >= 28)) ||
            (group == 8 && i < 28))
            continue;
        fprintf(stderr, "AUDIT_BEGIN name=%s page=0x%02lx reg=0x%02lx bits=%lu\n",
                p->name, p->page, p->reg, p->bits);
        fflush(stderr);
        (void)fsync(STDERR_FILENO);
        if (access_register(fd, p->page, p->reg, p->bits, &v) != 0) {
            fprintf(stderr, "AUDIT_FAILED name=%s\n", p->name);
            return -1;
        }
        fprintf(stderr, "AUDIT_VALUE name=%s page=0x%02lx reg=0x%02lx bits=%lu value=0x%08lx%08lx\n",
                p->name, p->page, p->reg, p->bits,
                (unsigned long)((uint32_t)(v >> 32)),
                (unsigned long)((uint32_t)v));
        fflush(stderr);
        (void)fsync(STDERR_FILENO);
    }
    return 0;
}

static int number(const char *arg, unsigned long *result)
{
    char *end;
    errno = 0;
    *result = strtoul(arg, &end, 0);
    return (errno || end == arg || *end != '\0') ? -1 : 0;
}

int main(int argc, char **argv)
{
    unsigned long page, reg, bits;
    uint32_t selector;
    uint64_t data = 0;
    int fd;

    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "abi") == 0) {
        fprintf(stderr, "NetBSD6 ioctl_lock=0x%08lx unlock=0x%08lx read=0x%08lx write_FORBIDDEN=0x%08lx size=%lu value_offset=%lu\n",
               (unsigned long)SRAB_LOCK, (unsigned long)SRAB_UNLOCK,
               (unsigned long)SRAB_READ, (unsigned long)SRAB_WRITE_FORBIDDEN,
               (unsigned long)sizeof(struct srab_request),
               (unsigned long)offsetof(struct srab_request, value));
        return 0;
    }
    /* The first live stage opens and closes the SRAB device without ioctl. */
    int open_only = argc == 2 && strcmp(argv[1], "open-only") == 0;
    int audit_vlan = argc == 2 &&
        (strcmp(argv[1], "audit-vlan") == 0 ||
         strcmp(argv[1], "audit-pvid") == 0 ||
         strcmp(argv[1], "audit-pvlan") == 0 ||
         strcmp(argv[1], "audit-cpuports") == 0 ||
         strcmp(argv[1], "audit-vtbl-state") == 0 ||
         strcmp(argv[1], "audit-mib-ports") == 0 ||
         strcmp(argv[1], "audit-mib-internal") == 0 ||
         strcmp(argv[1], "audit-mirror") == 0);
    int audit_group = (audit_vlan && strcmp(argv[1], "audit-pvid") == 0) ? 1 :
                      (audit_vlan && strcmp(argv[1], "audit-pvlan") == 0) ? 2 :
                      (audit_vlan && strcmp(argv[1], "audit-cpuports") == 0) ? 4 :
                      (audit_vlan && strcmp(argv[1], "audit-vtbl-state") == 0) ? 5 :
                      (audit_vlan && strcmp(argv[1], "audit-mib-ports") == 0) ? 6 :
                      (audit_vlan && strcmp(argv[1], "audit-mib-internal") == 0) ? 7 :
                      (audit_vlan && strcmp(argv[1], "audit-mirror") == 0) ? 8 : 3;
    if (!open_only && !audit_vlan && (argc != 5 ||
        (strcmp(argv[1], "read-locked") != 0 && strcmp(argv[1], "validate") != 0))) {
        fprintf(stderr, "usage: %s abi | open-only | audit-vlan | audit-pvid | audit-pvlan | audit-cpuports | audit-vtbl-state | audit-mib-ports | audit-mib-internal | audit-mirror | validate <page> <reg> <bits> | read-locked <page> <reg> <bits>\n",
                argv[0]);
        return 2;
    }
    if (!open_only && !audit_vlan && (number(argv[2], &page) || number(argv[3], &reg) ||
        number(argv[4], &bits) || make_selector(page, reg, bits, &selector) != 0)) {
        fprintf(stderr, "invalid SRAB address or data length\n");
        return 2;
    }
    if (!open_only && strcmp(argv[1], "validate") == 0) {
        fprintf(stderr, "SRAB validation only: selector=0x%08lx; NO device opened\n",
                (unsigned long)selector);
        return 0;
    }
    /* Direct invocation is unsafe: require an armed independent watchdog.
     * The wrapper sets the environment variable only after it has recorded
     * the watchdog marker to persistent Flash.
     */
    if (getenv("AIRPORT_ALLOW_SRAB_READ") == NULL ||
        strcmp(getenv("AIRPORT_ALLOW_SRAB_READ"), "ARMED_WITH_RECOVERY") != 0 ||
        access("/mnt/Flash/airport-srab-lab/armed", F_OK) != 0) {
        fprintf(stderr, "BLOCKED: use srab-safety.sh run-read (armed recovery required)\n");
        return 2;
    }
    /* DO NOT use alarm() with this cross-linked NetBSD libc.
     * A no-SRAB self-test demonstrated premature SIGALRM and nonsensical
     * return values. Recovery is enforced by srab-safety.sh's independent
     * one-shot reboot process and external SSH acknowledgement.
     * An uninterruptible kernel SRAB spin is not guaranteed recoverable.
     */
    checkpoint("OPEN_BEGIN");
    fd = open("/dev/bcmsrab0", O_RDONLY);
    if (fd < 0) {
        perror("open /dev/bcmsrab0");
        checkpoint("OPEN_FAILED");
        return 1;
    }
    checkpoint("OPEN_END");
    if (open_only) {
        checkpoint("CLOSE_BEGIN");
        if (close(fd) < 0) {
            perror("close /dev/bcmsrab0");
            return 1;
        }
        checkpoint("DONE_OPEN_ONLY");
        return 0;
    }
    checkpoint("LOCK_BEGIN");
    if (ioctl(fd, SRAB_LOCK, NULL) != 0) {
        perror("SRAB lock");
        checkpoint("LOCK_FAILED");
        close(fd);
        return 1;
    }
    checkpoint("LOCK_ACQUIRED");
    /* Best-effort only. Kernel SRAB busy loops are not necessarily interruptible. */
    int read_failed = audit_vlan ? audit_vlan_registers(fd, audit_group) :
        access_register(fd, page, reg, bits, &data);
    checkpoint("UNLOCK_BEGIN");
    int unlock_failed = ioctl(fd, SRAB_UNLOCK, NULL) != 0;
    if (unlock_failed)
        perror("SRAB unlock");
    else
        checkpoint("UNLOCK_END");
    if (read_failed || unlock_failed) {
        checkpoint("IOCTL_FAILED");
        close(fd);
        return 1;
    }
    /* Stderr deliberately used: stdout printf() is not reliable with this
     * cross-linked NetBSD 6 runtime; stderr is captured by the lab wrapper. */
    if (!audit_vlan)
        fprintf(stderr, "%s page=0x%02lx reg=0x%02lx width=%lu value=0x%016" PRIx64 "\n",
               "read", page, reg, bits, data);
    else
        checkpoint("AUDIT_VLAN_COMPLETE_READ_ONLY");
    checkpoint("CLOSE_BEGIN");
    if (close(fd) < 0) {
        perror("close /dev/bcmsrab0");
        return 1;
    }
    checkpoint("DONE");
    return 0;
}
