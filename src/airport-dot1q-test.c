/*
 * Reversible three-VLAN test using standard 802.1Q (DT disabled).
 * A detached native-Shell worker MUST restore the baseline on its deadline.
 * An independent reboot watchdog MUST remain armed until a fresh SSH ACK.
 * NO arbitrary CLI writes or destructive table clear. The separately built
 * service variant uses the exact same hardware values with its own bounded
 * startup/monitor transaction, not the research watchdog.
 */
#ifndef AIRPORT_EXPLICIT_UNSAFE_RESEARCH
#error "Explicit research-only build required"
#endif
#include <sys/types.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stddef.h>
#include <inttypes.h>

#define IO_READ   0xc0106d02UL
#define IO_WRITE  0x80106d03UL
#define IO_LOCK   0x20006d00UL
#define IO_UNLOCK 0x20006d01UL
#define SWMODE    0x000b0000U
#define CTRL0     0x34000000U
#define CTRL4     0x34050000U
#define CTRL5     0x34060000U
#define ACCESS    0x05800000U
#define INDEX     0x05810001U
#define ENTRY     0x05830002U
#define PVID2     0x34140001U
#define PVLAN2    0x31040001U
#define PVID3     0x34160001U
#define PVLAN3    0x31060001U
#define PVID8     0x34200001U
#define PVLAN8    0x31100001U
#define NATIVE_VID 18U
#define TEST_VID 1002U
#define GUEST_VID 1003U
#define NATIVE_ENTRY 0x00221f0fU
#define CANDIDATE_ENTRY 0x0020080cU
#define NATIVE_TARGET_ENTRY 0x0022170bU
#define LAN3_TARGET_PVID 1002U
#define LAN3_TARGET_MASK 0x00cU
#define GUEST_ENTRY 0x00200108U

struct request { uint32_t selector, reserved; uint64_t value; };
typedef char assert_size[(sizeof(struct request) == 16) ? 1 : -1];
typedef char assert_off[(offsetof(struct request, value) == 8) ? 1 : -1];
static void stage(const char *s)
{
    fprintf(stderr, "WRITE_PROBE_STAGE=%s\n", s);
    fflush(stderr);
    (void)fsync(2);
}
static int rd(int fd, uint32_t selector, uint32_t *out)
{
    struct request req;
    memset(&req, 0, sizeof(req));
    req.selector = selector;
    if (ioctl(fd, IO_READ, &req) != 0 || (req.value >> 32) != 0) {
        fprintf(stderr, "SRAB_READ_FAILED selector=0x%08" PRIx32 " errno=%d\n", selector, errno);
        return -1;
    }
    *out = (uint32_t)req.value;
    return 0;
}
/* The ONLY hardware register write function. The program has no write CLI
 * and uses only fixed INDEX, ENTRY and ACCESS selectors. */
