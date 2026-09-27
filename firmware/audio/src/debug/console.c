/* fb200-audio console. Commands (see `help`):
 *   help | stats | hb on|off | scan | dump [bus addr] | peek <addr> [len]
 *   poke <addr> <val> | reset | reboot
 * Addresses accept 0x.. hex or decimal. peek/poke are limited to RAM/flash
 * (peripheral access can stall a clock-gated block). */
#include <stdint.h>
#include <string.h>
#include "tusb.h"
#include "cdc_log.h"
#include "console.h"
#include "fsl_iomuxc.h"
#include "audio/i2c_probe.h"

extern int g_bss_writable;

static char line[96];
static size_t line_len;
static int heartbeat_on;

static uint32_t parse_num(const char *s, int *ok)
{
    uint32_t v = 0;
    *ok = 1;
    if (!s || !*s) { *ok = 0; return 0; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        if (!*s) { *ok = 0; return 0; }
        for (; *s; s++) {
            char c = *s;
            uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0')
                       : (c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10)
                       : (c >= 'A' && c <= 'F') ? (uint32_t)(c - 'A' + 10) : 99u;
            if (d > 15u) { *ok = 0; return 0; }
            v = (v << 4) | d;
        }
        return v;
    }
    for (; *s; s++) {
        if (*s < '0' || *s > '9') { *ok = 0; return 0; }
        v = v * 10u + (uint32_t)(*s - '0');
    }
    return v;
}

static int mem_ok(uint32_t addr)
{
    return (addr < 0x400000u) ||                       /* ITCM/DTCM */
           (addr >= 0x20000000u && addr < 0x20300000u) ||   /* DTCM/OCRAM */
           (addr >= 0x400F8000u && addr < 0x400F9000u) ||   /* SRC (always on) */
           (addr >= 0x401B8000u && addr < 0x401C8000u) ||   /* GPIO1-4 */
           (addr >= 0x60000000u && addr < 0x60800000u);     /* flash */
}

static void hexdump(uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        if ((i & 15u) == 0) log_printf("%08x:", (unsigned)(addr + i));
        log_printf(" %02x", *(volatile uint8_t *)(addr + i));
        if ((i & 15u) == 15u) log_printf("\r\n");
    }
    if (len & 15u) log_printf("\r\n");
}

static void cmd_dumpmem(const char *a1, const char *a2)
{
    int ok1, ok2;
    uint32_t addr = parse_num(a1, &ok1);
    uint32_t len = parse_num(a2, &ok2);
    if (!ok1 || !ok2) { log_printf("usage: dumpmem <addr> <len>\r\n"); return; }
    if (len > 4096u) len = 4096u;
    if (!mem_ok(addr) || !mem_ok(addr + len)) { log_printf("address not allowed\r\n"); return; }
    hexdump(addr, len);
}

static void cmd_src(void)
{
    log_printf("SRC_SRSR=%08x SRC_SBMR1=%08x SRC_SBMR2=%08x\r\n",
               (unsigned)*(volatile uint32_t *)0x400F8008u,
               (unsigned)*(volatile uint32_t *)0x400F8004u,
               (unsigned)*(volatile uint32_t *)0x400F801Cu);
}

/* Direct call into the bootloader's DFU entry (0x6000333C, mapped in flash).
 * Hypothesis: the stock app's 0xC1 calls it directly instead of resetting. */
__attribute__((noreturn)) static void jump_dfu(void)
{
    tud_disconnect();
    for (volatile uint32_t i = 0; i < 2000000u; i++) {
    }
    ((void (*)(void))(0x6000333Cu | 1u))();
    __builtin_unreachable();
}

/* Program one byte into the FlexSPI NOR flash through the IP command
 * interface, reusing the LUT the bootloader loaded (sequence 1 = page
 * program, per the FCB). Used to write the update flag. */
static int flash_program_byte(uint32_t flash_offset, uint8_t value)
{
    volatile uint32_t *fx = (volatile uint32_t *)0x402A8000u;
    fx[0x14 / 4] = (1u << 0) | (1u << 3);          /* W1C: IPCMDDONE/IPCMDERR */
    fx[0xA0 / 4] = flash_offset;                   /* IPCR0: flash offset */
    fx[0xBC / 4] = (1u << 0);                      /* IPTXFCR: clear TX FIFO */
    fx[0x180 / 4] = (uint32_t)value;               /* TFDR0: byte to program */
    fx[0xA4 / 4] = (1u << 8) | 1u;                 /* IPCR1: seq 1, 1 byte */
    fx[0xB0 / 4] = 1u;                             /* IPCMD: trigger */
    uint32_t timeout = 2000000u;
    while (!(fx[0x14 / 4] & (1u << 0)) && --timeout) {
    }
    int ok = (fx[0x14 / 4] & (1u << 0)) != 0u && (fx[0x14 / 4] & (1u << 3)) == 0u;
    fx[0x14 / 4] = (1u << 0) | (1u << 3);
    return ok;
}

/* Handover to the vendor bootloader: the stock app's 0xC1 path.
 * RE (docs/BOOTLOADER.md): the bootloader's update-mode entry is 0x600091BC
 * (it performs its own pin/IO and USB init); the bootloader reaches it when
 * the app-validity checks fail or the A+D buttons are held. Call it directly
 * like the stock 0xC1 handler does. */
