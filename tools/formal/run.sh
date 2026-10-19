#!/bin/sh
# Runs the Driftwire protocol 2 models (all at once, one core each) and checks
# each one's RESULT lines against the expected ones (tools/formal/README.md).
# PROVERIF names the binary; `run.sh rejoin handshake_nonce` runs only those.
set -u
cd "$(dirname "$0")"
PROVERIF=${PROVERIF:-proverif}
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

expected() {
  case $1 in
    handshake_code)          echo "true true true" ;;
    handshake_nonce)         echo "true" ;;
    code_order)              echo "true false" ;;     # sanity: the attack without the commitment
    handshake_x25519_later)  echo "true true" ;;
    handshake_mlkem_later)   echo "true true" ;;
    handshake_both_later)    echo "false false" ;;    # sanity: both broken
    rejoin)                  echo "true true true true true true true false" ;;
    *) echo "unknown" ;;
  esac
}

models=${*:-"handshake_code handshake_nonce code_order handshake_x25519_later handshake_mlkem_later handshake_both_later rejoin"}
for m in $models; do
  ( start=$(date +%s)
    "$PROVERIF" -lib driftwire "$m.pv" > "$out/$m.log" 2>&1
    echo $(( $(date +%s) - start )) > "$out/$m.secs" ) &
done
wait

fail=0
for m in $models; do
  got=$(sed -nE 's/^RESULT .* is (true|false|cannot be proved).*/\1/p' "$out/$m.log" | tr '\n' ' ' | sed 's/ $//')
  want=$(expected "$m")
  if [ "$got" = "$want" ]; then
    echo "ok    $m ($(cat "$out/$m.secs")s)"
  else
    echo "FAIL  $m ($(cat "$out/$m.secs")s): expected [$want] got [$got]"
    grep -m3 -i error "$out/$m.log"
    fail=1
  fi
done
exit $fail