static int wr(int fd, uint32_t selector, uint32_t value)
{
    struct request req;
    memset(&req, 0, sizeof(req));
    req.selector = selector;
    req.value = value;
    if (ioctl(fd, IO_WRITE, &req) != 0) {
        fprintf(stderr, "SRAB_WRITE_FAILED selector=0x%08" PRIx32 " errno=%d\n", selector, errno);
        return -1;
    }
    return 0;
}
static int wait_vta(int fd, uint32_t expected)
{
    uint32_t v = 0;
    unsigned int i;
    for (i = 0; i < 100; i++) {
        if (rd(fd, ACCESS, &v) != 0) return -1;
        if (!(v & 0x80)) {
            if (v == expected) return 0;
            fprintf(stderr, "UNEXPECTED_VTA=0x%08" PRIx32 "\n", v);
            return -1;
        }
    }
    stage("VTA_TIMEOUT");
    return -1;
}
static int read_vid(int fd, uint16_t vid, uint32_t *value)
{
    uint32_t check = 0;
    if (wr(fd, INDEX, vid) ||
        rd(fd, INDEX, &check) || check != vid ||
        wr(fd, ACCESS, 0x81) ||
        wait_vta(fd, 0x01) ||
        rd(fd, ENTRY, value)) return -1;
    return 0;
}
static int write_table_entry(int fd, uint16_t vid, uint32_t value)
{
    uint32_t check = 0;
    if (!((vid == TEST_VID && (value == 0 || value == CANDIDATE_ENTRY)) ||
          (vid == NATIVE_VID && (value == NATIVE_ENTRY || value == NATIVE_TARGET_ENTRY)) ||
          (vid == GUEST_VID && (value == 0 || value == GUEST_ENTRY)))) {
        stage("REJECTED_TABLE_VID_OR_VALUE_NOT_WHITELISTED");
        return -1;
    }
    if (wr(fd, INDEX, vid) ||
        rd(fd, INDEX, &check) || check != vid) return -1;
    fprintf(stderr, "TABLE_WRITE_WHITELISTED vid=%u value=0x%08" PRIx32 "\n",
            (unsigned)vid, value);
    if (wr(fd, ENTRY, value)) return -1;
    /* Apple esconfig and B53 both issue 0x80 = VTA START | WRITE.
     * Do not issue 0x82 (VLAN table clear). */
    stage("TABLE_WRITE_START");
    if (wr(fd, ACCESS, 0x80) || wait_vta(fd, 0x00)) return -1;
    stage("TABLE_WRITE_DONE");
    if (read_vid(fd, vid, &check) != 0 || check != value) {
        stage("TABLE_WRITE_READBACK_MISMATCH");
        return -1;
    }
    return 0;
}
static int write_port2(int fd, uint32_t reg, uint32_t value)
{
    uint32_t check = 0;
    if (!((reg == PVID2 && (value == 18 || value == LAN3_TARGET_PVID)) ||
          (reg == PVLAN2 && (value == 0x10f || value == LAN3_TARGET_MASK)))) {
        stage("REJECTED_PORT_REGISTER_OR_VALUE_NOT_WHITELISTED");
        return -1;
    }
    fprintf(stderr, "PORT2_WRITE_WHITELISTED selector=0x%08" PRIx32
            " value=0x%08" PRIx32 "\n", reg, value);
    if (wr(fd, reg, value) != 0 ||
        rd(fd, reg, &check) != 0 || check != value) {
        stage("PORT2_WRITE_READBACK_FAILED");
        return -1;
    }
    return 0;
}
static int check_reg(int fd, uint32_t selector, uint32_t expected, const char *name)
{
    uint32_t got = 0;
    if (rd(fd, selector, &got)) return -1;
    fprintf(stderr, "BASELINE %s=0x%08" PRIx32 " expected=0x%08" PRIx32 "\n",
            name, got, expected);
    if (got != expected) {
        stage("BASELINE_MISMATCH_STOP_BEFORE_TABLE_WRITE");
        return -1;
    }
    return 0;
}
static int verify_table(int fd, uint16_t vid, uint32_t expected, const char *name)
{
    uint32_t entry = 0;
    if (read_vid(fd, vid, &entry) != 0 || entry != expected) {
        fprintf(stderr, "VERIFY_TABLE_FAILED name=%s expected=0x%08" PRIx32
                " actual=0x%08" PRIx32 "\n", name, expected, entry);
        return -1;
    }
    fprintf(stderr, "TABLE_READBACK name=%s vid=%u actual=0x%08" PRIx32 "\n",
            name, (unsigned)vid, entry);
    return 0;
}

static int set_dt(int fd, uint32_t value)
{
    /* 0xc0 is accepted only for restoring the first, permissive experiment.
     * The active configuration drops source-membership violations (0x40). */
    if (value != 0xc4 && value != 0xc0 && value != 0x40) return -1;
    if (wr(fd, CTRL4, value)) return -1;
    return check_reg(fd, CTRL4, value, "DT_MODE_READBACK");
}

static int set_miss_policy(int fd, uint32_t value)
{
    if (value != 0x10 && value != 0x18) return -1;
    if (wr(fd, CTRL5, value)) return -1;
    return check_reg(fd, CTRL5, value, "VTABLE_MISS_POLICY");
}

static int verify(int fd, int active)
{
    return check_reg(fd, CTRL4, active ? 0x40 : 0xc4, "DT_MODE_SOURCE_POLICY") ||
        check_reg(fd, CTRL5, active ? 0x18 : 0x10, "VTABLE_MISS_POLICY") ||
        verify_table(fd, TEST_VID, active ? CANDIDATE_ENTRY : 0, "ACCESS1002") ||
        verify_table(fd, GUEST_VID, active ? GUEST_ENTRY : 0, "GUEST1003") ||
        check_reg(fd, PVID2, active ? 1002 : 18, "LAN3_PVID") ||
        check_reg(fd, PVLAN2, active ? 0xc : 0x10f, "LAN3_MASK") ||
        check_reg(fd, PVID3, 18, "WAN_PVID") ||
        check_reg(fd, PVLAN3, 0x10f, "WAN_MASK") ||
        check_reg(fd, PVID8, 18, "CPU_PVID") ||
        check_reg(fd, PVLAN8, 0x10f, "CPU_MASK") ||
        verify_table(fd, NATIVE_VID,
                     active ? NATIVE_TARGET_ENTRY : NATIVE_ENTRY, "NATIVE18");
}