__attribute__((noreturn)) static void handover(void)
{
    log_printf("handover: calling update-mode entry 0x600091bc\r\n");
    cdc_log_task();
    tud_disconnect();
    for (volatile uint32_t i = 0; i < 2000000u; i++) {
    }
    ((void (*)(void))(0x600091BCu | 1u))();
    __builtin_unreachable();
}

static void cmd_help(void)
{
    log_printf("commands: help | stats | src | hb on|off | scan | dump [bus addr] |\r\n"
               "          peek <addr> [len] | dumpmem <addr> <len> | poke <addr> <val> |\r\n"
               "          handover (DFU) | dfu (direct) | reset | reboot\r\n");
}

static void cmd_stats(void)
{
    log_printf("bss_writable=%d heartbeat=%d line_len=%u\r\n",
               g_bss_writable, heartbeat_on, (unsigned)line_len);
}

static void cmd_peek(const char *a1, const char *a2)
{
    int ok;
    uint32_t addr = parse_num(a1, &ok);
    if (!ok) { log_printf("usage: peek <addr> [len]\r\n"); return; }
    uint32_t len = 16;
    if (a2) { len = parse_num(a2, &ok); if (!ok) { log_printf("bad len\r\n"); return; } }
    if (len > 256) len = 256;
    if (!mem_ok(addr) || !mem_ok(addr + len)) { log_printf("address not allowed\r\n"); return; }
    hexdump(addr, len);
}

static void cmd_poke(const char *a1, const char *a2)
{
    int ok1, ok2;
    uint32_t addr = parse_num(a1, &ok1);
    uint32_t val = parse_num(a2, &ok2);
    if (!ok1 || !ok2) { log_printf("usage: poke <addr> <val>\r\n"); return; }
    if (!mem_ok(addr)) { log_printf("address not allowed\r\n"); return; }
    *(volatile uint8_t *)addr = (uint8_t)val;
    log_printf("ok %08x = %02x\r\n", (unsigned)addr, (unsigned)(val & 0xFFu));
}

static void cmd_dump(const char *a1, const char *a2)
{
    if (a1 && a2) {
        int ok1, ok2;
        uint32_t bus = parse_num(a1, &ok1);
        uint32_t addr = parse_num(a2, &ok2);
        if (!ok1 || !ok2) { log_printf("usage: dump [bus addr]\r\n"); return; }
        i2c_dump((uint8_t)bus, (uint8_t)addr);
    } else {
        i2c_dump_found();
    }
}

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int tokenize(char *s, char **argv, int max)
{
    int argc = 0;
    while (*s && argc < max) {
        while (*s == ' ') s++;
        if (!*s) break;
        argv[argc++] = s;
        while (*s && *s != ' ') s++;
        if (*s) *s++ = 0;
    }
    return argc;
}

static void dispatch(char *cmd)
{
    char *argv[3] = {0, 0, 0};
    int argc = tokenize(cmd, argv, 3);
    if (argc == 0) { cmd_help(); return; }

    if (streq(argv[0], "help")) cmd_help();
    else if (streq(argv[0], "stats")) cmd_stats();
    else if (streq(argv[0], "hb")) {
        heartbeat_on = (argc > 1 && streq(argv[1], "on"));
        log_printf("heartbeat %s\r\n", heartbeat_on ? "on" : "off");
    }
    else if (streq(argv[0], "scan")) i2c_scan_all();
    else if (streq(argv[0], "dump")) cmd_dump(argv[1], argv[2]);
    else if (streq(argv[0], "peek")) cmd_peek(argv[1], argv[2]);
    else if (streq(argv[0], "dumpmem")) cmd_dumpmem(argv[1], argv[2]);
    else if (streq(argv[0], "src")) cmd_src();
    else if (streq(argv[0], "poke")) cmd_poke(argv[1], argv[2]);
    else if (streq(argv[0], "flagtest")) {
        int ok = flash_program_byte(0x00086000u, 0x00u);
        log_printf("flag write %s; flag reads %02x\r\n", ok ? "ok" : "FAILED",
                   (unsigned)*(volatile uint8_t *)0x60086000u);
    }
    else if (streq(argv[0], "dfu")) {
        log_printf("jumping to the bootloader DFU entry\r\n");
        cdc_log_task();
        jump_dfu();
    }
    else if (streq(argv[0], "handover")) {
        log_printf("handover (DFU via the bootloader's 0xC1 path)\r\n");
        cdc_log_task();
        handover();
    }
    else if (streq(argv[0], "reset") || streq(argv[0], "reboot")) {
        log_printf("rebooting (handover)\r\n");
        cdc_log_task();
        console_reboot();
    }
    else log_printf("unknown command (try help)\r\n");
}

void console_init(void) { line_len = 0; heartbeat_on = 0; }

void console_task(void)
{
    while (tud_cdc_available()) {
        char c = (char)tud_cdc_read_char();
        if (c == '\r' || c == '\n') {
            cdc_log_write("\r\n", 2);
            line[line_len] = 0;
            dispatch(line);
            line_len = 0;
        } else if (c == 8 || c == 127) {
            if (line_len) { line_len--; cdc_log_write("\b \b", 3); }
        } else if (line_len < sizeof line - 1) {
            line[line_len++] = c;
            cdc_log_write(&c, 1);
        }
    }
}

int console_heartbeat_on(void) { return heartbeat_on; }
