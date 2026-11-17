/*
 * EXPERIMENTAL: limited BCM5301x ARL/VTBL table lookup for Apple NetBSD6.
 *
 * Unlike airport-switchctl.c, a table lookup is NOT strictly register-read-
 * only: querying the ASIC requires writing a 12-bit VID to the VTA index
 * scratch register, followed by the VTA_READ command (0x81). Those are the
 * ONLY hardware WRITE operations implemented here. There is NO VLAN TABLE
 * WRITE (0x80), CLEAR (0x82), VLAN entry update, or arbitrary-register CLI.
 *
 * Requires independent reboot timer, verified ROM baseline and external SSH
 * acknowledgment. May still disconnect management or need a power cycle.
 */
#ifndef AIRPORT_EXPLICIT_UNSAFE_RESEARCH
#error "VLAN table query is disabled by default; see docs/BUILDING.md"
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

#define SRAB_READ 0xc0106d02UL
#define SRAB_WRITE 0x80106d03UL
#define SRAB_LOCK 0x20006d00UL
#define SRAB_UNLOCK 0x20006d01UL

#define SELECT_SWITCH_MODE 0x000b0000UL
#define SELECT_VLAN_CTRL0  0x34000000UL
#define SELECT_ACCESS      0x05800000UL
#define SELECT_INDEX       0x05810001UL
#define SELECT_ENTRY       0x05830002UL
#define SELECT_PORT3_PVID   0x34160001UL /* ASIC port3; WAN is probable, not proven */
#define SELECT_PORT3_MASK   0x31060001UL

#define EXPECTED_SWITCH_MODE 0x07UL
#define EXPECTED_VLAN_CTRL0  0xe3UL
#define EXPECTED_INDEX       0x12UL
#define EXPECTED_ENTRY       0x00221f0fUL
#define EXPECTED_PORT3_PVID   0x12UL
#define EXPECTED_PORT3_MASK   0x010fUL

struct request {
    uint32_t selector;
    uint32_t reserved;
    uint64_t value;
};
typedef char assert_req_size[(sizeof(struct request) == 16) ? 1 : -1];
typedef char assert_req_value_offset[(offsetof(struct request, value) == 8) ? 1 : -1];

static void checkpoint(const char *stage)
{
    fprintf(stderr, "VTBL_STAGE=%s\n", stage);
    fflush(stderr);
    (void)fsync(STDERR_FILENO);
}

static int read_register(int fd, uint32_t sel, uint64_t *result)
{
    struct request req;
    memset(&req, 0, sizeof(req));
    req.selector = sel;
    if (ioctl(fd, SRAB_READ, &req) != 0) {
        fprintf(stderr, "VTBL_READ_FAILED selector=0x%08" PRIx32 " errno=%d\n", sel, errno);
        return -1;
    }
    *result = req.value;
    return 0;
}

static int write_index(int fd, uint16_t vid)
{
    struct request req;
    memset(&req, 0, sizeof(req));
    req.selector = SELECT_INDEX;
    req.value = vid; /* Only hardware scratch VLAN Table Index register */
    checkpoint("WRITE_INDEX");
    return ioctl(fd, SRAB_WRITE, &req);
}

static int start_read_command(int fd)
{
    struct request req;
    memset(&req, 0, sizeof(req));
    req.selector = SELECT_ACCESS;
    req.value = 0x81; /* bit7=START; bit1:0=READ. NEVER 0x80/0x82 */
    checkpoint("WRITE_READ_COMMAND");
    return ioctl(fd, SRAB_WRITE, &req);
}

static int check_baseline(int fd)
{
    static const struct {
        uint32_t selector;
        uint64_t expected;
        const char *name;
    } baseline[] = {
        {SELECT_SWITCH_MODE, EXPECTED_SWITCH_MODE, "switch_mode"},
        {SELECT_VLAN_CTRL0, EXPECTED_VLAN_CTRL0, "vlan_ctrl0"},
        {SELECT_INDEX, EXPECTED_INDEX, "vtbl_index"},
        {SELECT_PORT3_PVID, EXPECTED_PORT3_PVID, "asic_port3_pvid"},
        {SELECT_PORT3_MASK, EXPECTED_PORT3_MASK, "asic_port3_mask"},
    };
    unsigned long i;
    for (i = 0; i < sizeof(baseline) / sizeof(baseline[0]); i++) {
        uint64_t value = 0;
        if (read_register(fd, baseline[i].selector, &value) != 0)
            return -1;
        /* Avoid EABI4/EABI5 varargs disagreement for 64-bit printf fields. */
        fprintf(stderr, "VTBL_BASELINE %s=0x%08" PRIx32 " expected=0x%08" PRIx32 "\n",
                baseline[i].name, (uint32_t)value, (uint32_t)baseline[i].expected);
        if (value != baseline[i].expected) {
            checkpoint("BASELINE_MISMATCH_ABORT_NO_WRITES");
            return -1;
        }
    }
    /* Following a previous READ operation, access may retain opcode 0x01.
     * Do not require a reboot or modify access just to reset that opcode.
     * START must be clear and no other access flags are accepted.
     */
    {
        uint64_t access_value = 0, latch = 0;
        if (read_register(fd, SELECT_ACCESS, &access_value) != 0 ||
            read_register(fd, SELECT_ENTRY, &latch) != 0)
            return -1;
        fprintf(stderr, "VTBL_BASELINE vtbl_access=0x%08" PRIx32
                " vtbl_entry_latch=0x%08" PRIx32 "\n",
                (uint32_t)access_value, (uint32_t)latch);
        if ((access_value != 0 && access_value != 1) ||
            (access_value == 0 && latch != EXPECTED_ENTRY)) {
            checkpoint("BASELINE_VTBL_SCRATCH_MISMATCH_ABORT_NO_WRITES");
            return -1;
        }
    }
    checkpoint("BASELINE_MATCH");
    return 0;
}

