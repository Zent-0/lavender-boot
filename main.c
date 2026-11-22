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
#define WR8(a,v) do { } while (0)
#define SET_ELR(v) do { } while (0)
#define TEST_BRK() do { exc_seen = 1; } while (0)
#define WFI() do {} while (0)
#define DSB() do {} while (0)
#else
#define FB_BASE 0x9D400000UL
#define SYSREG(n) ({ u64 _v; __asm__ volatile("mrs %0, " #n : "=r"(_v)); _v; })
#define RD32(a) (*(volatile u32 *)(uintptr_t)(a))
#define WR32(a,v) (*(volatile u32 *)(uintptr_t)(a) = (v))
#define WR8(a,v) (*(volatile u8 *)(uintptr_t)(a) = (u8)(v))
#define SET_ELR(v) __asm__ volatile("msr elr_el1, %0" :: "r"((u64)(v)))
#define TEST_BRK() __asm__ volatile("brk #0")
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
static u32 cur_col, cur_row, kscale = 1, fg = 0x00FFFFFF, bg = 0x00000000;
static char logbuf[16384]; static u32 loglen;
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
        for (u32 sy = 0; sy < kscale; sy++) {
            for (u32 x = 0; x < FONT_W; x++) {
                u32 px = ((g[y * FONT_ROWBYTES + (x >> 3)] >> (7 - (x & 7))) & 1) ? fg : bg;
                for (u32 sx = 0; sx < kscale; sx++) dst[x * kscale + sx] = px;
            }
            dst += FB_STRIDE / 4;
        }
    }
}
static void kputc(char c) {
    if (loglen < sizeof logbuf - 1) logbuf[loglen++] = c;
    if (c == '\n') { cur_col = 0; cur_row += kscale; return; }
    if (cur_col + kscale > COLS) { cur_col = 0; cur_row += kscale; }
    if (cur_row + kscale > ROWS) return;
    put_glyph(cur_col, cur_row, c);
    cur_col += kscale;
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


/* ------------------------------------------------------------ eMMC (SDHCI, ADMA2) driver */
#ifdef HOST_TEST
static u32 dma_area[2048] __attribute__((aligned(4096)));
#define DMA_PTR ((u8 *)dma_area)
#else
#define DMA_PTR ((u8 *)0x82100000UL)         /* RAM block mapped non-cacheable when the MMU is on */
#endif
#define sect      ((u32 *)(DMA_PTR))
#define adma_desc ((u32 *)(DMA_PTR + 0x400))
#define wbuf      ((u8 *)(DMA_PTR + 0x800))
static u32 dbg_int;
static u64 emmc_base;

static int sdhci_reset_lines(u64 b) {
    WR8(b + 0x2F, 0x06);                                /* software reset CMD + DAT lines */
    for (u32 n = 0; n < 1000000; n++) if (!(((RD32(b + 0x2C) >> 24) & 6))) return 0;
    return -1;
}
/* one 512-byte block via ADMA2. returns 0 ok, <0 failure */
static int sdhci_xfer(u64 b, u64 lba, u32 *buf, int write, u32 *err) {
    u32 n, s = 0; *err = 0; dbg_int = 0;
    for (n = 0; n < 2000000; n++) if (!(RD32(b + 0x24) & 3)) break;
    if (n == 2000000) { *err = 0xE0000001; return -1; }
    WR32(b + 0x30, 0xFFFFFFFF);
    WR32(b + 0x04, (1u << 16) | 512);
    WR32(b + 0x08, (u32)lba);
    u64 a = (u64)(uintptr_t)buf, d = (u64)(uintptr_t)adma_desc;
    adma_desc[0] = (512u << 16) | 0x23;                 /* valid | end | transfer */
    adma_desc[1] = (u32)a; adma_desc[2] = (u32)(a >> 32); adma_desc[3] = 0;
    DSB();
    WR32(b + 0x58, (u32)d); WR32(b + 0x5C, (u32)(d >> 32));
    WR32(b + 0x0C, ((write ? 0x183Au : 0x113Au) << 16) | (write ? 0x0003u : 0x0013u));   /* CMD24 / CMD17, DMA + block count */
    for (n = 0; n < 4000000; n++) { s = RD32(b + 0x30); if (s & 0x8000) { *err = s; dbg_int = s; sdhci_reset_lines(b); return -2; } if (s & 2) break; }
    dbg_int = s;
    if (n == 4000000) { *err = 0xE0000004; sdhci_reset_lines(b); return -5; }
    WR32(b + 0x30, 0xFFFFFFFF);
    if (write) {                                        /* wait until the card releases DAT0 (programming done) */
        for (n = 0; n < 4000000; n++) if (RD32(b + 0x24) & (1u << 20)) break;
        if (n == 4000000) { *err = 0xE0000005; return -6; }
    }
    return 0;
}
static u64 le64(const u8 *p) { u64 v = 0; for (int i = 7; i >= 0; i--) v = (v << 8) | p[i]; return v; }
static u32 le32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

/* ---- GPT scan: lists key partitions, remembers the exact-name "logdump" partition ---- */
static u64 log_first, log_last; static int have_log, seen_boot, seen_ud;
static int gpt_scan(u64 b, int list) {
    u32 err; const u8 *h;
    if (sdhci_xfer(b, 1, sect, 0, &err) < 0) return -1;
    h = (const u8 *)sect;
    if (memcmp(h, "EFI PART", 8)) return -2;
    u64 eslba = le64(h + 72); u32 ecount = le32(h + 80), esize = le32(h + 84);
    if (esize < 128 || esize > 512 || ecount > 256) return -3;
    static const char *want[] = { "boot", "recovery", "userdata", "system", "vendor", "persist", "cache", 0 };
    u32 total = 0;
    for (u32 i = 0; i < ecount; i += 512 / esize) {
        if (sdhci_xfer(b, eslba + (u64)i * esize / 512, sect, 0, &err) < 0) return -4;
        for (u32 k = 0; k < 512 / esize; k++) {
            const u8 *e = (const u8 *)sect + k * esize;
            u32 any = 0; for (int q = 0; q < 16; q++) any |= e[q];
            if (!any) continue;
            total++;
            char name[37]; for (int q = 0; q < 36; q++) name[q] = (char)e[56 + q * 2]; name[36] = 0;
            u64 f = le64(e + 32), l = le64(e + 40);
            if (streq(name, "boot")) seen_boot = 1;
            if (streq(name, "userdata")) seen_ud = 1;
            if (streq(name, "logdump")) { log_first = f; log_last = l; have_log = 1; kprintf("logdump %llx-%llx %lluMB\n", f, l, (l - f + 1) / 2048); }
            else if (list) for (int w = 0; want[w]; w++) if (streq(name, want[w])) kprintf("%s %llx-%llx %lluMB\n", name, f, l, (l - f + 1) / 2048);
        }
    }
    if (!(seen_boot && seen_ud && total >= 10 && log_first >= 2048)) have_log = 0;      /* refuse to write unless the table looks right */
    return (int)total;
}

/* ---- persist the on-screen log into the first 16 KiB of the logdump partition (bounds-checked) ---- */
static int log_flush(void) {
    if (!have_log || !emmc_base) return -1;
    if (log_last < log_first || (log_last - log_first + 1) < 2048) return -2;       /* need >= 1 MiB */
    u32 nsec = (loglen + 511) / 512; if (!nsec) nsec = 1; if (nsec > 32) nsec = 32;
    u32 err;
    for (u32 s = 0; s < nsec; s++) {
        u64 lba = log_first + s;
        if (lba < log_first || lba > log_last) return -3;
        u32 off = s * 512, n = loglen > off ? loglen - off : 0; if (n > 512) n = 512;
        memset(wbuf, 0, 512); memcpy(wbuf, logbuf + off, n);
        if (sdhci_xfer(emmc_base, lba, (u32 *)wbuf, 1, &err) < 0) return -4;
    }
    for (u32 s = 0; s < nsec; s++) {
        u32 off = s * 512, n = loglen > off ? loglen - off : 0; if (n > 512) n = 512;
        if (sdhci_xfer(emmc_base, log_first + s, sect, 0, &err) < 0) return -5;
        if (memcmp(sect, logbuf + off, n)) return -6;
    }
    return 0;
}

/* ---- exceptions: vectors live in start.S, they call this ---- */
static volatile u32 exc_seen;
void exc_handler(u64 type, u64 esr, u64 far, u64 elr);
void exc_handler(u64 type, u64 esr, u64 far, u64 elr) {
    kcolor(C_RED); kprintf("\nEXC t%llu ESR %llx FAR %llx ELR %llx\n", type, esr, far, elr); kcolor(C_WHITE);
    if ((esr >> 26) == 0x3C) { exc_seen = 1; SET_ELR(elr + 4); return; }     /* BRK test: resume after it */
    log_flush();
    for (;;) WFI();
}

/* ---- stage bookkeeping + large summary ---- */
static const char *stg_name[12]; static int stg_res[12]; static int nstg;
static void stage(const char *name, int pass) {
    if (nstg < 12) { stg_name[nstg] = name; stg_res[nstg++] = pass; }
    kprintf("[%s] ", name); if (pass) ok("OK\n"); else bad("FAIL\n");
}
static void summary(void) {
    kscale = 2; kprintf("\n");
    for (int i = 0; i < nstg; i++) { kcolor(stg_res[i] ? C_GREEN : C_RED); kprintf("%s %s\n", stg_name[i], stg_res[i] ? "OK" : "FAIL"); }
    kscale = 1; kcolor(C_WHITE);
}

/* ---- MMU + caches (identity map) ---- */
static u64 cnt(void);
#ifdef HOST_TEST
static int mmu_enable(void) { kprintf("(host: MMU skipped)\n"); return 1; }
#else
static u64 l1_tab[512] __attribute__((aligned(4096)));
static u64 l2_tab[512] __attribute__((aligned(4096)));
#define PTE_BLK   0x1ULL
#define PTE_TBL   0x3ULL
#define PTE_AF    (1ULL << 10)
#define PTE_ISH   (3ULL << 8)
#define PTE_XN    ((1ULL << 53) | (1ULL << 54))
#define PTE_IDX(i) ((u64)(i) << 2)
static void dcache_inval(u64 start, u64 len) {
    for (u64 a = start & ~63ULL; a < start + len; a += 64) __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
static int mmu_enable(void) {
    /* attr idx: 0 = Device-nGnRnE, 1 = Normal WB, 2 = Normal non-cacheable */
    memset(l1_tab, 0, sizeof l1_tab); memset(l2_tab, 0, sizeof l2_tab);
    l1_tab[0] = 0x00000000ULL | PTE_BLK | PTE_AF | PTE_IDX(0) | PTE_XN;            /* 0..1G   peripherals */
    l1_tab[1] = 0x40000000ULL | PTE_BLK | PTE_AF | PTE_IDX(0) | PTE_XN;            /* 1..2G   peripherals */
    l1_tab[2] = (u64)(uintptr_t)l2_tab | PTE_TBL;                                   /* 2..3G   RAM bank 0  */
    for (u64 i = 0; i < 16; i++) l2_tab[i]      = (0x80000000ULL + i * 0x200000) | PTE_BLK | PTE_AF | PTE_ISH | PTE_IDX(1);                 /* 32 MiB WB: image, stack, tables */
    for (u64 i = 16; i < 32; i++) l2_tab[i]     = (0x80000000ULL + i * 0x200000) | PTE_BLK | PTE_AF | PTE_ISH | PTE_IDX(2) | PTE_XN;        /* 32 MiB NC: DMA buffers */
    for (u64 i = 234; i < 252; i++) l2_tab[i]   = (0x80000000ULL + i * 0x200000) | PTE_BLK | PTE_AF | PTE_IDX(0) | PTE_XN;                    /* framebuffer 0x9d400000.. as device */
    dcache_inval(0x80000000ULL, 0x200000);          /* drop stale lines over our image / bss / stack / tables */
    dcache_inval(0x82100000ULL, 0x10000);           /* and over the DMA buffers */
    u64 mair = 0x44FF00ULL;
    u64 tcr  = 25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12) | (1ULL << 23) | (2ULL << 32);   /* 39-bit VA, 4K, WB walks, 40-bit PA */
    u64 ttbr = (u64)(uintptr_t)l1_tab;
    u64 sctlr = SYSREG(sctlr_el1); sctlr |= (1ULL << 0) | (1ULL << 2) | (1ULL << 12); sctlr &= ~(1ULL << 19);
    __asm__ volatile(
        "dsb sy\n"
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "isb\n"
        "tlbi vmalle1\n"
        "dsb nsh\n"
        "isb\n"
        "ic iallu\n"
        "dsb nsh\n"
        "isb\n"
        "msr sctlr_el1, %3\n"
        "isb\n"
        :: "r"(mair), "r"(tcr), "r"(ttbr), "r"(sctlr) : "memory");
    return (SYSREG(sctlr_el1) & 0x1005) == 0x1005;
}
#endif
static u8 memtest[65536];

#if RISKY >= 3
static void stage_main(const u8 *dtb) {
    fdt_walk(dtb, 0);
    kprintf("EL%llu MIDR %llx MPIDR %llx\n", SYSREG(currentel) >> 2, SYSREG(midr_el1), SYSREG(mpidr_el1));
    exc_seen = 0; TEST_BRK(); stage("EXC", exc_seen);

    emmc_base = sdhci_base ? sdhci_base : 0xC0C4000;
    WR32(emmc_base + 0x34, RD32(emmc_base + 0x34) | 0x037F003B);       /* latch CC/TC/BRR + error status bits (no interrupts) */
    u32 err = 0; int r = sdhci_xfer(emmc_base, 1, sect, 0, &err);
    int gpt = (r >= 0 && !memcmp(sect, "EFI PART", 8));
    stage("EMMC", r >= 0); if (r < 0) kprintf("  err %08x INT %08x\n", err, dbg_int);
    stage("GPT", gpt);
    int total = gpt ? gpt_scan(emmc_base, 1) : -9;
    kprintf("partitions: %d  logdump: %s\n", total, have_log ? "found" : "MISSING");
    int lr = log_flush(); stage("LOG", lr == 0); if (lr) kprintf("  log_flush -> %d\n", lr);

#if RISKY == 4
    kprintf("entering MMU stage...\n"); log_flush();          /* persist everything so far before the risky step */
    int m = mmu_enable();
    stage("MMU", m);
    if (m) {
        kprintf("SCTLR now %08llx\n", SYSREG(sctlr_el1));
        r = sdhci_xfer(emmc_base, 1, sect, 0, &err);
        stage("EMMC2", r >= 0 && !memcmp(sect, "EFI PART", 8));
        u64 f = SYSREG(cntfrq_el0), t0 = cnt(), t1 = t0;
        for (u64 i = 0; i < 400000000ULL && (t1 - t0) < f / 10; i++) t1 = cnt();
        stage("TIMER", (t1 - t0) >= f / 10);
        t0 = cnt(); u32 bad_n = 0;
        for (u32 p = 0; p < 4; p++) { for (u32 i = 0; i < sizeof memtest; i++) memtest[i] = (u8)(i * 7 + p); for (u32 i = 0; i < sizeof memtest; i++) if (memtest[i] != (u8)(i * 7 + p)) bad_n++; }
        t1 = cnt();
        stage("RAM", bad_n == 0); kprintf("  4x64KB fill+check %llu us\n", (t1 - t0) * 1000000ULL / f);
    }
#endif
    summary();
    int fr = log_flush(); kprintf("final log flush: %d\n", fr);
}
#endif

#define SRC_VER "src5"
void kmain(const void *dtb) {
    fb_clear();
    kcolor(C_CYAN); kprintf("KB LOADER (C) %s %s %s\n", BUILD_TAG, RISKY == 4 ? "MMU" : RISKY == 3 ? "LOG" : RISKY ? "FULL" : "SAFE", SRC_VER); kcolor(C_WHITE);
#if RISKY < 3
    cpu_info();
#endif
    kprintf("x0(DTB) = %llx\n", (u64)(uintptr_t)dtb);
#if RISKY >= 3
    stage_main((const u8 *)dtb);
#else
    fdt_walk((const u8 *)dtb, RISKY == 0);
#if RISKY == 1
    risky_tests();
#endif
    kcolor(C_YEL); kprintf("\nDONE. Photo the screen.\n"); kcolor(C_WHITE);
#endif
    for (;;) { WFI();
#ifdef HOST_TEST
        return;
#endif
    }
}
