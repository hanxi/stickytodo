#!/usr/bin/env bash
#
# package-mac-client.sh — Build the macOS client as a universal (arm64 + x86_64)
# binary, ad-hoc sign it, and package it into a distributable .dmg.
#
# Why ad-hoc signing?
#   Without a paid Apple Developer account we can't produce a "Developer ID"
#   signature or notarization. Ad-hoc (`codesign -s -`) is the best we can do
#   locally: users will see a Gatekeeper warning on first launch and have to
#   right-click → Open once. Documented in README.
#
# Inputs:
#   env VERSION         Version tag baked into the artifact filename (default: dev)
#   flag --skip-codesign  Skip the post-build ad-hoc signing step (for CI runners
#                         that already sign inside xcodebuild).
#   flag --skip-dmg     Produce only the .app bundle inside a zip (no dmg).
#
# Outputs (under dist/mac-client/):
#   stickytodo-${VERSION}-macos-universal.app         (universal .app bundle)
#   stickytodo-${VERSION}-macos-universal.dmg         (disk image, unless --skip-dmg)
#   stickytodo-${VERSION}-macos-universal.app.zip     (fallback when --skip-dmg)
#   SHA256SUMS                                        (one line per artifact)
#
# Strategy:
#   1. xcodebuild with ARCHS="arm64 x86_64" ONLY_ACTIVE_ARCH=NO → fat binary
#      directly from Xcode (more reliable than post-hoc lipo merging).
#   2. `lipo -archs` / `file` verification to prove the binary really is universal.
#   3. `codesign --force --deep --sign -` for ad-hoc signature.
#   4. Prefer `create-dmg` if installed (brew tap sindresorhus/create-dmg);
#      fall back to native `hdiutil` which is always present on macOS.
#
# Usage:
#   VERSION=v1.2.3 scripts/package-mac-client.sh
#   scripts/package-mac-client.sh --skip-dmg

set -euo pipefail

# ---------- locate repo root ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PROJECT_DIR="$REPO_ROOT/client/mac"
PROJECT_FILE="$PROJECT_DIR/stickytodo.xcodeproj"
SCHEME="stickytodo"
OUT_DIR="$REPO_ROOT/dist/mac-client"
DERIVED="/tmp/stickytodoRelease"

# ---------- flags ----------
SKIP_CODESIGN=0
SKIP_DMG=0
for arg in "$@"; do
  case "$arg" in
    --skip-codesign) SKIP_CODESIGN=1 ;;
    --skip-dmg)      SKIP_DMG=1 ;;
    -h|--help)
      sed -n '2,34p' "$0"
      exit 0
      ;;
    *)
      echo "package-mac-client: unknown flag: $arg" >&2
      exit 2
      ;;
  esac
done

VERSION="${VERSION:-dev}"
ARTIFACT_BASE="stickytodo-${VERSION}-macos-universal"
APP_BUNDLE_NAME="${ARTIFACT_BASE}.app"

log() { printf '[package-mac-client] %s\n' "$*"; }

# ---------- preflight ----------
if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "package-mac-client: this script only runs on macOS (need xcodebuild)" >&2
  exit 1
fi
if ! command -v xcodebuild >/dev/null 2>&1; then
  echo "package-mac-client: xcodebuild missing — install Xcode (not just CLT)" >&2
  exit 1
fi
if [[ ! -d "$PROJECT_FILE" ]]; then
  echo "package-mac-client: $PROJECT_FILE not found" >&2
  exit 1
fi

log "VERSION=${VERSION}"
log "project: ${PROJECT_FILE}"
log "scheme : ${SCHEME}"
log "output : ${OUT_DIR}"

# ---------- build universal Release ----------
rm -rf "$DERIVED"
mkdir -p "$OUT_DIR"

LOG_FILE="$(mktemp /tmp/stickytodo-mac-release.XXXXXX)"
# shellcheck disable=SC2064
trap "rm -f '$LOG_FILE'" EXIT

log "xcodebuild clean build (Release, universal arm64+x86_64, ad-hoc sign)"
set +e
xcodebuild \
  -project "$PROJECT_FILE" \
  -scheme "$SCHEME" \
  -configuration Release \
  -destination 'platform=macOS' \
  -derivedDataPath "$DERIVED" \
  ARCHS="arm64 x86_64" \
  ONLY_ACTIVE_ARCH=NO \
  CODE_SIGN_IDENTITY="-" \
  CODE_SIGNING_REQUIRED=NO \
  CODE_SIGNING_ALLOWED=NO \
  clean build \
  > "$LOG_FILE" 2>&1