static int query(int fd, uint16_t vid)
{
    uint64_t current = 0, result = 0;
    unsigned int polls;
    int rc = -1;
    int index_modified = 0;

    if (check_baseline(fd) != 0)
        return -1;
    if (write_index(fd, vid) != 0) {
        fprintf(stderr, "VTBL_ERROR index_write errno=%d\n", errno);
        /* Attempt restore even if the write returned an error. */
        index_modified = 1;
        goto restore;
    }
    index_modified = 1;
    if (read_register(fd, SELECT_INDEX, &current) != 0 || current != vid) {
        checkpoint("INDEX_VERIFY_FAILED");
        goto restore;
    }
    checkpoint("INDEX_VERIFIED");
    if (start_read_command(fd) != 0) {
        fprintf(stderr, "VTBL_ERROR table_read_command errno=%d\n", errno);
        goto restore;
    }
    for (polls = 0; polls < 100; polls++) {
        if (read_register(fd, SELECT_ACCESS, &current) != 0)
            goto restore;
        if ((current & 0x80) == 0) {
            checkpoint("READ_COMMAND_COMPLETED");
            break;
        }
    }
    if (polls == 100 || (current & 0x7f) != 0x01) {
        checkpoint("READ_COMMAND_TIMEOUT_OR_BAD_STATE");
        goto restore;
    }
    if (read_register(fd, SELECT_ENTRY, &result) != 0)
        goto restore;
    fprintf(stderr, "VTBL_RESULT vid=%u raw=0x%08" PRIx32
            " members=0x%03" PRIx32 " untag=0x%03" PRIx32
            " additional=0x%08" PRIx32 "\n",
            (unsigned)vid, (uint32_t)result,
            (uint32_t)result & 0x1ff, ((uint32_t)result >> 9) & 0x1ff,
            (uint32_t)result & ~0x3ffffU);
    fflush(stderr);
    (void)fsync(STDERR_FILENO);
    /* LAN3 may already be isolated by the temporary cutover. Both states
     * are observed baselines; querying must never try to restore entries. */
    if (vid == 18 && result != EXPECTED_ENTRY && result != 0x0022170bUL) {
        checkpoint("VID18_ENTRY_UNEXPECTED_ABORT");
    } else {
        rc = 0;
    }
restore:
    if (index_modified) {
        checkpoint("RESTORE_PREVIOUS_INDEX_BEGIN");
        if (write_index(fd, EXPECTED_INDEX) != 0 ||
            read_register(fd, SELECT_INDEX, &current) != 0 ||
            current != EXPECTED_INDEX) {
            checkpoint("RESTORE_PREVIOUS_INDEX_FAILED");
            return -1;
        }
        checkpoint("RESTORE_PREVIOUS_INDEX_DONE");
    }
    return rc;
}

int main(int argc, char **argv)
{
    int fd, rc = 1, lock_acquired = 0;
    uint16_t vid = 0;

    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc == 2 && strcmp(argv[1], "abi") == 0) {
        fprintf(stderr, "VTBL_HELPER_ABI=1 read=0xc0106d02 write=0x80106d03 "
                "index_selector=0x05810001 access_selector=0x05800000 "
                "access_command=0x81 allowed_vids=18,1002,1003 "
                "request_size=%lu value_offset=%lu\n",
                (unsigned long)sizeof(struct request),
                (unsigned long)offsetof(struct request, value));
        return 0;
    }
    if (argc != 3 || strcmp(argv[1], "query") != 0 ||
        (strcmp(argv[2], "18") != 0 &&
         strcmp(argv[2], "1002") != 0 && strcmp(argv[2], "1003") != 0)) {
        fprintf(stderr, "usage: %s abi | query 18|1002|1003\n", argv[0]);
        return 2;
    }
    vid = (uint16_t)strtoul(argv[2], NULL, 10);
    if (getenv("AIRPORT_ALLOW_VLAN_TABLE_QUERY") == NULL ||
        strcmp(getenv("AIRPORT_ALLOW_VLAN_TABLE_QUERY"), "ARMED_WITH_RECOVERY") != 0 ||
        access("/mnt/Flash/airport-srab-lab/armed", F_OK) != 0) {
        checkpoint("BLOCKED_NO_ARMED_EXTERNAL_RECOVERY");
        return 2;
    }

    checkpoint("OPEN_BEGIN");
    fd = open("/dev/bcmsrab0", O_RDONLY);
    if (fd < 0) {
        perror("VTBL open");
        return 1;
    }
    checkpoint("LOCK_BEGIN");
    if (ioctl(fd, SRAB_LOCK, NULL) != 0) {
        perror("VTBL lock");
        goto done;
    }
    lock_acquired = 1;
    checkpoint("LOCK_ACQUIRED");
    rc = query(fd, vid) == 0 ? 0 : 1;
    if (rc == 0)
        checkpoint("QUERY_COMPLETED");
    else
        checkpoint("QUERY_FAILED");
done:
    if (lock_acquired) {
        checkpoint("UNLOCK_BEGIN");
        if (ioctl(fd, SRAB_UNLOCK, NULL) != 0) {
            perror("VTBL unlock");
            rc = 1;
        } else {
            checkpoint("UNLOCK_END");
        }
    }
    if (close(fd) != 0)
        rc = 1;
    checkpoint(rc == 0 ? "DONE" : "FAILED");
    return rc;
}
