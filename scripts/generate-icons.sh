#!/usr/bin/env bash
#
# generate-icons.sh — Rasterize the master SVG into every PNG/ICNS/favicon
# artifact consumed by the macOS client and the web client.
#
# Inputs:
#   assets/branding/stickytodo-icon.svg     (1024×1024 master; the colored brand mark)
#   assets/branding/stickytodo-menubar.svg  (18×18pt; pure-black TEMPLATE image for
#                                            the macOS menu bar — system auto-tints)
#
# Outputs:
#   client/mac/stickytodo/Assets.xcassets/AppIcon.appiconset/
#       icon_16x16.png, icon_16x16@2x.png, icon_32x32.png, icon_32x32@2x.png,
#       icon_128x128.png, icon_128x128@2x.png, icon_256x256.png,
#       icon_256x256@2x.png, icon_512x512.png, icon_512x512@2x.png,
#       Contents.json
#   client/mac/stickytodo/Assets.xcassets/MenuBarIcon.imageset/
#       menubar.png (18px), menubar@2x.png (36px), menubar@3x.png (54px),
#       Contents.json  (contains "template-rendering-intent":"template" so
#                       the system knows to auto-tint the artwork)
#   client/web/public/
#       favicon.svg (copy of master), favicon-32.png, favicon-16.png,
#       apple-touch-icon.png (180×180)
#   client/win/src/res/icons/stickytodo.ico        (multi-resolution ICO
#       embedded into stickytodo.exe via src/res/app.rc's `IDI_APPICON ICON
#       "icons\\stickytodo.ico"`. Contains 16/20/24/32/40/48/64/128/256 frames
#       so Explorer/Taskbar/Alt-Tab/Jump-List all pick a crisp size.)
#   assets/branding/out/AppIcon.icns              (standalone icns for DMG/about box)
#
# Rasterization strategy:
#   macOS ships no CLI SVG rasterizer that handles modern SVG well by default.
#   We try in order:
#     1. rsvg-convert (brew install librsvg)     — best SVG fidelity
#     2. qlmanage (Quick Look, macOS built-in)   — uses Core Graphics, handles
#                                                  gradients + filters correctly
#     3. magick / convert (ImageMagick)          — ONLY if its SVG delegate
#                                                  (rsvg-convert) is actually
#                                                  installed. The built-in MSVG
#                                                  renderer renders modern SVG
#                                                  (gradients/filters) as mostly
#                                                  black; we must not fall back
#                                                  to it silently.
#   At least one must succeed or the script exits non-zero.
#
# Usage (from repo root or anywhere):
#   scripts/generate-icons.sh                # full regen
#   scripts/generate-icons.sh --mac-only     # only the AppIcon.appiconset + icns
#   scripts/generate-icons.sh --web-only     # only the web favicons
#   scripts/generate-icons.sh --win-only     # only the Windows multi-res .ico
#
# Idempotent: rerunning overwrites outputs deterministically.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SVG_SRC="$REPO_ROOT/assets/branding/stickytodo-icon.svg"
MENUBAR_SVG_SRC="$REPO_ROOT/assets/branding/stickytodo-menubar.svg"
MAC_ICONSET_DIR="$REPO_ROOT/client/mac/stickytodo/Assets.xcassets/AppIcon.appiconset"
MAC_MENUBAR_DIR="$REPO_ROOT/client/mac/stickytodo/Assets.xcassets/MenuBarIcon.imageset"
WEB_PUBLIC_DIR="$REPO_ROOT/client/web/public"
WIN_ICO_PATH="$REPO_ROOT/client/win/src/res/icons/stickytodo.ico"
BRANDING_OUT_DIR="$REPO_ROOT/assets/branding/out"
TMP_DIR="$(mktemp -d -t stickytodo-icons.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

MAC_ONLY=0
WEB_ONLY=0
WIN_ONLY=0
for arg in "$@"; do
  case "$arg" in
    --mac-only) MAC_ONLY=1 ;;
    --web-only) WEB_ONLY=1 ;;
    --win-only) WIN_ONLY=1 ;;
    -h|--help)
      sed -n '2,35p' "$0"
      exit 0
      ;;
    *)
      echo "generate-icons: unknown flag: $arg" >&2
      exit 2
      ;;
  esac
done

