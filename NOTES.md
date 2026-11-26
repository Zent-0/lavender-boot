# lavender-boot: engineering notes (handoff file)

Goal: boot Apple's open-source XNU (iOS 16 generation, iPhone X userland) on the
Redmi Note 7 (lavender, Snapdragon 660), the same way `ios-sweet` (Redmi Note 10 Pro)
and `ios-topaz` (Redmi Note 12 4G, Snapdragon 685) did.

Owner works from a phone only (no PC). Writes Azerbaijani. Photographs the Note 7 screen
with a second phone to report results. Any AI agent reading this: keep answers short,
give complete files rather than diffs when possible, and update this file after every stage.

## SAFETY RULES (do not break)
- The loader only WRITES to the `logdump` partition (log_flush). Never write to any other
  partition (persist, modem, boot-critical ones, userdata...). Reads of GPT/LBA0 are fine.
- Keep every new risky feature behind its own RISKY level so a bad build is easy to bisect.
- Always keep the previous working build (boot_testN.img / update-rN.zip) to roll back.

## Hardware facts (read from the device by the loader)
- SoC SDM660 (`qcom,sdm660-mtp`). 8 cores, runs at **EL1**, x0 = DTB at `0x853FE000`
  (DTB size 0x80054, version 17).
- Cores 0-3: Kryo 260 Silver, MIDR `0x51AF8014` (Cortex-A53 class), MPIDR `0x80000000..3`.
  Cores 4-7: Kryo 260 Gold, MIDR `0x51AF8002` (Cortex-A73 class), MPIDR `0x80000100..103`.
  DT `reg` values: `0 1 2 3 100 101 102 103`.
- Core 0 ID regs: PFR0 `0x01002222` (GICv3 sysreg interface present), ISAR0 `0x11120`
  (AES/SHA1/SHA2/CRC32, no LSE atomics, no RDM), ISAR1 0, **MMFR0 `0x1122`** (PARange 40-bit,
  16-bit ASID, **TGran16 = 0 on core 0, i.e. 16KB granule NOT supported on the silver/A53 core**).
- Timer: CNTFRQ 19.2 MHz. Arch-timer PPIs in DT are Qualcomm-style PPI 1,2,3,0, which means
  INTID **17, 18, 19, 16** (sec phys, ns phys, virt, hyp). The loader uses the virtual timer
  (INTID 19). Do NOT assume the usual 30/27.
- GICv3: GICD `0x17A00000`, GICR `0x17B00000` (one 128 KB frame per CPU), GICD_CTLR reads
  `0x12` (ARE_NS + EnableGrp1A), TYPER `0x007B04F3`.
- PSCI method: **smc**. PSCI_VERSION returns 1.0. CPU_ON = `0xC4000003`, AFFINITY_INFO = `0xC4000004`.
  Pass the DT `reg` value as target (works as is).
- eMMC: SDHCI MSM v5 at `0x0C0C4000` (second controller `0x0C084000`), uses ADMA2.
  GPT has 66 partitions. Partition `logdump` (64 MB) is used as the on-device log.
- Other DT nodes: UART `0x0C170000`, `0x0C1B0000`; display `qcom,mdss_mdp` at `0x0C900000`.
- RAM: `0x80000000-0xFFFFFFFF` and `0x100000000-0x17E4BFFFF`.
- Bootargs seen: `rcupdate.rcu_expedited=1 androidboot.hardware=qcom user_debug=31 msm_rtb.filter=`.
- Reserved regions (first ones): wlan_msa_guard `0x85600000`, wlan_msa_mem `0x85700000`,
  removed_regions `0x85800000 +0x3700000`, modem_fw `0x8AC00000`, adsp_fw `0x92A00000`,
  pil_mba `0x94800000`, cdsp_fw `0x94A00000`, splash `0x9D400000 +0x2300000`,
  dfps_data `0x9F7FF000`, ramoops `0xB0000000 +0x400000`.
- The loader's own DMA/shared block is at `0x82100000` (mapped non-cacheable when MMU is on).

## Repo layout
`main.c` (loader, one file), `start.S`, `link.ld`, `font.h`, `header.bin`/`footer.bin`/`vbmeta.bin`,
`mkboot.py` (packs an Android boot image), `Makefile`, `.github/workflows` (CI build).
`main.c` is built in stages selected by `RISKY`: 0/1 SAFE/FULL probes, 3 LOG, 4 MMU, 5 IRQ+SMP.
It also builds on a PC with `-DHOST_TEST` (fake sysregs) for logic tests.

