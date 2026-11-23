# S22 Checklist (Windows 11 ARM64 — Hardware Real SM-S901E / r0q)
tags: #s22 #windows11 #arm64 #checklist
Updated 2026-11-22 · Windows 11 Pro ARM64 `26300.9457` · Mu-Silicium `r0q`

> Este checklist separa rigorosamente o que foi **verificado no S22 físico (`SM-S901E`)** do que está em trazida/experimental ou ainda não portado.

## ✅ Verificado no Hardware Físico (Galaxy S22 SM-S901E)
- [x] **Backup completo de partições críticas (EFS/UFS)** + guia de proteção [scripts/UFS-PARTITIONS-SAFETY.md](scripts/UFS-PARTITIONS-SAFETY.md) e [docs/REPLICATION_GUIDE.md](docs/REPLICATION_GUIDE.md)
- [x] **UEFI Mu-Silicium (`r0q`) customizado em Android v4 `boot.img`** flashado em `sda25` (`boot`) via TWRP (`patches/mu-silicium/mu-silicium-r0q.patch` + `silicium-acpi-r0q.patch`)
- [x] **Navegação UEFI sem botão Power**: contador automático de **15 s** no Boot Manager + **Vol+ & Vol− juntos = Enter imediato**
- [x] **UEFI Charger Mode (`ChargerMode.efi`)**: leitura ao vivo do MAX77705 (I2C5 `0x36` 2 mΩ + `0x69`), carregamento 2.5A, exibindo **%**, **V/mV**, **mA**, **W** e **°C**, OLED saver e saída sem botão Power
- [x] **Reboot 1-Click para TWRP (`scripts/Reiniciar_TWRP.cmd` + `RebootRecovery.efi`)**: escreve `\EFI\S22\reboot_twrp.flag` na ESP → `PlatformBootManager.c` configura PMK8350 `SDAM_2 0x7148` via `SPMIDxe` → warm reboot direto pro TWRP com zero escrita em UFS
- [x] **Carregamento rápido no TWRP (`scripts/fc.sh`)**: `siop_level=100`, cores 5–7 offline e `powersave` nos cores 0–4
- [x] **Boot completo até a Área de Trabalho do Windows 11 Pro ARM64 (`26300.9457` pt-BR) no UFS interno (`sda41` NTFS 4K)**:
  - `Waipio/DSDT.asl` (`UFS0._STA = 0x0F`) + correção de `UniquePartitionGUID` duplicados na GPT do `sda` mantendo `#1..#6` (EFS/modem) intactos
  - OOBE/Wi-Fi pulados nativamente via `Unattend.xml` + desligamento limpo verificado (`sda41` monta `rw` no TWRP sem `fsck.ntfs`)
- [x] **Touchscreen nativo STMicroelectronics FTS2BA61Y (`fts2ba61y.sys` v1.0.1.0, `ACPI\STMT0001`)**:
  - Trilhos **`PM8350C LDO8` (1,8 V VDD)** e **`PM8350C LDO11` (3,0 V AVDD)** ligados no UEFI (`PlatformBootManager.c` via `CmdDbDxe` + `RpmhDxe`, `DRV_ID = 2`)
  - Driver KMDF ARM64 com SPI bit-bang em TLMM (GPIOs 28–31 + IRQ 46), RMW preservando `EGPIO_ENABLE` — toques calibrados e responsivos no S22 real
- [x] **USB 3.0 OTG Host (`URS0` `QCOM0C8B`/`PNP0CA1` + `urssynopsys.sys` + MAX77705 MUIC `0x06=0x09` + Boost 5 V / 1500 mA)**:
  - `USB\ROOT_HUB30` iniciado sem erros, Hub USB-C, mouse, teclado, pendrive e USB tethering funcionais no Windows 11
