#!/usr/bin/env bash
#
# package-win-client.sh — Build the Windows native client (Win32 + C++/WinRT
# + Direct2D) and, optionally, wrap it in an Inno Setup installer.
#
# Why bash on Windows?
#   GitHub Actions' windows-2022 runner ships Git Bash out of the box, and
#   so does any developer who installed Git for Windows. Keeping the build
#   orchestration in bash (instead of PowerShell) lets this script read
#   almost identically to package-mac-client.sh / package-server.sh and
#   lets maintainers do a line-by-line comparison when tweaking flags.
#
# Inputs:
#   env VERSION         Baked into the artifact filename (default: dev).
#   env VCPKG_ROOT      Absolute path to a vcpkg checkout. Required —
#                       CMakePresets.json references it via $env{VCPKG_ROOT}.
#   env ISCC            Absolute path to Inno Setup's `iscc.exe`. If unset,
#                       we try a handful of common locations; if still not
#                       found the installer step is skipped (not fatal —
#                       CI provides iscc; local devs may only want the .exe).
#   flag --skip-installer  Skip the Inno Setup step even if iscc is present.
#   flag --skip-tests   Skip the gtest run (tests already run in a separate
#                       job in CI; devs rarely need them inline here).
#
# Outputs (under dist/win-client/):
#   stickytodo-${VERSION}-windows-x64\            Plain unzipped exe folder
#       stickytodo.exe                             Clean brand name — same
#                                                  discipline as macOS .app
#                                                  (no version embedded).
#       README.md                                  (copied from repo root)
#       LICENSE.txt                                (copied from repo root)
#   stickytodo-${VERSION}-windows-x64.zip          Portable archive
#   stickytodo-setup-${VERSION}.exe                Inno Setup installer
#                                                  (skipped if iscc missing)
#   SHA256SUMS                                     One line per artifact
#
# Strategy:
#   1. Configure + build with the existing `release` CMake preset so we
#      reuse the same vcpkg toolchain that developers use locally.
#   2. If tests are enabled, reconfigure with BUILD_TESTS=ON into a separate
#      `test` build dir and run ctest — we never mix Release and test builds
#      because tests link different TUs (just the codec / models, not the
#      full app) and we don't want that contamination in the shipping exe.
#   3. Stage the exe + README + LICENSE into a clean staging dir and zip it.
#   4. Invoke iscc against installer/setup.iss with /D overrides so the
#      .iss file doesn't need to know the actual paths.
#   5. Emit SHA256SUMS using either `sha256sum` (Git Bash has it) or
#      `powershell Get-FileHash` as fallback.
#
# Usage:
#   VCPKG_ROOT=C:/vcpkg VERSION=v1.2.3 scripts/package-win-client.sh
#   scripts/package-win-client.sh --skip-installer --skip-tests

set -euo pipefail

# ---------- locate repo root ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

CLIENT_DIR="$REPO_ROOT/client/win"
INSTALLER_DIR="$REPO_ROOT/installer"
OUT_DIR="$REPO_ROOT/dist/win-client"

# ---------- flags ----------
SKIP_INSTALLER=0
SKIP_TESTS=0
for arg in "$@"; do
  case "$arg" in
    --skip-installer) SKIP_INSTALLER=1 ;;
    --skip-tests)     SKIP_TESTS=1 ;;
    -h|--help)
      sed -n '2,50p' "$0"
      exit 0
      ;;
    *)
      echo "package-win-client: unknown flag: $arg" >&2
      exit 2
      ;;
  esac
done

VERSION="${VERSION:-dev}"
ARTIFACT_BASE="stickytodo-${VERSION}-windows-x64"

echo "==> package-win-client"
echo "    repo root:     $REPO_ROOT"
echo "    client dir:    $CLIENT_DIR"
echo "    version:       $VERSION"
echo "    output dir:    $OUT_DIR"

# ---------- sanity: vcpkg + CMake presets ----------
if [[ -z "${VCPKG_ROOT:-}" ]]; then
  echo "package-win-client: VCPKG_ROOT must be set (CMakePresets.json references \$env{VCPKG_ROOT})." >&2
  echo "                    Install vcpkg (https://learn.microsoft.com/vcpkg/get_started/) and export VCPKG_ROOT." >&2
  exit 3
