#!/usr/bin/env bash
# Guardrails (CLAUDE.md) checked mechanically:
#  - system Wine intact (pacman -Qkk wine);
#  - every package added since the initial backup is cited in ROLLBACK.md;
#  - every /etc file modified since the backup is cited in ROLLBACK.md;
#  - no world-writable file in the repository and no udev rule 0666/0777;
#  - no binary memory patching in src/ (WriteProcessMemory, detours...).
. "$(dirname "$0")/lib/common.sh"
bad=0
RB="$REPO/ROLLBACK.md"
BASE=$(ls "$REPO"/backups/pkglist-all-*.txt 2>/dev/null | head -1)

# Packages installed outside the project (D-016): exact names, attribution inferred, not proven.
KNOWN_USER_PKGS=" nmap openbsd-netcat unrar "
KNOWN_USER_ETC=" /etc/resolv.pre-tailscale-backup.conf "
if command -v pacman >/dev/null 2>&1 && pacman -Q wine >/dev/null 2>&1; then
  alt=$(pacman -Qkk wine 2>&1 | grep -oE '[0-9]+ altered files' | grep -oE '^[0-9]+')
  if [ "${alt:-x}" = 0 ]; then info "system wine: 0 modified files"; else info "system wine modified: $(pacman -Qkk wine 2>&1 | tail -1)"; bad=1; fi
  [ "$(command -v wine)" = /usr/bin/wine ] || { info "wine in PATH is not /usr/bin/wine: $(command -v wine)"; bad=1; }
else
  info "SKIPPED system wine check: no pacman or no system wine package on this machine"
fi

if [ -n "$BASE" ] && command -v pacman >/dev/null 2>&1; then
  while read -r p _; do
    case "$KNOWN_USER_PKGS" in *" $p "*) info "added package ignored (D-016, installed outside the project): $p"; continue;; esac
    # full name required in ROLLBACK.md or in uninstall.sh (which must undo it)
    if ! grep -qw -- "$p" "$RB" "$REPO/uninstall.sh" 2>/dev/null; then
      info "added package not recorded (neither ROLLBACK.md nor uninstall.sh): $p"; bad=1
    fi
  done < <(comm -13 <(cut -d' ' -f1 "$BASE" | sort) <(pacman -Q | cut -d' ' -f1 | sort) | sed 's/$/ x/')
  # removed packages
  while read -r p; do
    grep -qw -- "$p" "$RB" "$REPO/uninstall.sh" 2>/dev/null || { info "removed package not recorded: $p"; bad=1; }
  done < <(comm -23 <(cut -d' ' -f1 "$BASE" | sort) <(pacman -Q | cut -d' ' -f1 | sort))
else
  info "SKIPPED package baseline comparison: no backups/pkglist-all-*.txt (fresh clone) or no pacman"
fi

REF=$(ls "$REPO"/backups/etc-*.tar.gz 2>/dev/null | head -1)
if [ -z "$REF" ]; then
  info "SKIPPED /etc baseline comparison: no backups/etc-*.tar.gz (fresh clone)"
else
  # Volatile files modified by the system itself (not by the team).
  VOLATILE='^/etc/(ld\.so\.cache|resolv\.conf|adjtime|machine-id|mtab|\.pwd\.lock|pacman\.d/gnupg/.*|cups/.*|NetworkManager/system-connections/.*|udev/hwdb\.bin)$'
  while read -r f; do
    [[ "$f" =~ $VOLATILE ]] && continue
    case "$KNOWN_USER_ETC" in *" $f "*) info "modified /etc file ignored (D-016, outside the project): $f"; continue;; esac
    [ "$f" = __ECHEC_FIND__ ] && { info "/etc scan failed"; bad=1; continue; }
    if [ "$f" = /etc/fstab ]; then # D-014: only the /windows lines (ntfs3 -> ntfs-3g) may differ
      d=$(diff <(tar -xzOf "$REF" etc/fstab 2>/dev/null || tar -xzOf "$REF" ./etc/fstab) /etc/fstab | grep '^[<>]' | grep -v '^[<>][[:space:]]*$' | grep -v '[[:space:]]/windows[[:space:]]' | grep -vE '^< New[[:space:]]+Partition[[:space:]]+/data')
      [ -z "$d" ] && continue
      info "/etc/fstab: difference outside /windows (D-014): $d"; bad=1; continue
    fi
    grep -qF -- "$f" "$RB" || { info "modified /etc file not recorded: $f"; bad=1; }
  done < <(sudo -n find /etc -newer "$REF" \( -type f -o -type l \) 2>/dev/null || echo "__ECHEC_FIND__")
  if sudo -n true 2>/dev/null; then info "/etc compared with $(basename "$REF")"; else info "sudo -n unavailable: /etc NOT checked"; bad=1; fi
fi

ww=$(cd "$REPO" && git ls-files -z | xargs -0 -r stat -c '%a %n' 2>/dev/null | awk '$1 ~ /[2367]$/ {print $2}')
[ -z "$ww" ] || { info "world-writable repository files: $ww"; bad=1; }
for r in /etc/udev/rules.d/*parsec* "$REPO"/packaging/*.rules "$REPO"/src/*/*.rules; do
  [ -f "$r" ] || continue
  if grep -qE 'MODE="?0?6[67][67]|MODE="?0?777' "$r"; then info "udev rule too permissive: $r"; bad=1; fi
done
# Versioned project code only (no downloads or traces).
# D-013: VirtualProtect allowed only under src/capture/dxgi/ (DXGI/DXVK vtable hook, never Parsec).
hits=$(cd "$REPO" && git ls-files -z -- src runtime bin packaging ':!src/capture/dxgi' | xargs -0 -r grep -nE 'WriteProcessMemory|VirtualProtect(Ex)?\(|DetourAttach|MH_CreateHook|chmod +(-R +)?777|chmod +(-R +)?a\+rwx' 2>/dev/null)
[ -z "$hits" ] || { info "forbidden patterns (binary patch / chmod 777):"; echo "$hits" | sed 's/^/  #   /'; bad=1; }
# The prefix wine must not be a modified system wine: runtime/env.sh points into runtime/.
if [ -f "$REPO/runtime/env.sh" ]; then
  w=$(bash -c ". '$REPO/runtime/env.sh' >/dev/null 2>&1; echo \"\$WINE\"")
  case "$w" in "$REPO"/runtime/*|"") info "prefix wine: ${w:-system wine}";; *) info "WINE outside the repository: $w"; bad=1;; esac
fi
[ $bad = 0 ] || fail "at least one guardrail not respected (see above)"
pass "guardrails verified"
