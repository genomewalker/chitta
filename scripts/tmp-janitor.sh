#!/usr/bin/env bash
# Remove this user's stale replica/eval directories from node-local /tmp: older
# than CHITTA_TMP_MAX_AGE_H (default 24 h), at least 100 MB, and no process has
# its cwd inside. /tmp is 100 GB shared by every user of the login node and is
# never cleaned (2026-09-17: 65 GB of finished-stream replicas). Streams and gates
# call this; --dry-run only lists.
set -uo pipefail
max_age_h="${CHITTA_TMP_MAX_AGE_H:-24}"
dry=0; [[ "${1:-}" == --dry-run ]] && dry=1
freed=0
while IFS= read -r d; do
    size=$(du -sm "$d" 2>/dev/null | cut -f1); [[ ${size:-0} -ge 100 ]] || continue
    users=$(ls -l /proc/[0-9]*/cwd 2>/dev/null | grep -c " $d")
    [[ $users -eq 0 ]] || continue
    if (( dry )); then echo "would remove ${size} MB $d"; else rm -rf "$d" && freed=$((freed + size)); fi
done < <(find /tmp -maxdepth 1 -mindepth 1 -type d -user "$USER" -mmin "+$((max_age_h * 60))" \
         ! -name 'chitta-*.sock*' ! -name '*.startlock' ! -name 'claude-*' 2>/dev/null)
(( dry )) || echo "tmp-janitor: freed ${freed} MB on $(hostname -s); /tmp $(df -h /tmp | awk 'NR==2{print $5}') used"
