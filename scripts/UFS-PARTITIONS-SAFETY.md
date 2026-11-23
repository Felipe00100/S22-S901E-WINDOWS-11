# Segurança e Cuidados com as Partições UFS do Galaxy S22 (SM-S901E / `r0q`)

> **ALERTA CRÍTICO:** Partições proprietárias do Samsung Galaxy S22 contêm dados únicos de fábrica (IMEI, calibração de rádio/modem, chaves DRM/Knox, bootloader). **NUNCA apague, formate ou sobrescreva** nenhuma partição listada na tabela de proteção abaixo.

---

## 1. Backup Obrigatório Antes de Qualquer Modificação (via TWRP)

Com o celular no **TWRP** e conectado via ADB:

```bash
# 1. Salvar a tabela GPT inteira da LUN0 (sda)
adb shell "sgdisk --backup=/tmp/sda_gpt_backup.bin /dev/block/sda"
adb pull /tmp/sda_gpt_backup.bin ./sda_gpt_backup.bin

# 2. Listar e salvar o mapa completo de partições por nome
adb shell "ls -la /dev/block/by-name/" > s22_by_name.txt
adb shell "parted /dev/block/sda unit s print" > s22_sda_layout.txt

# 3. Backup das partições críticas únicas do aparelho (EFS / Modem / Persist)
for p in efs sec_efs modemst1 modemst2 fsg fsc persist param up_param; do
  adb shell "dd if=/dev/block/by-name/$p of=/tmp/${p}.img bs=4M" && adb pull "/tmp/${p}.img" "./backup_${p}.img"
done
```

---

## 2. Partições INTOCÁVEIS vs. Partições de Trabalho

| Categoria | Partições | Regra |
| :--- | :--- | :--- |
| **Fábrica / Rádio (PERDA IRREVERSÍVEL)** | `efs`, `sec_efs`, `modemst1`, `modemst2`, `fsg`, `fsc`, `persist`, `ddr` | **NUNCA tocar.** Perder `efs`/`modemst*` zera o IMEI permanentemente. |
| **Bootloader / Firmware** | `xbl`, `xbl_config`, `abl`, `tz`, `hyp`, `devcfg`, `rpm`, `aop`, `keymaster`, `qupfw`, `uefisecapp`, `vbmeta`, `param`, `up_param` | **NUNCA alterar** via particionamento do Windows. |
| **Recovery (TWRP)** | `recovery` | **Manter intacta!** É a nossa rede de segurança (e acessível direto pelo menu `"Reboot to Recovery (TWRP)"` do nosso UEFI). |
| **Boot / Kernel** | `boot`, `vendor_boot`, `dtbo` | Fazer backup antes de testar o `boot.img` com o `Mu-Silicium`. |
| **Espaço para Windows (SOMENTE encolher `userdata`)** | `userdata` → criar `esp` (FAT32, 300 MB) + `win` (NTFS, >= 30 GB) | **ÚNICA área** onde devemos criar partições novas para o Windows 11 ARM64! |

---

## 3. Procedimento Seguro para UFS no Samsung Snapdragon (`Modding-UFS`)

1. No `Mu-Silicium`, entre em **Mass Storage** apenas **depois** de já ter feito o backup da GPT (`sda_gpt_backup.bin`) no TWRP.
2. No Gerenciador de Disco do Windows no PC, clique com o botão direito no disco UFS e selecione **Online** (Samsung deixa o bit offline por padrão).
3. **NÃO reinicie o celular ainda!** Volte ao TWRP/ADB (ou repare imediatamente com `gdisk /dev/block/sda` → `r` → `c` → `w` → `y`) para restaurar o cabeçalho GPT secundário sem corromper nenhuma partição existente.
4. Encolha **somente** a partição `userdata` para liberar espaço livre ao final de `sda`, criando:
   - **`esp`**: FAT32, ~300 MB, tipo `EF00` (EFI System Partition)
   - **`win`**: NTFS, >= 30 GB, tipo `0700` (Microsoft Basic Data)
5. Aplique a ISO `Windows11_Client_arm64_pt-br_26300_9457.iso` (`install.wim`) **somente** na partição `win`, gere o BCD em `esp` (`bcdboot W:\Windows /s S: /f UEFI`) e habilite assinatura de teste no BCD (`bcdedit /store S:\EFI\Microsoft\Boot\BCD /set {default} testsigning on`).