BUILD_EXIT=$?
set -e

if [[ $BUILD_EXIT -ne 0 ]]; then
  echo "[package-mac-client] xcodebuild failed (exit=$BUILD_EXIT), last 60 lines:" >&2
  tail -n 60 "$LOG_FILE" >&2
  exit $BUILD_EXIT
fi
if ! grep -q '\*\* BUILD SUCCEEDED \*\*' "$LOG_FILE"; then
  echo "[package-mac-client] BUILD SUCCEEDED marker missing, failing. Last 60 lines:" >&2
  tail -n 60 "$LOG_FILE" >&2
  exit 1
fi

BUILT_APP="$DERIVED/Build/Products/Release/stickytodo.app"
if [[ ! -d "$BUILT_APP" ]]; then
  echo "[package-mac-client] BUILD SUCCEEDED but no .app at $BUILT_APP" >&2
  exit 1
fi

# Copy to versioned name in OUT_DIR.
rm -rf "$OUT_DIR/$APP_BUNDLE_NAME"
cp -R "$BUILT_APP" "$OUT_DIR/$APP_BUNDLE_NAME"
APP_PATH="$OUT_DIR/$APP_BUNDLE_NAME"
log "app bundle: $APP_PATH"

# ---------- verify universal ----------
INNER_BINARY="$APP_PATH/Contents/MacOS/stickytodo"
if [[ ! -f "$INNER_BINARY" ]]; then
  echo "[package-mac-client] inner binary missing at $INNER_BINARY" >&2
  exit 1
fi

ARCHS_FOUND="$(lipo -archs "$INNER_BINARY" 2>&1)"
log "inner binary archs: $ARCHS_FOUND"
case "$ARCHS_FOUND" in
  *arm64*\ *x86_64*|*x86_64*\ *arm64*)
    log "universal binary verified"
    ;;
  *)
    echo "[package-mac-client] expected universal (arm64 + x86_64), got: $ARCHS_FOUND" >&2
    exit 1
    ;;
esac

# ---------- ad-hoc sign ----------
if [[ "$SKIP_CODESIGN" -eq 1 ]]; then
  log "skip-codesign: using signature from xcodebuild as-is"
else
  log "ad-hoc signing (codesign -s -)"
  # --deep signs every nested binary/framework.
  # --options runtime enables Hardened Runtime; required by notarization but
  #   doesn't hurt ad-hoc signed builds either.
  # --force overwrites existing signatures (xcodebuild already put one on).
  codesign --force --deep --options runtime --sign - "$APP_PATH"

  # Verify signature integrity.
  codesign --verify --deep --strict --verbose=2 "$APP_PATH" 2>&1 | tail -5
  log "codesign ok"
fi

# ---------- package dmg ----------
ARTIFACTS=("$APP_BUNDLE_NAME")
cd "$OUT_DIR"

if [[ "$SKIP_DMG" -eq 1 ]]; then
  log "skip-dmg: creating a zip archive of the .app bundle instead"
  ZIP_NAME="${ARTIFACT_BASE}.app.zip"
  rm -f "$ZIP_NAME"
  # ditto preserves macOS metadata (xattrs, resource forks) better than zip.
  ditto -c -k --sequesterRsrc --keepParent "$APP_BUNDLE_NAME" "$ZIP_NAME"
  log "zip: $ZIP_NAME"
  ARTIFACTS+=("$ZIP_NAME")
