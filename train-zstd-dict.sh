#!/bin/bash
#
# Train a zstd dictionary from a git repository's objects.
# Usage: train-zstd-dict.sh [git-binary] [repo-path]
#
set -uo pipefail

GIT="${1:-git}"
REPO="${2:-.}"
DICT_SIZE="${DICT_SIZE:-112640}"   # 110KB default
SAMPLE_COUNT="${SAMPLE_COUNT:-10000}"
SAMPLE_DIR=$(mktemp -d)
OID_LIST=$(mktemp)

trap "rm -rf $SAMPLE_DIR $OID_LIST" EXIT

echo "Listing objects..."
"$GIT" -C "$REPO" cat-file --batch-all-objects \
    --batch-check='%(objectname) %(objecttype) %(objectsize)' \
    > "$OID_LIST"

TOTAL=$(wc -l < "$OID_LIST" | tr -d ' ')
echo "  $TOTAL total objects"

# Sample small objects (< 64KB) — these benefit most from dictionaries
echo "Sampling up to $SAMPLE_COUNT objects (< 64KB)..."
awk '$3 > 0 && $3 < 65536' "$OID_LIST" |
    awk 'BEGIN{srand(42)} {print rand(), $0}' |
    sort -n | head -n "$SAMPLE_COUNT" | cut -d' ' -f2- > "${OID_LIST}.sample"

NSAMPLED=$(wc -l < "${OID_LIST}.sample" | tr -d ' ')
echo "  Selected $NSAMPLED samples"

echo "Extracting object content..."
while IFS=' ' read -r oid otype osize; do
    "$GIT" -C "$REPO" cat-file "$otype" "$oid" > "$SAMPLE_DIR/$oid" 2>/dev/null || true
done < "${OID_LIST}.sample"

NFILES=$(ls "$SAMPLE_DIR" | wc -l | tr -d ' ')
echo "  Extracted $NFILES files"

if [ "$NFILES" -lt 100 ]; then
    echo "Error: too few samples for dictionary training"
    exit 1
fi

GITDIR=$("$GIT" -C "$REPO" rev-parse --git-dir)
case "$GITDIR" in
    /*) ;;
    *)  GITDIR="$REPO/$GITDIR" ;;
esac

echo "Training dictionary (target: $DICT_SIZE bytes)..."
zstd --train -r "$SAMPLE_DIR" -o "$GITDIR/zstd-dict" --maxdict="$DICT_SIZE" 2>&1

echo ""
ls -lh "$GITDIR/zstd-dict"
echo "Done."
