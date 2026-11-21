/* Redmi Note 7 (lavender, SDM660) bare-metal boot test loader.
 * Runs at EL1, MMU/caches off, entered from ABL with x0 = DTB.
 * Output goes to the bootloader's splash framebuffer (1080x2340 XRGB8888, stride 4320). */
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include "font.h"

#ifndef RISKY
#define RISKY 0
#endif
#ifndef BUILD_TAG
#define BUILD_TAG "local"
#endif

typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64; typedef int64_t s64;

#ifdef HOST_TEST
extern uint8_t host_fb[];
extern u64 fake_sysreg(const char *name);
extern u32 host_rd32(u64 addr);
extern void host_secondary(void);
#define FB_BASE ((uintptr_t)host_fb)
#define SYSREG(n) fake_sysreg(#n)
#define RD32(a) host_rd32(a)
#define WR32(a,v) host_wr32((a),(v))
extern void host_wr32(u64 addr, u32 v);
#define WFI() do {} while (0)
#define DSB() do {} while (0)
#else
#define FB_BASE 0x9D400000UL
#define SYSREG(n) ({ u64 _v; __asm__ volatile("mrs %0, " #n : "=r"(_v)); _v; })
#define RD32(a) (*(volatile u32 *)(uintptr_t)(a))
#define WR32(a,v) (*(volatile u32 *)(uintptr_t)(a) = (v))
#define WFI() __asm__ volatile("wfi")
#define DSB() __asm__ volatile("dsb sy" ::: "memory")
#endif

extern u64 smc_call(u64 fn, u64 a1, u64 a2, u64 a3);
extern void secondary_entry(void);

#define FB_STRIDE 4320
#define FB_BYTES  (FB_STRIDE * 2340)
#define MARGIN_X 12
#define MARGIN_Y 10
#define COLS 48
#define ROWS 53