- [x] **Wi-Fi 6E interno — 5 GHz (`Qualcomm WCN6855 / QCA6490`, `PCI\VEN_17CB&DEV_1103&SUBSYS_010817CB`) CONECTADO E NAVEGANDO NO WINDOWS 11!** 🎉
  - **UEFI (`PlatformBootManager.c` + `S22Pcie0PhyTbl.h` + `MemoryMapLib.c`)**: trilhos RPMh (`ldob5/6`, `smpb10/11@980mV/12`, `smpe2`, `smpc1`, `ldoe7`), sequência `GPIO 204` (`XO_CLK` com fix `EGPIO_ENABLE`) + `GPIO 80` (`WLAN_EN`) + `GPIO 94` (`PERST#`), PHY QMP Gen3x1 bloqueado (`Gen2 x1`, `ep=0x110317CB`), iATU CFG0 4 KB sem aliasing, `apps_smmu` (`0x15000000`) bypass emulado limpo via `SMR[112]=0x80011C00` / `S2CR[112]=0x4A` / `CB[74].SCTLR=0` (0 faults!), `PARF_INT_ALL_MASK |= 0x7F802000`, DesignWare `iMSI-RX` armado em `0x17100040`
  - **ACPI (`acpi/r0q/`)**: `MCFG.aml` (`0x60000000`) + `APIC.aml` com **`0x0D` GIC MSI Frame (`0x17100000`, `INTID 64..95`)** + `SSDT.asl` (`PCI0` `_CCA = 0`, `WPDC` `ACPI\S22W0001` com `msi0..msi7` `0xAD..0xB4`)
  - **Bridge MSI (`s22wlanpdc.sys` v1.0.15.0 produção enxuta)**: intercepta os MSIs no DWC `iMSI-RX` (`Bank 2`) e injeta os vetores habilitados direto em `GICD_ISPENDR[2]`, executando a ISR do `qcwlan8280` com 0 BSOD e 0 perdas
  - **Fix de Coerência DMA SM8450 (`scripts/patch_qcwlan8280_wc.ps1` → `qcwlan8280_wc.sys`)**: patch de 3 palavras (`0x14002d1f0`, `0x1402a8a20`, `0x1400084d4`) mudando as alocações DMA do `qcwlan8280.sys` de `MmCached` para **`MmWriteCombined` (`Normal Non-Cacheable`)** para compatibilidade com o barramento PCIe0 não-coerente do SM8450 — `WCN6855` entra em Mission Mode (`bEe=2`), cria interfaces DBS (`Wi-Fi` + `Wi-Fi 3`), conecta e navega em 5 GHz!

## 🔧 Em Trazida / Próximos Passos (Agora testáveis ao vivo via Wi-Fi sem TWRP!)
- [ ] **Wi-Fi 2.4 GHz TX (`PMR735A s3e.e` PA)**: habilitar o trilho 2,2 V `s3e.e` via AOSS QMP (`0x0C300000`) no Windows (`s22wlanpdc.sys`) após `qcpep8450.sys` inicializar (5 GHz já funciona 100%)
- [ ] **Bateria % + Carregamento + Limite 50–100% (`drivers/max77705-battery/`, `ACPI\MAXM7705`)**: testar ao vivo sobre Wi-Fi e resolver `PANIC_STACK_SWITCH (0x2B)` antes de habilitar `_STA = 0x0F` em `SSDT.asl`
- [ ] **USB-C Dinâmico / Hot-Plug / USB-PD / Modo Device (`drivers/max77705-usbc/`, `ACPI\MAXM7706`)**: testar ao vivo sobre Wi-Fi e validar múltiplos dispositivos no hub USB (mouse + teclado + tethering)

## ❌ Ainda Não Portados (Fases Futuras)
- [ ] **GPU Adreno 730 (DirectX / WDDM)** — depende de driver Qualcomm Adreno ARM64 compatível com SM8450
- [ ] **Áudio (Alto-falantes estéreo CS35L41 + microfones)** — depende de ADSP/Q6APM/AudioReach para Waipio
- [ ] **Bluetooth (WCN6855 UART)**
- [ ] **Sensores (SLPI: acelerômetro, giroscópio, bússola, luz ambiente STK33915, proximidade)**
- [ ] **Câmeras (CAMSS: IMX374, S5K3K1, IMX563, S5KGN3) + Flash LED**
- [ ] **Modem celular (4G/5G IPA v5.1) + GPS/GNSS**
- [ ] **Vibração (CS40L26 Halo Core DSP)**
- [ ] **NFC (NXP PN547/SN110)**
- [ ] **Leitor de digitais ultrassônico (QBT2608)** — bloqueado por QSEE TA proprietário
- [ ] **Suspensão profunda (Modern Standby / S0ix)**