else
  DMG_NAME="${ARTIFACT_BASE}.dmg"
  rm -f "$DMG_NAME"

  # Prefer create-dmg (prettier output with background + Applications symlink);
  # fall back to hdiutil if unavailable or failing. Use a flag var — NOT a
  # literal "create-dmg=missing" assignment, because `-` is not allowed in
  # bash variable names and the shell would try to execute it as a command.
  dmg_produced=0
  if command -v create-dmg >/dev/null 2>&1; then
    log "packaging with create-dmg"
    if ( set -x; create-dmg "$APP_BUNDLE_NAME" "$OUT_DIR" ); then
      # create-dmg writes a file like "stickytodo X.Y.Z.dmg" next to the .app;
      # rename it to our canonical artifact name.
      produced="$(find "$OUT_DIR" -maxdepth 1 -name 'stickytodo *.dmg' -print -quit)"
      if [[ -n "${produced:-}" && -f "$produced" ]]; then
        mv "$produced" "$DMG_NAME"
        dmg_produced=1
      fi
    else
      echo "[package-mac-client] create-dmg failed — falling back to hdiutil" >&2
    fi
  fi

  if [[ "$dmg_produced" -eq 0 ]]; then
    log "packaging with hdiutil"
    STAGE_DIR="$(mktemp -d /tmp/stickytodo-dmg-stage.XXXXXX)"
    # shellcheck disable=SC2064
    trap "rm -rf '$LOG_FILE' '$STAGE_DIR'" EXIT

    cp -R "$APP_BUNDLE_NAME" "$STAGE_DIR/"
    # Drag-to-install convenience: a symlink to /Applications.
    ln -s /Applications "$STAGE_DIR/Applications"

    # Retry hdiutil on "Resource busy" — macOS sometimes holds locks on newly
    # created directories until fseventsd settles (empirically <1s).
    #
    # Unique volume name per run to avoid "hdiutil: create failed - Resource
    # busy" when multiple jobs share the same builder host and leave a
    # lingering /Volumes/stickytodo-* mount. BASHPID is the current shell's
    # PID — guaranteed-set in bash 4+, and immune to the $-escaping gotchas
    # some editors/templaters have when double-dollar appears in a string.
    VOLNAME="stickytodo ${VERSION} ($$)"

    hdiutil_try() {
      # `-ov` already overwrites existing output, but some macOS versions
      # still complain if the target file is held open by fseventsd. Remove
      # it explicitly so each retry starts from a clean slate.
      rm -f "$DMG_NAME"
      hdiutil create \
        -volname "$VOLNAME" \
        -srcfolder "$STAGE_DIR" \
        -ov \
        -format UDZO \
        "$DMG_NAME" 2>&1
    }

    attempt=1
    while : ; do
      if out="$(hdiutil_try)"; then
        break
      fi
      if [[ "$attempt" -ge 3 ]]; then
        echo "[package-mac-client] hdiutil failed after 3 attempts:" >&2
        echo "$out" >&2
        exit 1
      fi
      echo "[package-mac-client] hdiutil attempt $attempt failed, retrying in 2s..." >&2
      echo "$out" | head -5 >&2
      # Also try detaching any stray volumes from previous attempts — the
      # "Resource busy" case is usually caused by a lingering mount.
      for vol in /Volumes/stickytodo*; do
        [[ -d "$vol" ]] && hdiutil detach -force "$vol" 2>/dev/null || true
      done
      attempt=$((attempt + 1))
      sleep 2
    done

    rm -rf "$STAGE_DIR"
    # Restore the simpler trap (only LOG_FILE needs cleanup from here on).
    # shellcheck disable=SC2064
    trap "rm -f '$LOG_FILE'" EXIT
    dmg_produced=1
  fi

  if [[ ! -f "$DMG_NAME" ]]; then
    echo "[package-mac-client] failed to produce $DMG_NAME" >&2
    exit 1
  fi

  DMG_SIZE="$(du -h "$DMG_NAME" | awk '{print $1}')"
  log "dmg: $DMG_NAME (${DMG_SIZE})"
  ARTIFACTS+=("$DMG_NAME")
fi

# ---------- checksums ----------
log "generating SHA256SUMS"
# Only checksum file artifacts (not .app bundles — they're directories).
FILE_ARTIFACTS=()
for a in "${ARTIFACTS[@]}"; do
  if [[ -f "$OUT_DIR/$a" ]]; then
    FILE_ARTIFACTS+=("$a")
  fi
done
if [[ "${#FILE_ARTIFACTS[@]}" -gt 0 ]]; then
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${FILE_ARTIFACTS[@]}" > SHA256SUMS
  else
    shasum -a 256 "${FILE_ARTIFACTS[@]}" > SHA256SUMS
  fi
  log "SHA256SUMS:"
  sed 's/^/  /' SHA256SUMS
else
  log "no file artifacts to checksum (only .app bundle produced)"
fi

log "done."