fi
if [[ ! -f "$CLIENT_DIR/CMakePresets.json" ]]; then
  echo "package-win-client: missing $CLIENT_DIR/CMakePresets.json" >&2
  exit 3
fi

# ---------- discover iscc (Inno Setup compiler) ----------
# Only matters when the user actually wants the installer. We do the
# discovery up front so "installer missing" feedback is immediate instead
# of after a full Release build.
ISCC_BIN=""
if [[ $SKIP_INSTALLER -eq 0 ]]; then
  if [[ -n "${ISCC:-}" && -x "$ISCC" ]]; then
    ISCC_BIN="$ISCC"
  else
    # Common install locations for Inno Setup 6 on a fresh windows-2022 runner
    # (inno-setup is pre-installed via Chocolatey at this path) and on most
    # developer machines.
    for candidate in \
      "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
      "/c/Program Files/Inno Setup 6/ISCC.exe" \
      "C:/Program Files (x86)/Inno Setup 6/ISCC.exe" \
      "C:/Program Files/Inno Setup 6/ISCC.exe"
    do
      if [[ -f "$candidate" ]]; then
        ISCC_BIN="$candidate"
        break
      fi
    done
  fi

  if [[ -z "$ISCC_BIN" ]]; then
    echo "package-win-client: iscc.exe not found — installer step will be skipped."
    echo "                    Install Inno Setup 6 or set ISCC=<path>, or pass --skip-installer to silence this warning."
    SKIP_INSTALLER=1
  else
    echo "    iscc:          $ISCC_BIN"
  fi
fi

# ---------- step 1: configure + build Release ----------
echo "==> [1/5] configure (release preset)"
cd "$CLIENT_DIR"
cmake --preset release

echo "==> [2/5] build (release preset)"
cmake --build --preset release --config Release

EXE_PATH="$CLIENT_DIR/build/release/stickytodo.exe"
if [[ ! -f "$EXE_PATH" ]]; then
  # Ninja single-config layout puts the exe directly in the build dir.
  # MSBuild multi-config layout would nest it under Release/. Cover both.
  if [[ -f "$CLIENT_DIR/build/release/Release/stickytodo.exe" ]]; then
    EXE_PATH="$CLIENT_DIR/build/release/Release/stickytodo.exe"
  else
    echo "package-win-client: could not locate stickytodo.exe after build" >&2
    echo "                    searched: $CLIENT_DIR/build/release/stickytodo.exe" >&2
    echo "                    searched: $CLIENT_DIR/build/release/Release/stickytodo.exe" >&2
    exit 4
  fi
fi
echo "    exe:           $EXE_PATH"

# ---------- step 2: unit tests (optional) ----------
if [[ $SKIP_TESTS -eq 0 ]]; then
  echo "==> [3/5] configure + run unit tests (debug preset with BUILD_TESTS=ON)"
  # The `debug` preset already has BUILD_TESTS=ON baked in, which also
  # activates the `tests` vcpkg feature via the CMakeLists wiring. We use
  # it rather than passing -DBUILD_TESTS=ON against the release preset so
  # the test binaries live in a separate build dir and never contaminate
  # the shipping Release output we staged above.
  cmake --preset debug
  cmake --build --preset debug --config Debug
  ( cd "$CLIENT_DIR/build/debug" && ctest --output-on-failure -C Debug )
else
  echo "==> [3/5] unit tests skipped (--skip-tests)"
fi

# ---------- step 3: stage artifacts ----------
echo "==> [4/5] stage artifacts into $OUT_DIR"
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR/$ARTIFACT_BASE"

# Copy the exe with a clean brand name. Same rule as the macOS bundle —
# the on-disk name users see after unzipping MUST be the clean brand name
# (stickytodo.exe), not the versioned artifact name.
cp "$EXE_PATH" "$OUT_DIR/$ARTIFACT_BASE/stickytodo.exe"

# README and LICENSE are optional — they're nice-to-have but not required
# for the portable zip to be functional.
[[ -f "$REPO_ROOT/README.md" ]] && cp "$REPO_ROOT/README.md" "$OUT_DIR/$ARTIFACT_BASE/README.md"
[[ -f "$REPO_ROOT/LICENSE" ]]  && cp "$REPO_ROOT/LICENSE"  "$OUT_DIR/$ARTIFACT_BASE/LICENSE.txt"