# Mutual exclusivity: the three "--*-only" flags are documented as selecting
# *one* target. If caller passes more than one we'd silently do nothing (each
# section's guard requires all other "*_ONLY" vars to be 0), so fail fast with
# a clear message instead.
only_count=$(( MAC_ONLY + WEB_ONLY + WIN_ONLY ))
if (( only_count > 1 )); then
  echo "generate-icons: --mac-only / --web-only / --win-only are mutually exclusive" >&2
  exit 2
fi

log() { printf '[generate-icons] %s\n' "$*"; }

[[ -f "$SVG_SRC" ]] || { echo "generate-icons: missing $SVG_SRC" >&2; exit 1; }
[[ -f "$MENUBAR_SVG_SRC" ]] || { echo "generate-icons: missing $MENUBAR_SVG_SRC" >&2; exit 1; }

# ---------- pick a rasterizer ----------
# Check whether ImageMagick's SVG delegate resolves to an actually-installed
# renderer. If the delegate string references `rsvg-convert` (or `inkscape`)
# but that binary is missing, magick silently falls back to its internal MSVG
# parser, which renders gradients/filters as solid black — that's the bug we
# saw in the first run.
magick_has_working_svg_delegate() {
  command -v magick >/dev/null 2>&1 || return 1
  local delegate
  delegate="$(magick -list delegate 2>/dev/null | awk '/^[[:space:]]*svg =>/ {print; exit}')"
  # If we can't read delegates, be conservative and assume it's broken.
  [[ -n "$delegate" ]] || return 1
  case "$delegate" in
    *rsvg-convert*)  command -v rsvg-convert >/dev/null 2>&1 ;;
    *inkscape*)      command -v inkscape     >/dev/null 2>&1 ;;
    *) return 1 ;;
  esac
}

RASTER=""
if command -v rsvg-convert >/dev/null 2>&1; then
  RASTER="rsvg"
elif command -v qlmanage >/dev/null 2>&1; then
  RASTER="qlmanage"
elif magick_has_working_svg_delegate; then
  RASTER="magick"
elif command -v convert >/dev/null 2>&1 && magick_has_working_svg_delegate; then
  RASTER="convert"
else
  echo "generate-icons: no reliable SVG rasterizer found. Install one of:" >&2
  echo "  brew install librsvg        # recommended (rsvg-convert)" >&2
  echo "  (macOS has qlmanage built-in, but we couldn't locate it)" >&2
  exit 1
fi
log "rasterizer: $RASTER"

# render_svg_to_png <src-svg> <size> <out-path>
# Used for the full-color AppIcon master. `$RASTER` is selected once at startup
# with qlmanage preferred (because magick's built-in MSVG renders the gradient
# + filter AppIcon as solid black; see the header comment).
#
# Cache key includes the SVG path so rendering the AppIcon master and any
# future colored SVG don't clobber each other's cached qlmanage thumbnails.
render_svg_to_png() {
  local src="$1" size="$2" out="$3"
  case "$RASTER" in
    rsvg)
      rsvg-convert -w "$size" -h "$size" -f png "$src" -o "$out"
      ;;
    magick)
      # -background none keeps transparent edges; -density scales vector rendering
      magick -background none -density 600 "$src" -resize "${size}x${size}" "$out"
      ;;
    convert)
      convert -background none -density 600 "$src" -resize "${size}x${size}" "$out"
      ;;
    qlmanage)
      # qlmanage renders at a single "-s N" max dimension; to keep all sizes
      # sharp we render the master ONCE at 1024 (reused across every target
      # size) and then let sips resample down.
      local src_key ql_master
      src_key="$(basename "$src" .svg)"
      ql_master="$TMP_DIR/ql-${src_key}-1024.png"
      if [[ ! -s "$ql_master" ]]; then
        local ql_tmp="$TMP_DIR/ql_${src_key}_dir"
        mkdir -p "$ql_tmp"
        qlmanage -t -s 1024 -o "$ql_tmp" "$src" >/dev/null 2>&1 || true
        local raw
        raw="$(ls "$ql_tmp"/*.png 2>/dev/null | head -n 1 || true)"
        [[ -n "$raw" ]] || { echo "generate-icons: qlmanage failed to render $src" >&2; return 1; }
        cp "$raw" "$ql_master"
      fi
      # sips -z fits within H×W, preserving aspect ratio. Our masters are
      # square, so output is always ${size}×${size}.
      sips -s format png -z "$size" "$size" "$ql_master" --out "$out" >/dev/null
      ;;
  esac
  [[ -s "$out" ]] || { echo "generate-icons: empty output $out" >&2; return 1; }
}

