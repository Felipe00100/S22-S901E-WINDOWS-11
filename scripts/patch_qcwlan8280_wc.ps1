<#
.SYNOPSIS
    Generates test-signed qcwlan8280_wc.sys (MmWriteCombined / Normal Non-Cacheable DMA)
    for Samsung Galaxy S22 (SM-S901E / SM8450 / Waipio) non-coherent PCIe0 WCN6855 Wi-Fi 6E.

.DESCRIPTION
    Qualcomm qcwlan8280.sys (v1.0.3451.5100, SC8280XP) hardcodes dmaV3AllocApi=1 (0x14002d1f0)
    and allocates MHI/BHI/CE/DP DMA rings via AllocateCommonBufferWithBounds(..., CacheType=MmCached).
    SM8450 PCIe0 is physically non-coherent (no CPU cache snooping), causing stale CPU cache reads
    in MHIProcessEventRing and BHI SBL downloads.

    This 3-word patch switches DMA allocations to MmAllocateContiguousMemorySpecifyCache(..., MmWriteCombined):
      1. 0x14002d1f0 (file 0x2c5f0):  0xb90a2136 -> 0xb90a213f  (str wzr, [x9, #0xa20]  ; dmaV3AllocApi = 0)
      2. 0x1402a8a20 (file 0x2a6c20): 0x00000001 -> 0x00000000  (initial dmaV3AllocApi = 0)
      3. 0x1400084d4 (file 0x78d4):   0xb9402924 -> 0x52800044  (mov w4, #2             ; MmWriteCombined = Normal-NC)

    Safety checks enforced before patching:
      - Exact SHA-256 verification of validated input qcwlan8280.sys v1.0.3451.5100
        (override only with -AllowUnknownHash if intentionally testing another matching build)
      - PE32+ header & ARM64 machine type (0xAA64) validation
      - Exact original 32-bit word verification at all 3 patch offsets
      - Post-sign Authenticode verification
#>
param(
    [switch]$AllowUnknownHash
)

$ErrorActionPreference = "Stop"

$ExpectedInputSha256 = "89DE2259DDB65661AA8112A929F602D4799EFA19544C4F1DB6E0F90CA0AC1407"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$Src      = Join-Path $RepoRoot "S901E Drivers\WiFi-WCN6855-Qualcomm\qcwlan8280.sys"
$Dst      = Join-Path $RepoRoot "S901E Drivers\WiFi-WCN6855-Qualcomm\qcwlan8280_wc.sys"
$SignTool = "C:\ADK\bin\10.0.26100.0\x64\signtool.exe"

if (-not (Test-Path $Src)) { throw "Original qcwlan8280.sys not found at $Src" }

$InputHash = (Get-FileHash $Src -Algorithm SHA256).Hash.ToUpperInvariant()
if ($InputHash -ne $ExpectedInputSha256) {
    if (-not $AllowUnknownHash) {
        throw "Input SHA-256 mismatch!`n  Expected: $ExpectedInputSha256 (qcwlan8280.sys v1.0.3451.5100)`n  Actual:   $InputHash`nRefusing to patch an unverified binary (use -AllowUnknownHash only at your own risk)."
    }
    Write-Warning "Bypassing SHA-256 check (-AllowUnknownHash): $InputHash"
} else {
    Write-Host "[OK] Input SHA-256 verified: $InputHash" -ForegroundColor Green
}

python -c @"
import struct
data = bytearray(open(r'$Src', 'rb').read())
assert data[:2] == b'MZ', 'Not a valid MZ/PE executable'
pe = struct.unpack_from('<I', data, 0x3c)[0]
assert data[pe:pe+4] == b'PE\x00\x00', 'Invalid PE header signature'
machine = struct.unpack_from('<H', data, pe + 4)[0]
assert machine == 0xAA64, f'Expected ARM64 PE (0xAA64), got {hex(machine)}'
opt_magic = struct.unpack_from('<H', data, pe + 24)[0]
assert opt_magic == 0x20B, f'Expected PE32+ (0x20B), got {hex(opt_magic)}'

assert struct.unpack_from('<I', data, 0x78d4)[0]   == 0xb9402924, 'Unexpected word at 0x78d4'
assert struct.unpack_from('<I', data, 0x2c5f0)[0]  == 0xb90a2136, 'Unexpected word at 0x2c5f0'
assert struct.unpack_from('<I', data, 0x2a6c20)[0] == 0x00000001, 'Unexpected word at 0x2a6c20'

struct.pack_into('<I', data, 0x78d4,   0x52800044)
struct.pack_into('<I', data, 0x2c5f0,  0xb90a213f)
struct.pack_into('<I', data, 0x2a6c20, 0x00000000)

sec_dir = pe + 24 + 112 + 4 * 8
cert_off, cert_sz = struct.unpack_from('<II', data, sec_dir)
if cert_off > 0 and cert_off + cert_sz == len(data):
    data = data[:cert_off]
    struct.pack_into('<II', data, sec_dir, 0, 0)
struct.pack_into('<I', data, pe + 24 + 64, 0)
open(r'$Dst', 'wb').write(data)
print('[OK] PE32+ ARM64 validated & 3-word patch applied')
"@
if ($LASTEXITCODE -ne 0) { throw "Python patch failed" }

$Cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq "CN=S22 Windows Test Cert" } | Select-Object -First 1
if (-not $Cert) { throw "CN=S22 Windows Test Cert not found in Cert:\CurrentUser\My" }

& $SignTool sign /v /s My /sha1 $Cert.Thumbprint /fd SHA256 "$Dst"
if ($LASTEXITCODE -ne 0) { throw "signtool failed" }

$Sig = Get-AuthenticodeSignature "$Dst"
if (-not $Sig.SignerCertificate -or $Sig.SignerCertificate.Thumbprint -ne $Cert.Thumbprint) {
    throw "Post-sign certificate verification failed (Status=$($Sig.Status))"
}

Write-Host "[OK] Patched & signed: $Dst (Signer=$($Sig.SignerCertificate.Subject), Status=$($Sig.Status))" -ForegroundColor Green
Write-Host "SHA256: $((Get-FileHash $Dst -Algorithm SHA256).Hash)" -ForegroundColor Cyan