static int restore(int fd)
{
    uint32_t ctrl, miss, pvid, mask, native, access, guest;
    int fail = 0;
    if (rd(fd, CTRL4, &ctrl) || rd(fd, CTRL5, &miss) || rd(fd, PVID2, &pvid) ||
        rd(fd, PVLAN2, &mask) || read_vid(fd, 18, &native) ||
        read_vid(fd, 1002, &access) || read_vid(fd, 1003, &guest)) return -1;
    if ((ctrl != 0xc0 && ctrl != 0xc4 && ctrl != 0x40) ||
        (miss != 0x10 && miss != 0x18) || (pvid != 18 && pvid != 1002) ||
        (mask != 0xc && mask != 0x10f) ||
        (native != NATIVE_ENTRY && native != NATIVE_TARGET_ENTRY) ||
        (access != 0 && access != CANDIDATE_ENTRY) ||
        (guest != 0 && guest != GUEST_ENTRY)) {
        stage("UNKNOWN_PRESTATE_REFUSE_RESTORE"); return -1;
    }
    stage("DOT1Q_RESTORE_BEGIN");
    if (set_dt(fd, 0xc4)) fail = 1;
    if (set_miss_policy(fd, 0x10)) fail = 1;
    if (write_port2(fd, PVID2, 18)) fail = 1;
    if (write_port2(fd, PVLAN2, 0x10f)) fail = 1;
    if (write_table_entry(fd, 18, NATIVE_ENTRY)) fail = 1;
    if (write_table_entry(fd, 1002, 0)) fail = 1;
    if (write_table_entry(fd, 1003, 0)) fail = 1;
    if (verify(fd, 0)) fail = 1;
    stage(fail ? "DOT1Q_RESTORE_FAILED" : "DOT1Q_RESTORE_VERIFIED");
    return fail ? -1 : 0;
}

static int apply(int fd)
{
    if (verify(fd, 0)) return -1;
    stage("DOT1Q_APPLY_BEGIN");
    if (write_table_entry(fd, 1003, GUEST_ENTRY) ||
        write_table_entry(fd, 1002, CANDIDATE_ENTRY) ||
        write_table_entry(fd, 18, NATIVE_TARGET_ENTRY) ||
        write_port2(fd, PVLAN2, 0xc) || write_port2(fd, PVID2, 1002) ||
        set_miss_policy(fd, 0x18) || set_dt(fd, 0x40) || verify(fd, 1)) {
        stage("DOT1Q_APPLY_FAILED_RESTORING");
        (void)restore(fd);
        return -1;
    }
    stage("DOT1Q_APPLY_VERIFIED");
    return 0;
}

int main(int argc, char **argv)
{
    int fd, rc, unlock_rc;
    uint32_t miss;
    setvbuf(stderr, NULL, _IONBF, 0);
#ifdef AIRPORT_THREE_VLAN_SERVICE
    if (argc == 2 && !strcmp(argv[1], "abi")) {
        fprintf(stderr, "AIRPORT_THREE_VLAN_ABI=1 config=strict-8021q-v1 read=0xc0106d02 write=0x80106d03 size=16\n");
        return 0;
    }
#endif
    if (argc != 2 || (strcmp(argv[1], "apply") && strcmp(argv[1], "restore") &&
                      strcmp(argv[1], "verify") && strcmp(argv[1], "baseline"))) return 2;
#ifdef AIRPORT_THREE_VLAN_SERVICE
    if (!getenv("AIRPORT_THREE_VLAN_GUARDED") ||
        strcmp(getenv("AIRPORT_THREE_VLAN_GUARDED"), "BOUNDED_SERVICE_TRANSACTION") ||
        access("/mnt/Memory/airport-three-vlan/armed", F_OK)) return 2;
#else
    if (!getenv("AIRPORT_DOT1Q_GUARDED") ||
        strcmp(getenv("AIRPORT_DOT1Q_GUARDED"), "DETACHED_ROLLBACK") ||
        access("/mnt/Flash/airport-srab-lab/armed", F_OK)) return 2;
#endif
    fd = open("/dev/bcmsrab0", O_RDONLY);
    if (fd < 0) return 1;
    if (ioctl(fd, IO_LOCK, NULL)) { close(fd); return 1; }
    stage("LOCK_ACQUIRED");
    if (check_reg(fd, SWMODE, 7, "SWITCH") ||
        check_reg(fd, CTRL0, 0xe3, "CTRL0") ||
        rd(fd, CTRL5, &miss) || (miss != 0x10 && miss != 0x18)) rc = -1;
    else if (!strcmp(argv[1], "apply")) rc = apply(fd);
    else if (!strcmp(argv[1], "restore")) rc = restore(fd);
    else if (!strcmp(argv[1], "baseline")) rc = verify(fd, 0);
    else rc = verify(fd, 1);
    if (!strcmp(argv[1], "verify") || !strcmp(argv[1], "baseline"))
        stage(rc ? "CONFIG_VERIFY_FAILED" : "CONFIG_VERIFY_SUCCESS");
    unlock_rc = ioctl(fd, IO_UNLOCK, NULL);
    stage(unlock_rc ? "UNLOCK_FAILED" : "UNLOCK_SUCCESS");
    if (close(fd)) rc = -1;
    return (rc || unlock_rc) ? 1 : 0;
}