# render_menubar_svg_to_png <src-svg> <size> <out-path>
# Separate path for menu bar TEMPLATE images because they have a very different
# fidelity requirement from the colored AppIcon:
#   - MUST preserve alpha (transparent background). macOS template rendering
#     keys off the alpha channel to paint the menu bar tint — if background
#     pixels are opaque white, the system "tint" floods the whole square and
#     you see nothing.
#   - The source SVG is pure-black paths (no gradient, no filter), so magick's
#     built-in MSVG handles it perfectly; the reason we avoid magick for the
#     colored master doesn't apply here.
#   - qlmanage CANNOT be used here: on macOS 14+ it flattens SVG transparency
#     onto an opaque white canvas (verified: mean alpha=255, mean RGB≈254),
#     which defeats template rendering entirely. We keep a warning + abort
#     path instead of silently producing a broken imageset.
render_menubar_svg_to_png() {
  local src="$1" size="$2" out="$3"
  if command -v rsvg-convert >/dev/null 2>&1; then
    rsvg-convert -w "$size" -h "$size" -f png "$src" -o "$out"
  elif command -v magick >/dev/null 2>&1; then
    magick -background none -density 600 "$src" -resize "${size}x${size}" "$out"
  elif command -v convert >/dev/null 2>&1; then
    convert -background none -density 600 "$src" -resize "${size}x${size}" "$out"
  else
    echo "generate-icons: menu bar template image needs rsvg-convert or ImageMagick" >&2
    echo "  brew install librsvg       # recommended" >&2
    echo "  brew install imagemagick   # alternative" >&2
    echo "  (qlmanage is NOT usable here — it flattens transparency)" >&2
    return 1
  fi
  [[ -s "$out" ]] || { echo "generate-icons: empty output $out" >&2; return 1; }
}

# build_windows_ico <out-ico>
# Pack a multi-resolution Windows .ico from the colored master SVG.
#
# Frame set (16/20/24/32/40/48/64/128/256) matches the sizes the Windows shell
# actually asks for across DPI scales + surfaces (Taskbar, Alt-Tab, Start
# Menu, Explorer tiles, File Properties). Shipping all of them lets Windows
# pick a pre-rendered frame instead of bilinear-resampling a single large one
# (which looks fuzzy at 16/20/24).
#
# The 256×256 frame is stored as PNG-compressed inside the ICO (Vista+ format
# extension). ImageMagick and icotool both emit it that way automatically;
# png2ico does not support 256, so we skip that size when falling back to it.
#
# Tool preference (all OS-agnostic — the Windows ico ships from any host):
#   1. magick / convert (ImageMagick)  — single-call multi-frame packer, most
#                                         portable; always emits PNG compression
#                                         for the 256 frame
#   2. icotool (icoutils)              — brew install icoutils; clean CLI
#   3. png2ico                         — last resort; caps at 128, so output
#                                         is a degraded ICO (warning printed)
build_windows_ico() {
  local out="$1"
  local work="$TMP_DIR/win-ico"
  mkdir -p "$work"

  # Sizes that Windows actually requests. Keep sorted ascending so packing
  # tools emit frames in a deterministic order.
  local -a WIN_SIZES=(16 20 24 32 40 48 64 128 256)

  log "  rendering Windows PNG frames: ${WIN_SIZES[*]}"
  local s
  for s in "${WIN_SIZES[@]}"; do
    render_svg_to_png "$SVG_SRC" "$s" "$work/icon_${s}.png"
  done

  mkdir -p "$(dirname "$out")"

  # ImageMagick: `magick icon_16.png icon_20.png ... icon_256.png out.ico`
  # packs every input as a separate frame; the 256 frame is auto-encoded as
  # embedded PNG (verify with `magick identify out.ico` — look for "PNG" in
  # the format column of the largest frame).
  if command -v magick >/dev/null 2>&1; then
    local -a frames=()
    for s in "${WIN_SIZES[@]}"; do frames+=("$work/icon_${s}.png"); done
    magick "${frames[@]}" "$out"
  elif command -v convert >/dev/null 2>&1; then
    local -a frames=()
    for s in "${WIN_SIZES[@]}"; do frames+=("$work/icon_${s}.png"); done
    convert "${frames[@]}" "$out"
  elif command -v icotool >/dev/null 2>&1; then
    # icotool auto-picks PNG compression for frames ≥ 256, per its manpage.
    local -a frames=()
    for s in "${WIN_SIZES[@]}"; do frames+=("$work/icon_${s}.png"); done
    icotool -c -o "$out" "${frames[@]}"
  elif command -v png2ico >/dev/null 2>&1; then
    # png2ico chokes on 256; drop it and warn so the caller knows the ICO is
    # missing the tile-sized frame.
    echo "generate-icons: png2ico does not support 256px frames; omitting it" >&2
    local -a frames=()
    for s in "${WIN_SIZES[@]}"; do
      [[ "$s" -le 128 ]] && frames+=("$work/icon_${s}.png")
    done
    png2ico "$out" "${frames[@]}"
  else
    echo "generate-icons: need one of: magick (ImageMagick), icotool (icoutils), or png2ico to build a Windows .ico" >&2
    echo "  brew install imagemagick    # recommended — single tool, handles 256 frame" >&2
    echo "  brew install icoutils       # alternative" >&2
    return 1
  fi

  [[ -s "$out" ]] || { echo "generate-icons: empty output $out" >&2; return 1; }
}

