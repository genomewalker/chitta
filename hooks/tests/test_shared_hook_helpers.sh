#!/bin/bash
# Regression coverage for helpers shared by multiple lifecycle/tool hooks.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
source "$ROOT/hooks/lib.sh"

# Escaped contents must retain whitespace, Unicode and JSON control characters.
for value in '' 'plain' $'"quote" \\ slash\n\tcarriage\rα' $'tail\n\n'; do
    expected=$(printf '%s' "$value" | jq -Rs .)
    [[ "\"$(json_escape "$value")\"" == "$expected" ]]
done

# Directory existence resolves dash ambiguity; nonexistent components survive.
mkdir -p "$T/project-name/sub dir"
for path in "$T/project-name" "$T/project-name/sub dir" "$T/missing/child"; do
    [[ "$(decode_project_path "${path//\//-}")" == "$path" ]]
done
[[ "$(decode_project_path '')" == '' ]]

cat > "$T/chitta" <<'STUB'
#!/bin/bash
[[ "$1" == realm_detect ]] || exit 9
printf '%s\n' "$PWD"
exit "${REALM_EXIT:-0}"
STUB
chmod +x "$T/chitta"
export CHITTA_BIN="$T/chitta" MAX_WAIT=2
[[ "$(detect_project_realm "$T/project-name")" == "$T/project-name" ]]
[[ "$(detect_project_realm "$T/missing")" == "$PWD" ]]
[[ "$(detect_project_realm '')" == "$PWD" ]]
[[ "$(REALM_EXIT=1 detect_project_realm "$T")" == "$T"$'\nbrahman' ]]
printf 'shared hook helpers: passed\n'
