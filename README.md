# lavender-boot
Bare-metal boot test loader for Redmi Note 7 (lavender, SDM660). Packs into a boot.img matching the stock ABL layout.
- `boot_c_safe.img`: shows CPU info + DTB dump on screen, touches no unknown hardware.
- `boot_c_full.img`: timer, PSCI, GICv3 read, CPU_ON for core 1.
- `boot_c_log.img`: eMMC ADMA read+write, GPT, exception test, persists on-screen log to the logdump partition.
- `boot_c_mmu.img`: same plus identity-mapped MMU+caches stage.
Restore Android by flashing your own boot_backup.img to the Boot partition.