# ---------- Mac AppIcon.appiconset ----------
if [[ "$WEB_ONLY" -eq 0 ]]; then
  log "building macOS AppIcon.appiconset at $MAC_ICONSET_DIR"
  mkdir -p "$MAC_ICONSET_DIR"

  # (filename, pixel size) pairs — covers all macOS AppIcon slots
  declare -a MAC_PNGS=(
    "icon_16x16.png:16"
    "icon_16x16@2x.png:32"
    "icon_32x32.png:32"
    "icon_32x32@2x.png:64"
    "icon_128x128.png:128"
    "icon_128x128@2x.png:256"
    "icon_256x256.png:256"
    "icon_256x256@2x.png:512"
    "icon_512x512.png:512"
    "icon_512x512@2x.png:1024"
  )
  for pair in "${MAC_PNGS[@]}"; do
    fname="${pair%%:*}"
    size="${pair##*:}"
    render_svg_to_png "$SVG_SRC" "$size" "$MAC_ICONSET_DIR/$fname"
    log "  rendered $fname (${size}×${size})"
  done

  # Write Contents.json for the AppIcon slots. This is the exact layout Xcode
  # expects; do not reorder — asset-catalog tooling matches by (size, scale).
  cat > "$MAC_ICONSET_DIR/Contents.json" <<'JSON'
{
  "images" : [
    { "filename" : "icon_16x16.png",       "idiom" : "mac", "scale" : "1x", "size" : "16x16" },
    { "filename" : "icon_16x16@2x.png",    "idiom" : "mac", "scale" : "2x", "size" : "16x16" },
    { "filename" : "icon_32x32.png",       "idiom" : "mac", "scale" : "1x", "size" : "32x32" },
    { "filename" : "icon_32x32@2x.png",    "idiom" : "mac", "scale" : "2x", "size" : "32x32" },
    { "filename" : "icon_128x128.png",     "idiom" : "mac", "scale" : "1x", "size" : "128x128" },
    { "filename" : "icon_128x128@2x.png",  "idiom" : "mac", "scale" : "2x", "size" : "128x128" },
    { "filename" : "icon_256x256.png",     "idiom" : "mac", "scale" : "1x", "size" : "256x256" },
    { "filename" : "icon_256x256@2x.png",  "idiom" : "mac", "scale" : "2x", "size" : "256x256" },
    { "filename" : "icon_512x512.png",     "idiom" : "mac", "scale" : "1x", "size" : "512x512" },
    { "filename" : "icon_512x512@2x.png",  "idiom" : "mac", "scale" : "2x", "size" : "512x512" }
  ],
  "info" : { "author" : "xcode", "version" : 1 }
}
JSON

  # Also ensure the enclosing Assets.xcassets has a top-level Contents.json,
  # otherwise Xcode may warn about a loose icon set without a parent catalog.
  # NOTE: plain variable (no `local`) because we're at script scope here.
  asset_catalog_root="$(dirname "$MAC_ICONSET_DIR")"
  if [[ ! -f "$asset_catalog_root/Contents.json" ]]; then
    cat > "$asset_catalog_root/Contents.json" <<'JSON'
{
  "info" : { "author" : "xcode", "version" : 1 }
}
JSON
  fi

  # ---------- standalone AppIcon.icns (for DMG/about dialogs) ----------
  mkdir -p "$BRANDING_OUT_DIR"
  ICONSET_WORK="$TMP_DIR/AppIcon.iconset"
  mkdir -p "$ICONSET_WORK"
  # iconutil expects the classic .iconset layout (same filenames as above).
  cp "$MAC_ICONSET_DIR/"icon_*.png "$ICONSET_WORK/"
  iconutil -c icns "$ICONSET_WORK" -o "$BRANDING_OUT_DIR/AppIcon.icns"
  log "wrote $BRANDING_OUT_DIR/AppIcon.icns"

  # ---------- MenuBarIcon.imageset (TEMPLATE image for menu bar) ----------
  # macOS menu bar icons must be template images: black artwork + alpha only,
  # so the system can tint them for light/dark menu bars and pressed states.
  # The "template-rendering-intent":"template" property in Contents.json is
  # what actually flips the render mode; naming alone is not enough.
  log "building macOS MenuBarIcon.imageset at $MAC_MENUBAR_DIR"
  mkdir -p "$MAC_MENUBAR_DIR"

  # Menu bar optical size is 18×18pt → 18px @1x, 36px @2x, 54px @3x.
  # Uses render_menubar_svg_to_png (NOT render_svg_to_png) because template
  # images must preserve alpha; see that function's comment for why qlmanage
  # is explicitly rejected.
  render_menubar_svg_to_png "$MENUBAR_SVG_SRC" 18 "$MAC_MENUBAR_DIR/menubar.png"
  render_menubar_svg_to_png "$MENUBAR_SVG_SRC" 36 "$MAC_MENUBAR_DIR/menubar@2x.png"
  render_menubar_svg_to_png "$MENUBAR_SVG_SRC" 54 "$MAC_MENUBAR_DIR/menubar@3x.png"
  log "  rendered menubar.png / menubar@2x.png / menubar@3x.png"

  cat > "$MAC_MENUBAR_DIR/Contents.json" <<'JSON'
{
  "images" : [
    { "filename" : "menubar.png",    "idiom" : "mac", "scale" : "1x" },
    { "filename" : "menubar@2x.png", "idiom" : "mac", "scale" : "2x" },
    { "filename" : "menubar@3x.png", "idiom" : "mac", "scale" : "3x" }
  ],
  "info" : { "author" : "xcode", "version" : 1 },
  "properties" : { "template-rendering-intent" : "template" }
}
JSON
fi