# Produce the portable zip. Prefer `7z` (smaller + deterministic-ish
# output) when available on the runner, otherwise fall back to PowerShell
# Compress-Archive. Last-resort fallback is `zip` which Git Bash ships.
ZIP_OUT="$OUT_DIR/$ARTIFACT_BASE.zip"
(
  cd "$OUT_DIR"
  if command -v 7z >/dev/null 2>&1; then
    7z a -tzip -mx=9 "$ARTIFACT_BASE.zip" "$ARTIFACT_BASE" >/dev/null
  elif command -v zip >/dev/null 2>&1; then
    zip -qr "$ARTIFACT_BASE.zip" "$ARTIFACT_BASE"
  elif command -v powershell >/dev/null 2>&1; then
    powershell -NoLogo -NoProfile -Command "Compress-Archive -Path '$ARTIFACT_BASE' -DestinationPath '$ARTIFACT_BASE.zip' -Force"
  else
    echo "package-win-client: no zip tool found (tried 7z, zip, powershell Compress-Archive)" >&2
    exit 5
  fi
)
echo "    portable:      $ZIP_OUT"

# ---------- step 4: Inno Setup installer (optional) ----------
INSTALLER_OUT=""
if [[ $SKIP_INSTALLER -eq 0 ]]; then
  echo "==> [5/5] build Inno Setup installer"
  if [[ ! -f "$INSTALLER_DIR/setup.iss" ]]; then
    echo "package-win-client: $INSTALLER_DIR/setup.iss not found" >&2
    exit 6
  fi

  # iscc wants Windows-style paths when launched from Git Bash (MSYS does
  # NOT auto-translate paths inside /D defines, only the first positional
  # script path). `cygpath -w` is the portable way to convert — it ships
  # with Git Bash via the MSYS runtime. On non-cygwin shells we fall back
  # to leaving the forward-slash path as-is, which iscc tolerates too.
  to_win() {
    if command -v cygpath >/dev/null 2>&1; then
      cygpath -w "$1"
    else
      echo "$1"
    fi
  }

  "$ISCC_BIN" \
    "/DAppVersion=$VERSION" \
    "/DArtifactDir=$(to_win "$OUT_DIR/$ARTIFACT_BASE")" \
    "/DRepoRoot=$(to_win "$REPO_ROOT")" \
    "/DOutputDir=$(to_win "$OUT_DIR")" \
    "/DOutputBaseName=stickytodo-setup-$VERSION" \
    "$(to_win "$INSTALLER_DIR/setup.iss")"

  INSTALLER_OUT="$OUT_DIR/stickytodo-setup-$VERSION.exe"
  if [[ ! -f "$INSTALLER_OUT" ]]; then
    echo "package-win-client: iscc reported success but $INSTALLER_OUT is missing" >&2
    exit 7
  fi
  echo "    installer:     $INSTALLER_OUT"
else
  echo "==> [5/5] Inno Setup installer skipped"
fi

# ---------- step 5: SHA256SUMS ----------
echo "==> emit SHA256SUMS"
(
  cd "$OUT_DIR"
  # Collect top-level artifact filenames (zip + installer if present);
  # we intentionally do NOT hash the unzipped staging dir contents — the
  # hashes users verify are the downloadable files.
  files=( "$ARTIFACT_BASE.zip" )
  [[ -f "stickytodo-setup-$VERSION.exe" ]] && files+=( "stickytodo-setup-$VERSION.exe" )

  rm -f SHA256SUMS
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${files[@]}" > SHA256SUMS
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "${files[@]}" > SHA256SUMS
  elif command -v powershell >/dev/null 2>&1; then
    # PowerShell fallback — format matches GNU sha256sum so users can
    # `sha256sum -c` downstream regardless of which tool produced the file.
    : > SHA256SUMS
    for f in "${files[@]}"; do
      h=$(powershell -NoLogo -NoProfile -Command "(Get-FileHash -Algorithm SHA256 '$f').Hash.ToLower()")
      printf '%s  %s\n' "$h" "$f" >> SHA256SUMS
    done
  else
    echo "package-win-client: no hashing tool found (tried sha256sum / shasum / powershell)" >&2
    exit 8
  fi
)

echo ""
echo "==> done. artifacts in $OUT_DIR:"
ls -la "$OUT_DIR" | sed 's/^/    /'
