# lavender-boot
Bare-metal boot test loader for Redmi Note 7 (lavender, SDM660). Packs into a boot.img matching the stock ABL layout.
- `boot_c_safe.img`: shows CPU info + DTB dump on screen, touches no unknown hardware.
- `boot_c_full.img`: timer, PSCI, GICv3 read, CPU_ON for core 1.
- `boot_c_emmc.img`: reads eMMC sector 0/1 (GPT) via SDHCI PIO and lists key partitions.
Restore Android by flashing your own boot_backup.img to the Boot partition.
