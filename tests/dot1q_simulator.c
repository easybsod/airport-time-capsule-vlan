/* Execute the unmodified hardware engine against a register/VTA simulator.
 * This verifies program behavior, not the physical ASIC or ARM ABI. */
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
#include <stdarg.h>
#include <assert.h>

static int fake_open(const char *, int, ...);
static int fake_ioctl(int, unsigned long, ...);
static int fake_close(int);
static int fake_access(const char *, int);
static char *fake_getenv(const char *);

#define AIRPORT_EXPLICIT_UNSAFE_RESEARCH
#define AIRPORT_THREE_VLAN_SERVICE
#define main engine_main
#define open fake_open
#define ioctl fake_ioctl
#define close fake_close
#define access fake_access
#define getenv fake_getenv
#include "../src/airport-dot1q-test.c"
#undef main
#undef open
#undef ioctl
#undef close
#undef access
#undef getenv

struct reg_value { uint32_t selector, value; };
static struct reg_value registers[] = {
    { SWMODE, 7 }, { CTRL0, 0xe3 }, { CTRL4, 0xc4 }, { CTRL5, 0x10 },
    { ACCESS, 0 }, { INDEX, 18 }, { ENTRY, 0 },
    { PVID2, 18 }, { PVLAN2, 0x10f }, { PVID3, 18 }, { PVLAN3, 0x10f },
    { PVID8, 18 }, { PVLAN8, 0x10f },
};
static uint32_t table[4096];
static unsigned int write_count, table_write_count, lock_count, unlock_count, open_count;
static int guarded = 1, marker = 1, fail_pvid_once;

static uint32_t *reg_at(uint32_t selector)
{
    size_t i;
    for (i = 0; i < sizeof(registers) / sizeof(registers[0]); i++)
        if (registers[i].selector == selector) return &registers[i].value;
    assert(!"unexpected register selector");
    return NULL;
}

static uint32_t encode(unsigned int members, unsigned int untag)
{
    return (1U << 21) | (untag << 9) | members;
}

static void reset(void)
{
    *reg_at(CTRL4) = 0xc4; *reg_at(CTRL5) = 0x10;
    *reg_at(PVID2) = 18; *reg_at(PVLAN2) = 0x10f;
    *reg_at(INDEX) = 18; *reg_at(ACCESS) = 0;
    memset(table, 0, sizeof(table));
    table[18] = encode(0x10f, 0x10f);
    write_count = table_write_count = lock_count = unlock_count = open_count = 0;
    guarded = marker = 1;
    fail_pvid_once = 0;
}

static int fake_open(const char *path, int flags, ...)
{
    assert(!strcmp(path, "/dev/bcmsrab0"));
    assert(flags == O_RDONLY);
    open_count++;
    return 123;
}
static int fake_close(int fd) { assert(fd == 123); return 0; }
static int fake_access(const char *path, int mode)
{
    assert(!strcmp(path, "/mnt/Memory/airport-three-vlan/armed"));
    assert(mode == F_OK);
    return marker ? 0 : -1;
}
static char *fake_getenv(const char *name)
{
    assert(!strcmp(name, "AIRPORT_THREE_VLAN_GUARDED"));
    return guarded ? "BOUNDED_SERVICE_TRANSACTION" : NULL;
}
static int fake_ioctl(int fd, unsigned long operation, ...)
{
    va_list ap;
    struct request *req;
    assert(fd == 123);
    va_start(ap, operation);
    req = va_arg(ap, struct request *);
    va_end(ap);
    if (operation == IO_LOCK) { assert(!req); lock_count++; return 0; }
    if (operation == IO_UNLOCK) { assert(!req); unlock_count++; return 0; }
    assert(req && req->reserved == 0);
    if (operation == IO_READ) { req->value = *reg_at(req->selector); return 0; }
    assert(operation == IO_WRITE && req->value <= UINT32_MAX);
    write_count++;
    /* No global forwarding change, unrelated port change, or table clear. */
    assert(req->selector == INDEX || req->selector == ACCESS || req->selector == ENTRY ||
           req->selector == PVID2 || req->selector == PVLAN2 ||
           req->selector == CTRL4 || req->selector == CTRL5);
    if (fail_pvid_once && req->selector == PVID2 && req->value == 1002) {
        fail_pvid_once = 0; errno = EIO; return -1;
    }
    if (req->selector == INDEX) {
        assert(req->value == 18 || req->value == 1002 || req->value == 1003);
    }
    if (req->selector == ACCESS) {
        unsigned int vid = *reg_at(INDEX);
        assert(req->value == 0x80 || req->value == 0x81);
        if (req->value == 0x81) *reg_at(ENTRY) = table[vid];
        else { table[vid] = *reg_at(ENTRY); table_write_count++; }
        *reg_at(ACCESS) = req->value & 1U;
        return 0;
    }
    *reg_at(req->selector) = (uint32_t)req->value;
    return 0;
}