## Stage log (photos of the screen)
| Build | Result |
|---|---|
| r1 SAFE/FULL | CPU info, DT dump, timer 19.2 MHz, PSCI_VERSION OK, GICv3 found, CPU_ON for core 1 worked (core 1 alive) |
| r2/r3 EMMC | SDHCI registers readable, `read LBA0` FAIL `r=-4 err=e0000003` |
| r4 EMMC | ADMA read works, GPT parsed (66 partitions) |
| r7 LOG | EXC (BRK test, `ESR f2000000`) OK, EMMC OK, GPT OK, log written to logdump OK |
| r7 MMU | MMU + caches on (`SCTLR 0xC01805`), EMMC2/TIMER/RAM OK |
| r8 IRQ | GICD/GICR/GICC OK, **IRQ FAIL, SMP FAIL** (timer INTID assumed wrong) |
| r9 IRQ | timer INTIDs read from DT, **IRQ OK (5 ticks), SMP OK, all 7 secondary cores alive** (state `s1f`) |
| r10 IRQ | per-core ID_AA64MMFR0 printed: **cores 0-3 `0x1122` (TGran16 = 0), cores 4-7 `0x101122` (TGran16 = 1)**. The gold (A73-class) cores support the 16K granule, like ios-topaz |
| r11 IRQ | gold cores enable their MMU with 16K-granule tables (T0SZ 28, TG0 16K, 32 MiB L2 blocks); every secondary also does a private 4 KB RAM fill+check. Expected states: silver `s5f`, gold `s7f` (0x20 = 16K tables in use, 0x40 = RAM check passed). Result: pending |

Per-core state bits in `SHARED->state`: 1 started, 2 MMU on, 4 redistributor init, 8 CPU interface,
0x10 timer IRQ received, 0x20 (r11+) running on 16K-granule tables, 0x40 (r11+) private RAM check passed.
`s1f` (r9/r10) or `s5f`/`s7f` (r11) = everything passed. In the log line `cN onX`, X is the PSCI CPU_ON return
value (0 = success), not an "online" flag.

## What the references did (from public reports, verify in their NOTES.md)
- `dipa-studio/ios-sweet`: bare-metal shim packed as an Android boot.img v2, started with
  `fastboot boot` (nothing flashed); XNU `xnu-8796` is built on GitHub macOS runners
  (`blacktop/darwin-xnu-build`) with a patch series; the shim generates an Apple-style device tree (ADT)
  and `mkpayload.py` packs kernel + ADT. Uses 16K pages, GICv3, timers, PSCI. iPhone X userland (iOS 16.x).
- `MakrSas/ios-topaz` (SDM685 = 4x A73 + 4x A53): the A53 cores cannot do 16K pages, so they are
  switched off and only the four A73 cores run XNU. This is the closest model for lavender.

## Next steps (in order)
1. DONE (r10): gold cores 4-7 report TGran16 = 1.
2. r11 (now): 16K-granule MMU + IRQ + RAM check on gold cores. If a gold core stays at state `1` it hung while switching granule: re-check TCR/TTBR (36-bit VA, start level 2, 16 KB aligned table).
3. Park cores 0-3 (PSCI CPU_OFF or WFI) and run the whole stack from a single gold core (core 4) as the boot CPU for XNU.
4. Read `ios-sweet`/`ios-topaz` NOTES.md fully; list which XNU patches are SoC-independent (Apple sysreg handling, AIC -> GICv3, timer, PMGR stubs, ADT format) and which are SoC-specific.
5. Framebuffer (splash region `0x9D400000`) for visible output from XNU; UART as a backup log.
6. Build XNU through GitHub Actions on a macOS runner (phone-only workflow), pack payload, boot to first XNU log line.

## Corrections to earlier assumptions
- Timer INTID is 19 (DT), not 30.
- The first 16K-granule reading was from core 0 (silver); the gold cores (r10) do support it.

## Restore point
`boot_a17.img` (64 MB) is a backup of the working Android 17 ROM boot partition (header v2, kernel ~20 MB, ramdisk ~3.9 MB).
Restore with OrangeFox: Install -> Install Image -> boot. Backup zip `backup-boot.zip` dd's the boot partition to /sdcard1 and /sdcard with md5 check.
