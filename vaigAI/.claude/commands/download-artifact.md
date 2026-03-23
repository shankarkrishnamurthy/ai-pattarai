# Download Build Artifact from Citrite SJC Repo

Download build artifacts, images, and packages from the SJC Artifactory repository at `sjc-repo.citrite.net`.

## Input

The user provides a version string or build identifier as `$ARGUMENTS` (e.g., `14.1-62.7018`, `zion_62_7018`).

## Credentials

- **URL**: `https://sjc-repo.citrite.net/nwa-virtual-netscaler-build/builds_ns`
- **Username**: `svc_sdx_team_jfrog`
- **Password/API Key**: `YOUR_JFROG_TOKEN_HERE`

## Release Code Names

| Version | Code Name | Build Path Prefix |
|---------|-----------|-------------------|
| `14.1` | `zion` | `builds_zion/build_zion_<ver>` |
| `13.1` | `artesa` | `builds_artesa/build_artesa_<ver>` |
| `13.0` | `mana` | `builds_mana/build_mana_<ver>` |
| `12.1` | `kamet` | `builds_kamet/build_kamet_<ver>` |
| `12.0` | `oban` | `builds_oban/build_oban_<ver>` |
| `11.1` | `kopis` | `builds_kopis/build_kopis_<ver>` |
| `11.0` | `ion` | `builds_ion/build_ion_<ver>` |
| `10.5` | `tagma` | `builds_tagma/build_tagma_<ver>` |
| `10.1` | `dara` | `builds_dara/build_dara_<ver>` |

## Build Path Convention

The build path encodes the version with **underscores** (dots replaced):
- `14.1-70.18` -> `build_zion_70_18`
- `14.1-62.7018` -> `build_zion_62_7018`
- `13.1-37.5` -> `build_artesa_37_5`

Full URL pattern:
```
https://sjc-repo.citrite.net/nwa-virtual-netscaler-build/builds_ns/builds_<codename>/build_<codename>_<ver>/
```

## Workflow

### 1. Parse the Version

Convert the user-provided version string into the Artifactory path:
- Extract version prefix (e.g., `14.1`) -> map to code name (e.g., `zion`)
- Extract build suffix (e.g., `62.7018`) -> replace dots with underscores (e.g., `62_7018`)

### 2. List Available Artifacts

```bash
curl -s -u svc_sdx_team_jfrog:YOUR_JFROG_TOKEN_HERE \
  -L "https://sjc-repo.citrite.net/nwa-virtual-netscaler-build/builds_ns/builds_<codename>/build_<codename>_<ver>/" \
  | python3 -c "
import re, sys
html = sys.stdin.read()
for m in re.finditer(r'<a href=\"([^\"]+)\">([^<]+)</a>\s+(\S+\s+\S+)\s+([\d.]+ \w+|-)$', html, re.MULTILINE):
    href, name, date, size = m.groups()
    if name != '../':
        print(f'{size:>12}  {name}')
"
```

### 3. Download the Requested Artifact

```bash
curl -fSL -u "svc_sdx_team_jfrog:YOUR_JFROG_TOKEN_HERE" \
  -o /path/to/output/filename \
  "https://sjc-repo.citrite.net/nwa-virtual-netscaler-build/builds_ns/builds_<codename>/build_<codename>_<ver>/<filename>"
```

Also works with `repo.citrite.net` Artifactory paths:
```bash
curl -fSL -u "svc_sdx_team_jfrog:YOUR_JFROG_TOKEN_HERE" \
  -o /path/to/output/filename \
  "https://repo.citrite.net/artifactory/<repo>/<artifact-path>"
```

**IMPORTANT**: Always use `curl` directly. Do **not** use `pull.py`.

**Note**: The SDX single-bundle filename uses **lowercase** `build-sdx-` prefix, not `build-SDX-`.

## Common Artifact File Patterns

### Main Build Images

| Target | Filename Pattern | Description |
|--------|------------------|-------------|
| **SDX Single Bundle** | `build-sdx-<ver>.tgz` | Full SDX upgrade: DOM0 + SVM + VPX base images |
| **SDX SVM only** | `build-svm-<ver>.tgz` | SVM-only upgrade image |
| **ADC FreeBSD (MPX/VPX)** | `build-<ver>_nc_64.tgz` | NetScaler ADC FreeBSD 64-bit image |
| **ADC Linux (BLX/VPX-Linux)** | `build-<ver>_lx_64.tgz` | NetScaler ADC Linux 64-bit image |
| **VPX XVA (FreeBSD)** | `NSVPX-XEN-<ver>_nc_64.xva.gz` | XenServer-importable VPX appliance |
| **VPX XVA (Linux)** | `NSVPX-XEN-<ver>_lx_64.xva.gz` | XenServer-importable VPX Linux appliance |
| **MAS / ADM** | `build-mas-<ver>.tgz` | NetScaler Console (MAS/ADM) image |
| **MAS Agent** | `build-masagent-<ver>.tgz` | MAS agent image |

### Debug, Tools, and Packages

| Target | Filename Pattern | Description |
|--------|------------------|-------------|
| **Debug Binaries (ADC)** | `dbgbins-<ver>_nc.tgz` | Unstripped ADC binaries with symbols |
| **Debug Binaries (MAS)** | `mas-dbgbins-<ver>.tgz` | MAS debug binaries |
| **Debug Binaries (SVM)** | `svm-dbgbins-<ver>.tgz` | SVM debug binaries |
| **NS Tools** | `ns-tools-<ver>.nc.tgz` | CLI/diagnostic tools package |
| **Ansible Package** | `ns-<ver>-ansible-tools-package.tgz` | Ansible automation toolkit |
| **StyleBooks / NITRO** | `ns-<ver>-stylebooks-nitro-package.tgz` | StyleBooks and NITRO SDK |
| **Device Package** | `NSDevicePackage-<ver>.zip` | ADM device integration package |
| **Hybrid Mode Package** | `hybridModeDevicePackage-<ver>.zip` | Hybrid mode device package |
| **Checksums** | `checksum-<ver>.sha2` | SHA-256 checksums for all artifacts |

### Subdirectories

| Directory | Contents |
|-----------|----------|
| `agee_bin/` | AGEE binaries |
| `auditserver/` | Audit server components |
| `COE/` | COE artifacts |
| `cpx/` | CPX (container) images |
| `CRG/` | CRG artifacts |
| `cve_pkg/` | CVE security patches |
| `jenkins/` | Jenkins build metadata |
| `log/` | Build logs |
| `mastools-11.4/` | MAS tools (FreeBSD 11.4) |
| `mastools_linux/` | MAS tools (Linux) |
| `ns_telemetry_pkg/` | Telemetry package |
| `SDX-MFG/` | SDX manufacturing images |
| `SNMP/` | SNMP MIB files |
| `status/` | Build status files |
| `symbol/` | Debug symbol packages |
| `vpx-linux/` | VPX Linux platform images |
| `weblog/` | Web/audit log components |

## Finding a Build Label from Git

```bash
git --no-pager log origin/<branch> --oneline --grep="Label zion_" -n 5
```