static int cli(const char *action)
{
    char *argv[] = { "switch-config", (char *)action, NULL };
    return engine_main(2, argv);
}

int main(void)
{
    reset();
    assert(NATIVE_ENTRY == encode(0x10f, 0x10f));
    assert(NATIVE_TARGET_ENTRY == encode(0x10b, 0x10b));
    assert(CANDIDATE_ENTRY == encode((1U << 2) | (1U << 3), 1U << 2));
    assert(GUEST_ENTRY == encode((1U << 3) | (1U << 8), 0));
    assert(sizeof(struct request) == 16 && offsetof(struct request, value) == 8);
    assert(PVID2 == ((0x34U << 24) | ((0x10U + 2U * 2U) << 16) | 1U));
    assert(PVLAN2 == ((0x31U << 24) | ((2U * 2U) << 16) | 1U));

    /* ABI and guard failures must never open or access the device. */
    assert(cli("abi") == 0 && open_count == 0);
    guarded = 0;
    assert(cli("apply") == 2 && open_count == 0);
    guarded = 1; marker = 0;
    assert(cli("verify") == 2 && open_count == 0);
    marker = 1;

    assert(cli("baseline") == 0 && table_write_count == 0);
    assert(cli("apply") == 0);
    assert(table[18] == encode(0x10b, 0x10b));
    assert(table[1002] == encode(0xc, 4));
    assert(table[1003] == encode(0x108, 0));
    assert(*reg_at(PVID2) == 1002 && *reg_at(PVLAN2) == 0xc);
    assert(*reg_at(CTRL4) == 0x40 && *reg_at(CTRL5) == 0x18);
    assert(table_write_count == 3);
    assert(cli("verify") == 0 && table_write_count == 3);
    assert(cli("restore") == 0 && table_write_count == 6);
    assert(cli("baseline") == 0);
    assert(lock_count == unlock_count);

    /* A baseline mismatch may query entries but must never update the table. */
    reset(); *reg_at(PVID2) = 19;
    assert(cli("apply") == 1 && table_write_count == 0);
    assert(lock_count == unlock_count);

    /* Failure halfway through apply must restore all changed fields. */
    reset(); fail_pvid_once = 1;
    assert(cli("apply") == 1);
    assert(cli("baseline") == 0);
    assert(table[1002] == 0 && table[1003] == 0);
    assert(lock_count == unlock_count);

    /* Unknown state must not be overwritten by restore. */
    reset(); table[1002] = 0xdead;
    assert(cli("restore") == 1 && table_write_count == 0);
    assert(table[1002] == 0xdead && lock_count == unlock_count);

    /* Internal entry and port writers reject values outside their allowlists. */
    reset();
    assert(write_table_entry(123, 4094, 0) == -1);
    assert(write_table_entry(123, 1002, 0xffffffffU) == -1);
    assert(write_port2(123, PVID3, 1002) == -1);
    assert(write_count == 0);
    puts("dot1q simulator: all checks passed");
    return 0;
}
