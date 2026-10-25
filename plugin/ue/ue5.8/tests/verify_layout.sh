#!/bin/bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
UE_ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
PROJECT_ROOT=$(cd "$UE_ROOT/../.." && pwd)
MODULE_DIR="$UE_ROOT/MagicSR/Source/MagicSR"
RESOLVED_PROJECT_ROOT=$(cd "$MODULE_DIR/../../../../../" && pwd)

required=(
  "$UE_ROOT/MagicSR/MagicSR.uplugin"
  "$UE_ROOT/MagicSR/Source/MagicSR/MagicSR.Build.cs"
  "$UE_ROOT/MagicSR/Source/MagicSR/Public/MagicSRBlueprintLibrary.h"
  "$UE_ROOT/MagicSR/Source/MagicSR/Private/MagicSRBlueprintLibrary.cpp"
)

for path in "${required[@]}"; do
  if [ ! -f "$path" ]; then
    echo "[verify] missing file: $path"
    exit 1
  fi
done

if [ "$RESOLVED_PROJECT_ROOT" != "$PROJECT_ROOT" ]; then
  echo "[verify] Build.cs project root resolution mismatch: expected $PROJECT_ROOT, got $RESOLVED_PROJECT_ROOT"
  exit 1
fi

if [ ! -f "$PROJECT_ROOT/interface/mc_interface.h" ]; then
  echo "[verify] missing mc_interface.h: $PROJECT_ROOT/interface/mc_interface.h"
  exit 1
fi

if [ ! -f "$PROJECT_ROOT/build/android/build/libmagic_sr.a" ]; then
  echo "[verify] missing Android static library: $PROJECT_ROOT/build/android/build/libmagic_sr.a"
  exit 1
fi

if ! grep -q '../../../../../' "$UE_ROOT/MagicSR/Source/MagicSR/MagicSR.Build.cs"; then
  echo "[verify] MagicSR.Build.cs does not resolve project root from the module directory"
  exit 1
fi

echo "[verify] UE plugin bridge layout looks complete."