/* ------------------------------------------------------------ libc bits */
void *memcpy(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }
int memcmp(const void *x, const void *y, size_t n) { const u8 *a = x, *b = y; while (n--) { if (*a != *b) return *a - *b; a++; b++; } return 0; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int startswith(const char *s, const char *p) { while (*p) { if (*s++ != *p++) return 0; } return 1; }
static int contains(const char *s, const char *p) { size_t n = strlen(p); for (; *s; s++) if (!memcmp(s, p, n)) return 1; return 0; }

/* ------------------------------------------------------------ console */
static u32 cur_col, cur_row, fg = 0x00FFFFFF, bg = 0x00000000;
#define C_WHITE 0x00FFFFFF
#define C_GREEN 0x0040FF40
#define C_RED   0x00FF4040
#define C_CYAN  0x0040E0FF
#define C_YEL   0x00FFE040

static void fb_clear(void) {
    volatile u64 *p = (volatile u64 *)FB_BASE;
    for (u32 i = 0; i < FB_BYTES / 8; i++) p[i] = 0;
}
static void put_glyph(u32 col, u32 row, char c) {
    if (c < 32 || c > 126) c = '?';
    const unsigned char *g = font_data[c - 32];
    volatile u32 *dst = (volatile u32 *)(FB_BASE + (u64)(MARGIN_Y + row * FONT_H) * FB_STRIDE + (MARGIN_X + col * FONT_W) * 4);
    for (u32 y = 0; y < FONT_H; y++) {
        for (u32 x = 0; x < FONT_W; x++)
            dst[x] = ((g[y * FONT_ROWBYTES + (x >> 3)] >> (7 - (x & 7))) & 1) ? fg : bg;
        dst += FB_STRIDE / 4;
    }
}
static void kputc(char c) {
    if (c == '\n') { cur_col = 0; cur_row++; return; }
    if (cur_col >= COLS) { cur_col = 0; cur_row++; }
    if (cur_row >= ROWS) return;
    put_glyph(cur_col++, cur_row, c);
}
static void kputs_n(const char *s, int max) { while (*s && max-- != 0) kputc(*s++); }
static void kcolor(u32 c) { fg = c; }

static void put_num(u64 v, u32 base, int upper, int width, char pad, int neg) {
    char buf[24]; int n = 0;
    if (!v) buf[n++] = '0';
    while (v) { u32 d = (u32)(v % base); buf[n++] = (char)(d < 10 ? '0' + d : (upper ? 'A' : 'a') + d - 10); v /= base; }
    if (neg) buf[n++] = '-';
    while (n < width) buf[n++] = pad;
    while (n) kputc(buf[--n]);
}
static void kprintf(const char *f, ...) {
    va_list ap; va_start(ap, f);
    for (; *f; f++) {
        if (*f != '%') { kputc(*f); continue; }
        f++;
        char pad = ' '; int width = 0, prec = -1, lng = 0;
        if (*f == '0') { pad = '0'; f++; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        if (*f == '.') { f++; prec = 0; while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0'); }
        while (*f == 'l' || *f == 'z') { lng++; f++; }
        switch (*f) {
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)"; kputs_n(s, prec); break; }
        case 'c': kputc((char)va_arg(ap, int)); break;
        case 'x': case 'X': { u64 v = lng ? va_arg(ap, u64) : va_arg(ap, u32); put_num(v, 16, *f == 'X', width, pad, 0); break; }
        case 'u': { u64 v = lng ? va_arg(ap, u64) : va_arg(ap, u32); put_num(v, 10, 0, width, pad, 0); break; }
        case 'd': { s64 v = lng ? va_arg(ap, s64) : (s64)(int)va_arg(ap, int); int ng = v < 0; put_num(ng ? (u64)-v : (u64)v, 10, 0, width, pad, ng); break; }
        case '%': kputc('%'); break;
        default: kputc('?'); break;
        }
    }
    va_end(ap);
}
static void ok(const char *s)   { kcolor(C_GREEN); kprintf("%s", s); kcolor(C_WHITE); }
static void bad(const char *s)  { kcolor(C_RED);   kprintf("%s", s); kcolor(C_WHITE); }

/* ------------------------------------------------------------ FDT */
static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u64 rcells(const u8 *p, u32 n) { u64 v = 0; while (n--) { v = (v << 32) | be32(p); p += 4; } return v; }

static u64 gic_base;           /* first reg of first interrupt-controller@ node */
static u64 gicr_base;          /* second reg entry of it */
static u64 cpu_reg[8]; static u32 ncpu;
static u64 sdhci_base;
static u64 sdhci_reg[4], sdhci_regsz[4]; static u32 sdhci_nreg; static const char *sdhci_regnames; static u32 sdhci_regnames_len;

struct nstk { char name[48]; u32 ac, sc; };

static void build_path(struct nstk *st, int depth, char *out, int max) {
    int n = 0; out[0] = 0;
    for (int i = 1; i <= depth; i++) {
        if (n + 1 < max) out[n++] = '/';
        for (const char *s = st[i].name; *s && n + 1 < max; s++) out[n++] = *s;
    }
    if (!n) { out[0] = '/'; n = 1; }
    out[n] = 0;
}

static void fdt_walk(const u8 *dtb, int verbose) {
    if (be32(dtb) != 0xd00dfeed) { bad("DTB BAD MAGIC\n"); return; }
    u32 total = be32(dtb + 4), off_st = be32(dtb + 8), off_str = be32(dtb + 12), sz_st = be32(dtb + 36);
    const u8 *p = dtb + off_st, *end = p + sz_st; const char *strs = (const char *)(dtb + off_str);
    static struct nstk st[16]; int depth = -1, sel_depth = -1, sdhci_cur = 0, sdhci_done = 0; u32 nodes = 0, nsel = 0, nrsv = 0;
    if (verbose) kprintf("DTB @%llx size %x ver %u\n", (u64)(uintptr_t)dtb, total, be32(dtb + 20));
    while (p < end) {
        u32 tag = be32(p); p += 4;
        if (tag == 1) {
            const char *nm = (const char *)p; u32 l = (u32)strlen(nm) + 1; p += (l + 3) & ~3u;
            depth++; if (depth >= 16) { bad("DTB TOO DEEP\n"); return; }
            u32 i = 0; while (nm[i] && i < 47) { st[depth].name[i] = nm[i]; i++; } st[depth].name[i] = 0;
            st[depth].ac = 2; st[depth].sc = 1; nodes++;
            sel_depth = -1; sdhci_cur = 0; if (depth == 2 && !sdhci_done && startswith(nm, "sdhci@")) { sdhci_cur = 1; sdhci_done = 1; }
            if (depth == 2 && (startswith(nm, "interrupt-controller@") || startswith(nm, "sdhci@") ||
                               streq(nm, "timer") || contains(nm, "mdss_mdp@") || startswith(nm, "serial@")) && nsel < 12) {
                sel_depth = depth; nsel++;
            }
        } else if (tag == 2) { depth--; sel_depth = -1; sdhci_cur = 0; }
        else if (tag == 3) {
            u32 len = be32(p), noff = be32(p + 4); p += 8; const u8 *val = p; p += (len + 3) & ~3u;
            const char *pn = strs + noff; char path[96]; build_path(st, depth, path, sizeof path);
            u32 pac = depth > 0 ? st[depth - 1].ac : 2, psc = depth > 0 ? st[depth - 1].sc : 1;
            if (streq(pn, "#address-cells")) st[depth].ac = be32(val);
            else if (streq(pn, "#size-cells")) st[depth].sc = be32(val);
            if (depth == 0 && verbose) {
                if (streq(pn, "model")) kprintf("MODEL %.41s\n", (const char *)val);
                else if (streq(pn, "compatible")) kprintf("COMPAT %.40s\n", (const char *)val);
            }
            if (startswith(path, "/memory") && streq(pn, "reg") && verbose) {
                u32 ent = (pac + psc) * 4;
                for (u32 o = 0; o + ent <= len; o += ent) { u64 a = rcells(val + o, pac), s = rcells(val + o + pac * 4, psc); kprintf("MEM %llx-%llx\n", a, a + s - 1); }
            }
            if (streq(path, "/chosen") && verbose) {
                if (streq(pn, "bootargs")) kprintf("BOOTARGS(%u) %.80s\n", len, (const char *)val);
                else if (startswith(pn, "linux,initrd")) kprintf("%s %llx\n", pn, rcells(val, len / 4));
            }
            if (startswith(path, "/reserved-memory/") && streq(pn, "reg") && len >= (pac + psc) * 4) {
                if (verbose && nrsv < 14) { kprintf("RSV %.30s +%llx\n", st[depth].name, rcells(val + pac * 4, psc)); }
                nrsv++;
            }
            if (startswith(path, "/cpus/cpu@") && streq(pn, "reg") && ncpu < 8) cpu_reg[ncpu++] = rcells(val, pac);
            if (streq(path, "/psci") && verbose) { if (streq(pn, "method")) kprintf("PSCI method %s\n", (const char *)val); }
            if (sel_depth == depth && streq(pn, "compatible") && verbose) kprintf("%.30s %.17s\n", st[depth].name, (const char *)val);
            if (startswith(st[depth].name, "interrupt-controller@") && depth == 2 && streq(pn, "reg") && !gic_base) {
                gic_base = rcells(val, pac);
                if (len >= (pac + psc) * 8) gicr_base = rcells(val + (pac + psc) * 4, pac);
            }
            if (sdhci_cur) {
                if (streq(pn, "reg")) {
                    sdhci_base = rcells(val, pac);
                    u32 ent = (pac + psc) * 4;
                    for (u32 o = 0; o + ent <= len && sdhci_nreg < 4; o += ent) { sdhci_reg[sdhci_nreg] = rcells(val + o, pac); sdhci_regsz[sdhci_nreg++] = rcells(val + o + pac * 4, psc); }
                } else if (streq(pn, "reg-names")) { sdhci_regnames = (const char *)val; sdhci_regnames_len = len; }
            }
        } else if (tag == 4) { }
        else break;
    }
    if (verbose) {
        kprintf("RSV regions: %u (first 14 shown)\nNODES %u\nCPU reg:", nrsv, nodes);
        for (u32 i = 0; i < ncpu; i++) kprintf(" %llx", cpu_reg[i]);
        kprintf("\n");
    }
}

/* ------------------------------------------------------------ CPU info */
static void cpu_info(void) {
    u64 el = SYSREG(currentel) >> 2, midr = SYSREG(midr_el1), mpidr = SYSREG(mpidr_el1), sctlr = SYSREG(sctlr_el1);
    u64 pfr0 = SYSREG(id_aa64pfr0_el1), isar0 = SYSREG(id_aa64isar0_el1), isar1 = SYSREG(id_aa64isar1_el1), mmfr0 = SYSREG(id_aa64mmfr0_el1);
    u64 frq = SYSREG(cntfrq_el0);
    kprintf("EL%llu  MIDR %08llx  MPIDR %08llx\n", el, midr, mpidr);
    kprintf("core: %s\n", ((midr >> 4) & 0xFFF) == 0x801 ? "Kryo260 Silver (A53 class)" : ((midr >> 4) & 0xFFF) == 0x800 ? "Kryo260 Gold (A73 class)" : "unknown");
    kprintf("SCTLR %08llx  MMU %s  D$ %s  I$ %s\n", sctlr, (sctlr & 1) ? "ON" : "off", (sctlr & 4) ? "ON" : "off", (sctlr & 0x1000) ? "ON" : "off");
    kprintf("timer %llu Hz  PA bits code %llu\n", frq, mmfr0 & 15);
    kprintf("AES %llu SHA1 %llu SHA2 %llu CRC32 %llu\n", (isar0 >> 4) & 15, (isar0 >> 8) & 15, (isar0 >> 12) & 15, (isar0 >> 16) & 15);
    kprintf("LSE atomics: %s   RDM: %s\n", ((isar0 >> 20) & 15) ? "yes" : "NO", ((isar0 >> 28) & 15) ? "yes" : "NO");
    kprintf("GICv3 sysreg: %s  FP/SIMD: %s\n", ((pfr0 >> 24) & 15) ? "yes" : "no", (((pfr0 >> 16) & 15) == 15) ? "NO" : "yes");
    kprintf("ISAR1 %llx (v8.2+ features: %s)\n", isar1, isar1 ? "some" : "none");
}

/* ------------------------------------------------------------ risky hardware tests */
static volatile u32 cpu1_flag; static volatile u64 cpu1_mpidr;
static u8 cpu1_stack[4096] __attribute__((aligned(16)));
void secondary_main(void) { cpu1_mpidr = SYSREG(mpidr_el1); DSB(); cpu1_flag = 1; DSB(); }

static u64 cnt(void) { u64 a = SYSREG(cntvct_el0), b = SYSREG(cntvct_el0); return ((a ^ b) >> 32) ? a : b; }

static void risky_tests(void) {
    u64 f = SYSREG(cntfrq_el0), t0 = cnt(), t1 = t0, spins = 0;
    kprintf("\n[1] timer: ");
    while ((t1 - t0) < f && spins < 400000000ULL) { t1 = cnt(); spins++; }
    if ((t1 - t0) >= f) ok("OK"); else bad("STUCK");
    kprintf(" (1 s, %llu loops)\n", spins);

    kprintf("[2] PSCI_VERSION: ");
    u64 v = smc_call(0x84000000, 0, 0, 0);
    if ((s64)v < 0) bad("FAIL"); else ok("OK");
    kprintf(" %llx\n", v);

    kprintf("[3] GICv3 @%llx: ", gic_base);
    if (gic_base) {
        u32 ctlr = RD32(gic_base), typer = RD32(gic_base + 4), pidr2 = RD32(gic_base + 0xFFE8);
        kprintf("CTLR %x TYPER %x PIDR2 %x ", ctlr, typer, pidr2);
        if (((pidr2 >> 4) & 15) == 3) ok("v3\n"); else bad("not v3?\n");
        if (gicr_base) kprintf("    GICR @%llx TYPER %x%08x\n", gicr_base, RD32(gicr_base + 12), RD32(gicr_base + 8));
    } else bad("no gic in DT\n");

    kprintf("[4] CPU_ON core1 (reg %llx): ", ncpu > 1 ? cpu_reg[1] : 1);
    u64 target = ncpu > 1 ? cpu_reg[1] : 1;
    u64 a0 = smc_call(0xC4000004, target, 0, 0);
    kprintf("AFF_INFO %d ", (int)a0);
    s64 r = (s64)smc_call(0xC4000003, target, (u64)(uintptr_t)secondary_entry, (u64)(uintptr_t)(cpu1_stack + sizeof cpu1_stack));
    kprintf("CPU_ON ret %d\n", (int)r);
#ifdef HOST_TEST
    host_secondary();
#endif
    for (u64 i = 0; i < 200000000ULL && !cpu1_flag; i++) { }
    kprintf("    "); if (cpu1_flag) { ok("CPU1 ALIVE"); kprintf(" mpidr %llx\n", cpu1_mpidr); } else bad("CPU1 TIMEOUT\n");
    kprintf("    AFF_INFO now %d\n", (int)smc_call(0xC4000004, target, 0, 0));
    if (sdhci_base) kprintf("eMMC controller @%llx (not touched yet)\n", sdhci_base);
}


/* ------------------------------------------------------------ eMMC (SDHCI, PIO) test */
static u32 sect[128];
static int sdhci_read_sector(u64 b, u64 lba, u32 *buf, u32 *err) {
    u32 n;
    for (n = 0; n < 2000000; n++) if (!(RD32(b + 0x24) & 3)) break;
    if (n == 2000000) { *err = 0xE0000001; return -1; }
    WR32(b + 0x30, 0xFFFFFFFF);
    WR32(b + 0x04, (1u << 16) | 512);
    WR32(b + 0x08, (u32)lba);
    WR32(b + 0x0C, (0x113Au << 16) | 0x0010u);          /* CMD17, R1, data present; read direction, PIO */
    u32 s = 0;
    for (n = 0; n < 4000000; n++) { s = RD32(b + 0x30); if (s & 0x8000) { *err = s; return -2; } if (s & 0x20) break; }
    if (n == 4000000) { *err = s | 0xE0000002; return -3; }
    for (u32 i = 0; i < 128; i++) buf[i] = RD32(b + 0x20);
    for (n = 0; n < 4000000; n++) { s = RD32(b + 0x30); if (s & 2) break; }
    WR32(b + 0x30, 0xFFFFFFFF);
    if (n == 4000000) { *err = 0xE0000003; return -4; }
    return 0;
}
static u64 le64(const u8 *p) { u64 v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static void emmc_test(void) {
    u64 b = sdhci_base ? sdhci_base : 0xC0C4000;
    kprintf("sdhci reg:"); for (u32 i = 0; i < sdhci_nreg; i++) kprintf(" %llx+%llx", sdhci_reg[i], sdhci_regsz[i]); kprintf("\n");
    if (sdhci_regnames) { kprintf("names:"); const char *p = sdhci_regnames; u32 k = 0; while (k < sdhci_regnames_len) { kprintf(" %.12s", p); u32 l = (u32)strlen(p) + 1; p += l; k += l; } kprintf("\n"); }
    kprintf("touching eMMC @%llx ...\n", b);
    kprintf("24 PRESENT %08x\n", RD32(b + 0x24));
    kprintf("28 HC/PWR  %08x\n", RD32(b + 0x28));
    kprintf("2C CLOCK   %08x\n", RD32(b + 0x2C));
    kprintf("30 INTST   %08x\n", RD32(b + 0x30));
    kprintf("3C HC2     %08x\n", RD32(b + 0x3C));
    kprintf("40 CAPS    %08x %08x\n", RD32(b + 0x40), RD32(b + 0x44));
    kprintf("FC VERSION %08x\n", RD32(b + 0xFC));
    u32 err = 0;
    kprintf("read LBA0: ");
    int r = sdhci_read_sector(b, 0, sect, &err);
    if (r) { bad("FAIL"); kprintf(" r=%d err=%08x\n", r, err); return; }
    ok("OK"); kprintf(" sig(510)=%04x\n", ((const u8 *)sect)[510] | (((const u8 *)sect)[511] << 8));
    kprintf("read LBA1: ");
    r = sdhci_read_sector(b, 1, sect, &err);
    if (r) { bad("FAIL"); kprintf(" r=%d err=%08x\n", r, err); return; }
    const u8 *h = (const u8 *)sect;
    if (memcmp(h, "EFI PART", 8)) { bad("no GPT"); kprintf(" %02x %02x %02x %02x\n", h[0], h[1], h[2], h[3]); return; }
    ok("GPT OK\n");
    u64 eslba = le64(h + 72); u32 ecount = le32(h + 80), esize = le32(h + 84);
    kprintf("GPT entries %u x %u B @LBA %llu\n", ecount, esize, eslba);
    static const char *want[] = { "boot", "recovery", "userdata", "system", "vendor", "persist", "modem", "cache", "dtbo", "vbmeta", 0 };
    u32 found = 0, total = 0;
    for (u32 i = 0; i < ecount && esize >= 128; i += 512 / esize) {
        if (sdhci_read_sector(b, eslba + (u64)i * esize / 512, sect, &err)) { bad("entry read FAIL\n"); kprintf(" err=%08x\n", err); break; }
        for (u32 k = 0; k < 512 / esize; k++) {
            const u8 *e = (const u8 *)sect + k * esize;
            u32 any = 0; for (int q = 0; q < 16; q++) any |= e[q];
            if (!any) continue;
            total++;
            char name[37]; for (int q = 0; q < 36; q++) name[q] = (char)e[56 + q * 2]; name[36] = 0;
            for (int w = 0; want[w]; w++) if (streq(name, want[w])) {
                u64 f = le64(e + 32), l = le64(e + 40);
                kprintf("%s %llx-%llx %lluMB\n", name, f, l, (l - f + 1) / 2048); found++;
            }
        }
    }
    kprintf("partitions: %u (listed %u)\n", total, found);
}

void kmain(const void *dtb) {
    fb_clear();
    kcolor(C_CYAN); kprintf("KB LOADER (C) %s %s\n", BUILD_TAG, RISKY == 2 ? "EMMC" : RISKY ? "FULL" : "SAFE"); kcolor(C_WHITE);
#if RISKY != 2
    cpu_info();
#endif
    kprintf("x0(DTB) = %llx\n", (u64)(uintptr_t)dtb);
    fdt_walk((const u8 *)dtb, RISKY == 0);
#if RISKY == 1
    risky_tests();
#elif RISKY == 2
    emmc_test();
#endif
    kcolor(C_YEL); kprintf("\nDONE. Photo the screen.\n"); kcolor(C_WHITE);
    for (;;) { WFI();
#ifdef HOST_TEST
        return;
#endif
    }
}
