#!/bin/bash
#
# Benchmark: zlib vs zlib-ng vs zstd compression in git
#
set -euo pipefail

GIT_SRC="$(cd "$(dirname "$0")" && pwd)"
BENCH_DIR="/tmp/git-compression-bench"
NPROC=$(sysctl -n hw.ncpu)

rm -rf "$BENCH_DIR"
mkdir -p "$BENCH_DIR"

BUILD_ZLIB="$BENCH_DIR/build-zlib"
BUILD_ZLIBNG="$BENCH_DIR/build-zlib-ng"
BUILD_ZSTD="$BENCH_DIR/build-zstd"
BASELINE="$BENCH_DIR/baseline.git"

echo "========================================"
echo "  Git Compression Benchmark"
echo "  zlib vs zlib-ng vs zstd"
echo "========================================"
echo ""

# ---- Build ----
echo "=== Building git (3 variants) ==="

echo "  [1/3] stock zlib..."
make -C "$GIT_SRC" -j"$NPROC" clean >/dev/null 2>&1 || true
make -C "$GIT_SRC" -j"$NPROC" prefix="$BUILD_ZLIB" install \
    >/dev/null 2>&1
echo "    done"

echo "  [2/3] zlib-ng..."
make -C "$GIT_SRC" -j"$NPROC" clean >/dev/null 2>&1
make -C "$GIT_SRC" -j"$NPROC" ZLIB_NG=1 ZLIB_NG_PATH=/opt/homebrew \
    prefix="$BUILD_ZLIBNG" install >/dev/null 2>&1
echo "    done"

echo "  [3/3] zstd..."
make -C "$GIT_SRC" -j"$NPROC" clean >/dev/null 2>&1
make -C "$GIT_SRC" -j"$NPROC" USE_ZSTD=1 ZSTD_PATH=/opt/homebrew \
    prefix="$BUILD_ZSTD" install >/dev/null 2>&1
echo "    done"

GIT_ZLIB="$BUILD_ZLIB/bin/git"
GIT_ZLIBNG="$BUILD_ZLIBNG/bin/git"
GIT_ZSTD="$BUILD_ZSTD/bin/git"

# ---- Prepare baseline repo ----
echo ""
echo "=== Preparing baseline repo ==="
"$GIT_ZLIB" clone --bare --no-hardlinks "$GIT_SRC" "$BASELINE" 2>&1 | tail -1
OBJ_COUNT=$("$GIT_ZLIB" -C "$BASELINE" rev-list --objects --all | wc -l | tr -d ' ')
PACK_SIZE=$(du -sh "$BASELINE/objects/pack/"*.pack | awk '{print $1}')
echo "  Objects: $OBJ_COUNT  Pack: $PACK_SIZE"

# ---- Helper to create a variant repo ----
make_variant() {
    local git_bin="$1" dest="$2" algo="$3" level="$4"
    rm -rf "$dest"
    cp -a "$BASELINE" "$dest"
    if [ "$algo" = "zstd" ]; then
        "$git_bin" -C "$dest" config core.compressionAlgorithm zstd
    fi
    if [ -n "$level" ]; then
        "$git_bin" -C "$dest" config core.compression "$level"
    fi
}

# ============================================================
# BENCHMARK 1: Full repack (measures compression speed)
# ============================================================
echo ""
echo "========================================"
echo "  Benchmark 1: repack -adF"
echo "  ($OBJ_COUNT objects, full recompression)"
echo "========================================"
echo ""

# Prepare fresh copies for each run
R_ZLIB="$BENCH_DIR/repack-zlib"
R_ZLIBNG="$BENCH_DIR/repack-zlib-ng"
R_ZSTD3="$BENCH_DIR/repack-zstd3"
R_ZSTD5="$BENCH_DIR/repack-zstd5"
R_ZSTD9="$BENCH_DIR/repack-zstd9"

hyperfine \
    --warmup 1 \
    --min-runs 3 \
    --export-markdown "$BENCH_DIR/repack.md" \
    -n "zlib" \
      --prepare "rm -rf $R_ZLIB && cp -a $BASELINE $R_ZLIB" \
      "$GIT_ZLIB -C $R_ZLIB repack -adF" \
    -n "zlib-ng" \
      --prepare "rm -rf $R_ZLIBNG && cp -a $BASELINE $R_ZLIBNG" \
      "$GIT_ZLIBNG -C $R_ZLIBNG repack -adF" \
    -n "zstd=3" \
      --prepare "rm -rf $R_ZSTD3 && cp -a $BASELINE $R_ZSTD3 && $GIT_ZSTD -C $R_ZSTD3 config core.compressionAlgorithm zstd && $GIT_ZSTD -C $R_ZSTD3 config core.compression 3" \
      "$GIT_ZSTD -C $R_ZSTD3 repack -adF" \
    -n "zstd=5" \
      --prepare "rm -rf $R_ZSTD5 && cp -a $BASELINE $R_ZSTD5 && $GIT_ZSTD -C $R_ZSTD5 config core.compressionAlgorithm zstd && $GIT_ZSTD -C $R_ZSTD5 config core.compression 5" \
      "$GIT_ZSTD -C $R_ZSTD5 repack -adF" \
    -n "zstd=9" \
      --prepare "rm -rf $R_ZSTD9 && cp -a $BASELINE $R_ZSTD9 && $GIT_ZSTD -C $R_ZSTD9 config core.compressionAlgorithm zstd && $GIT_ZSTD -C $R_ZSTD9 config core.compression 9" \
      "$GIT_ZSTD -C $R_ZSTD9 repack -adF"