# ---------- Windows multi-resolution .ico ----------
# This is the icon embedded into stickytodo.exe via src/res/app.rc's
# `IDI_APPICON ICON "icons\\stickytodo.ico"`. Windows picks a frame out of it
# per-DPI/per-surface, so we ship every size the shell might request rather
# than letting it resample a single large frame (which looks blurry at small
# sizes in the taskbar / Alt-Tab).
#
# AGENTS.md previously documented this file as "hand-checked-in, regenerate
# manually"; now that we have a reliable multi-frame packer path, keep that
# note in sync when editing this section.
if [[ "$MAC_ONLY" -eq 0 && "$WEB_ONLY" -eq 0 ]]; then
  log "building Windows .ico at $WIN_ICO_PATH"
  build_windows_ico "$WIN_ICO_PATH"
  log "  wrote $WIN_ICO_PATH"
fi

# ---------- Web favicons ----------
if [[ "$MAC_ONLY" -eq 0 && "$WIN_ONLY" -eq 0 ]]; then
  log "building web favicons at $WEB_PUBLIC_DIR"
  mkdir -p "$WEB_PUBLIC_DIR"

  # Copy SVG master directly — browsers support SVG favicons and it stays sharp
  # at any DPR; the PNG fallbacks below are for older browsers and iOS.
  cp "$SVG_SRC" "$WEB_PUBLIC_DIR/favicon.svg"
  log "  copied favicon.svg"

  render_svg_to_png "$SVG_SRC" 16  "$WEB_PUBLIC_DIR/favicon-16.png"
  render_svg_to_png "$SVG_SRC" 32  "$WEB_PUBLIC_DIR/favicon-32.png"
  render_svg_to_png "$SVG_SRC" 180 "$WEB_PUBLIC_DIR/apple-touch-icon.png"
  log "  rendered favicon-16, favicon-32, apple-touch-icon"
fi

log "done."
