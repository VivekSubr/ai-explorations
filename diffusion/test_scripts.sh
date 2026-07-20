#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT"

PYTHON_BIN="${PYTHON_BIN:-.venv-win/Scripts/python.exe}"
OUT_DIR="${OUT_DIR:-test-output}"
MODEL="${MODEL:-imagepipeline/EpiCRealism-Natural-Sin}"
VARIANT="${VARIANT:-fp16}"
DTYPE="${DTYPE:-auto}"
TORCH_DEVICE="${TORCH_DEVICE:-cpu}"
PROMPT="${PROMPT:-sexy indian woman}"
IMG2IMG_PROMPT="${IMG2IMG_PROMPT:-even sexier}"
SIZE="${SIZE:-2048}"
EXAMPLE_STEPS="${EXAMPLE_STEPS:-20}"
IMG2IMG_STEPS="${IMG2IMG_STEPS:-25}"
CFG="${CFG:-5.0}"
SEED="${SEED:-12345}"
STRENGTH="${STRENGTH:-0.6}"
DIFF_THRESHOLD="${DIFF_THRESHOLD:-5.0}"

assert_output() {
  local path="$1"
  if [[ ! -s "$path" ]]; then
    echo "Expected non-empty output was not created: $path" >&2
    exit 1
  fi
}

assert_changed() {
  local before="$1"
  local after="$2"
  "$PYTHON_BIN" - "$before" "$after" "$DIFF_THRESHOLD" <<'PY'
import sys
from PIL import Image, ImageChops, ImageStat

before_path, after_path, threshold = sys.argv[1], sys.argv[2], float(sys.argv[3])
before = Image.open(before_path).convert("RGB")
after = Image.open(after_path).convert("RGB")
if before.size != after.size:
    after = after.resize(before.size)

diff = ImageChops.difference(before, after)
mean_abs = sum(ImageStat.Stat(diff).mean) / 3
print(f"Mean absolute pixel difference: {mean_abs:.2f}")
if mean_abs < threshold:
    raise SystemExit(
        f"img2img output is too similar to the input: {mean_abs:.2f} < {threshold:.2f}"
    )
PY
}

mkdir -p "$OUT_DIR"

base_output="output.png"
img2img_output="$OUT_DIR/tc1-img2img.png"

echo "TC1: chaining example.py output into img2img.py"
"$PYTHON_BIN" example.py \
  --prompt "$PROMPT" \
  --output "$base_output" \
  --model "$MODEL" \
  --variant "$VARIANT" \
  --dtype "$DTYPE" \
  --steps "$EXAMPLE_STEPS" \
  --cfg "$CFG" \
  --seed "$SEED" \
  --width "$SIZE" \
  --height "$SIZE" \
  --device "$TORCH_DEVICE" \
  --allow-cpu
assert_output "$base_output"

"$PYTHON_BIN" img2img.py \
  --input "$base_output" \
  --output "$img2img_output" \
  --prompt "$IMG2IMG_PROMPT" \
  --model "$MODEL" \
  --variant "$VARIANT" \
  --dtype "$DTYPE" \
  --steps "$IMG2IMG_STEPS" \
  --cfg "$CFG" \
  --seed "$SEED" \
  --size "$SIZE" \
  --strength "$STRENGTH" \
  --device "$TORCH_DEVICE" \
  --allow-cpu
assert_output "$img2img_output"
assert_changed "$base_output" "$img2img_output"

echo "TC1 PASS"
echo "All tests passed."