# Record pack sizes from last repack run
echo ""
echo "--- Pack sizes after repack ---"
for label_repo in "zlib:$R_ZLIB" "zlib-ng:$R_ZLIBNG" "zstd=3:$R_ZSTD3" "zstd=5:$R_ZSTD5" "zstd=9:$R_ZSTD9"; do
    label="${label_repo%%:*}"
    repo="${label_repo#*:}"
    size=$(ls -lh "$repo/objects/pack/"*.pack 2>/dev/null | awk '{print $5}')
    printf "  %-10s %s\n" "$label" "$size"
done

# ============================================================
# BENCHMARK 2-4: Read performance (on already-repacked repos)
# ============================================================

# Ensure each repo has been repacked with its target compression
for label_repo in "zlib:$R_ZLIB:$GIT_ZLIB" "zlib-ng:$R_ZLIBNG:$GIT_ZLIBNG" \
    "zstd=3:$R_ZSTD3:$GIT_ZSTD" "zstd=5:$R_ZSTD5:$GIT_ZSTD" "zstd=9:$R_ZSTD9:$GIT_ZSTD"; do
    label="${label_repo%%:*}"
    rest="${label_repo#*:}"
    repo="${rest%%:*}"
    git_bin="${rest#*:}"
    # Verify pack exists
    if ! ls "$repo/objects/pack/"*.pack >/dev/null 2>&1; then
        echo "  Re-repacking $label..."
        make_variant "$git_bin" "$repo" \
            "$(echo "$label" | grep -q zstd && echo zstd || echo zlib)" \
            "$(echo "$label" | grep -oE '[0-9]+' || true)"
        "$git_bin" -C "$repo" repack -adF
    fi
done

echo ""
echo "========================================"
echo "  Benchmark 2: rev-list --objects --all"
echo "  (decompression-heavy traversal)"
echo "========================================"
echo ""

hyperfine \
    --warmup 2 \
    --min-runs 5 \
    --export-markdown "$BENCH_DIR/revlist.md" \
    -n "zlib" "$GIT_ZLIB -C $R_ZLIB rev-list --objects --all >/dev/null" \
    -n "zlib-ng" "$GIT_ZLIBNG -C $R_ZLIBNG rev-list --objects --all >/dev/null" \
    -n "zstd=3" "$GIT_ZSTD -C $R_ZSTD3 rev-list --objects --all >/dev/null" \
    -n "zstd=5" "$GIT_ZSTD -C $R_ZSTD5 rev-list --objects --all >/dev/null" \
    -n "zstd=9" "$GIT_ZSTD -C $R_ZSTD9 rev-list --objects --all >/dev/null"

echo ""
echo "========================================"
echo "  Benchmark 3: log -Sfoo --raw"
echo "  (content search + decompression)"
echo "========================================"
echo ""

hyperfine \
    --warmup 1 \
    --min-runs 3 \
    --export-markdown "$BENCH_DIR/logsearch.md" \
    -n "zlib" "$GIT_ZLIB -C $R_ZLIB log -Sfoo --raw >/dev/null" \
    -n "zlib-ng" "$GIT_ZLIBNG -C $R_ZLIBNG log -Sfoo --raw >/dev/null" \
    -n "zstd=3" "$GIT_ZSTD -C $R_ZSTD3 log -Sfoo --raw >/dev/null" \
    -n "zstd=5" "$GIT_ZSTD -C $R_ZSTD5 log -Sfoo --raw >/dev/null" \
    -n "zstd=9" "$GIT_ZSTD -C $R_ZSTD9 log -Sfoo --raw >/dev/null"

echo ""
echo "========================================"
echo "  Benchmark 4: log -p --all (full diffs)"
echo "  (heaviest decompression workload)"
echo "========================================"
echo ""

hyperfine \
    --warmup 1 \
    --min-runs 3 \
    --export-markdown "$BENCH_DIR/logdiff.md" \
    -n "zlib" "$GIT_ZLIB -C $R_ZLIB log -p --all >/dev/null" \
    -n "zlib-ng" "$GIT_ZLIBNG -C $R_ZLIBNG log -p --all >/dev/null" \
    -n "zstd=3" "$GIT_ZSTD -C $R_ZSTD3 log -p --all >/dev/null" \
    -n "zstd=5" "$GIT_ZSTD -C $R_ZSTD5 log -p --all >/dev/null" \
    -n "zstd=9" "$GIT_ZSTD -C $R_ZSTD9 log -p --all >/dev/null"

echo ""
echo "========================================"
echo "  DONE - Results in $BENCH_DIR/"
echo "========================================"
