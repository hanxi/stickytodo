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
#   env ARCH            Target architecture: "x64" (default) or "arm64".
#                       Picks the matching CMake preset (release / release-arm64)
#                       and the matching vcpkg triplet. On the x64 CI runner
#                       "arm64" is a cross-compile: binaries build fine but
#                       cannot execute, so ctest is skipped automatically for
#                       arm64 regardless of --skip-tests.
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
# Outputs (under dist/win-client/), with ${ARCH} resolving to x64 or arm64:
#   stickytodo-${VERSION}-windows-${ARCH}\        Plain unzipped exe folder
#       stickytodo.exe                             Clean brand name — same
#                                                  discipline as macOS .app
#                                                  (no version embedded).
#       README.md                                  (copied from repo root)
#       LICENSE.txt                                (copied from repo root)
#   stickytodo-${VERSION}-windows-${ARCH}.zip      Portable archive
#   stickytodo-setup-${VERSION}-${ARCH}.exe        Inno Setup installer
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

# ARCH gate: the only two shapes we know how to build. Guard explicitly so a
# typo like ARCH=amd64 fails loudly instead of quietly falling through to the
# x64 path and producing mis-labelled artifacts. "x64" is our canonical name
# (matches the install_prefix pattern used in artifact filenames and in
# every reference across AGENTS.md / README.md); Microsoft's toolchain
# naming (amd64, x86_64) is deliberately NOT accepted here.
ARCH="${ARCH:-x64}"
case "$ARCH" in
  x64|arm64) ;;
  *)
    echo "package-win-client: unsupported ARCH=\"$ARCH\" (expected: x64 | arm64)" >&2
    exit 2
    ;;
esac

# Pick the matching CMake preset. Presets live in client/win/CMakePresets.json
# and carry both CMAKE_BUILD_TYPE + VCPKG_TARGET_TRIPLET so we only need to
# hand cmake the preset name — triplet wiring is declarative, no env gymnastics.
if [[ "$ARCH" == "arm64" ]]; then
  RELEASE_PRESET="release-arm64"
  DEBUG_PRESET="debug-arm64"
else
  RELEASE_PRESET="release"
  DEBUG_PRESET="debug"
fi

ARTIFACT_BASE="stickytodo-${VERSION}-windows-${ARCH}"
INSTALLER_BASENAME="stickytodo-setup-${VERSION}-${ARCH}"

echo "==> package-win-client"
echo "    repo root:     $REPO_ROOT"
echo "    client dir:    $CLIENT_DIR"
echo "    version:       $VERSION"
echo "    arch:          $ARCH"
echo "    preset:        $RELEASE_PRESET"
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
echo "==> [1/5] configure ($RELEASE_PRESET preset)"
cd "$CLIENT_DIR"
cmake --preset "$RELEASE_PRESET"

echo "==> [2/5] build ($RELEASE_PRESET preset)"
# -j 1 forces a SERIAL build — intentionally slow, intentionally noisy. In a
# previous CI run the output showed "[2/20]..[6/20] Building..." with zero
# `FAILED:` lines and then "ninja: build stopped: subcommand failed", meaning
# the real cl.exe error text got interleaved with surviving jobs' stdout and
# swallowed by the GitHub Actions log streamer before it could flush. Serial
# build eliminates the interleave: whichever TU fails is the last thing on
# screen, so `error C2xxx:` lines are guaranteed to land in the log. The
# wall-clock cost is ~45 s on windows-2022 (20 TUs, each ~2 s of cl.exe
# time) — well worth it in exchange for debuggable failures. If / when the
# Win build is stable, drop -j 1 to let ninja parallelise again.
# --verbose echoes the full cl.exe command line for every TU, which is
# priceless when triaging "works locally, fails in CI" discrepancies caused
# by missing -D, missing /I, or MSVC vs vcpkg CRT model mismatches.
cmake --build --preset "$RELEASE_PRESET" --config Release -j 1 --verbose

EXE_PATH="$CLIENT_DIR/build/${RELEASE_PRESET}/stickytodo.exe"
if [[ ! -f "$EXE_PATH" ]]; then
  # Ninja single-config layout puts the exe directly in the build dir.
  # MSBuild multi-config layout would nest it under Release/. Cover both.
  if [[ -f "$CLIENT_DIR/build/${RELEASE_PRESET}/Release/stickytodo.exe" ]]; then
    EXE_PATH="$CLIENT_DIR/build/${RELEASE_PRESET}/Release/stickytodo.exe"
  else
    echo "package-win-client: could not locate stickytodo.exe after build" >&2
    echo "                    searched: $CLIENT_DIR/build/${RELEASE_PRESET}/stickytodo.exe" >&2
    echo "                    searched: $CLIENT_DIR/build/${RELEASE_PRESET}/Release/stickytodo.exe" >&2
    exit 4
  fi
fi
echo "    exe:           $EXE_PATH"

# ---------- step 2: unit tests (optional) ----------
#
# arm64-on-x64 cross-compile: the windows-2022 GitHub Actions runner is an
# x64 host, so even though arm64 binaries build cleanly (MSVC's cross tools
# handle the codegen), they cannot execute on the build machine. Running
# ctest against arm64 test binaries would hang on Windows' image loader
# rejecting the wrong architecture. Skip tests automatically in that case
# — we rely on the x64 matrix leg to exercise the test suite, since the
# codec / models code under test is architecture-independent C++ anyway.
if [[ "$ARCH" == "arm64" ]]; then
  echo "==> [3/5] unit tests skipped (arm64 cross-compile on x64 host)"
elif [[ $SKIP_TESTS -eq 0 ]]; then
  echo "==> [3/5] configure + run unit tests ($DEBUG_PRESET preset with BUILD_TESTS=ON)"
  # The `debug` preset already has BUILD_TESTS=ON baked in, which also
  # activates the `tests` vcpkg feature via the CMakeLists wiring. We use
  # it rather than passing -DBUILD_TESTS=ON against the release preset so
  # the test binaries live in a separate build dir and never contaminate
  # the shipping Release output we staged above.
  cmake --preset "$DEBUG_PRESET"
  cmake --build --preset "$DEBUG_PRESET" --config Debug
  ( cd "$CLIENT_DIR/build/${DEBUG_PRESET}" && ctest --output-on-failure -C Debug )
else
  echo "==> [3/5] unit tests skipped (--skip-tests)"
fi

# ---------- step 3: stage artifacts ----------
echo "==> [4/5] stage artifacts into $OUT_DIR"
# Do NOT rm -rf $OUT_DIR here — the matrix job runs x64 and arm64 in separate
# GitHub Actions invocations (each has its own fresh $OUT_DIR from
# `actions/checkout`), so clobbering is unnecessary; and on a developer
# machine, iterating between `ARCH=x64 ...` and `ARCH=arm64 ...` should
# accumulate both architectures' artifacts side by side rather than silently
# delete the other arch on every run. Clean only this arch's staging dir.
rm -rf "$OUT_DIR/$ARTIFACT_BASE"
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

  # CRITICAL: disable MSYS2's automatic argument-path conversion for this
  # one command.
  #
  # Background — on a previous CI run the iscc invocation failed with:
  #   "You may not specify more than one script filename."
  #
  # Root cause: Git Bash on windows-2022 runs under the MSYS2 runtime,
  # which by default rewrites argv entries that look like POSIX paths
  # before handing them to a native Win32 executable. When it sees our
  # /D defines carrying Windows-style values like
  #   /DArtifactDir=D:\a\stickytodo\stickytodo\dist\win-client\...
  # MSYS2's heuristic (wrongly) treats the leading `/D` as the root-level
  # `/D` directory, prepends the Git install prefix, and mangles the
  # entire argument into something like
  #   C:/Program Files/Git/DArtifactDir=D:/a/stickytodo/...
  # iscc then parses that blob as an .iss filename and, because we also
  # pass the real setup.iss as the last positional argument, it bails
  # out with the "more than one script filename" error above.
  #
  # MSYS2_ARG_CONV_EXCL='*' tells the MSYS runtime to skip the heuristic
  # entirely for this process, so the /D arguments reach iscc verbatim.
  # We scope the export to just the iscc invocation so the rest of the
  # script (which relies on POSIX path semantics) is unaffected.
  MSYS2_ARG_CONV_EXCL='*' MSYS_NO_PATHCONV=1 \
    "$ISCC_BIN" \
      "/DAppVersion=$VERSION" \
      "/DAppArch=$ARCH" \
      "/DArtifactDir=$(to_win "$OUT_DIR/$ARTIFACT_BASE")" \
      "/DRepoRoot=$(to_win "$REPO_ROOT")" \
      "/DOutputDir=$(to_win "$OUT_DIR")" \
      "/DOutputBaseName=$INSTALLER_BASENAME" \
      "$(to_win "$INSTALLER_DIR/setup.iss")"

  INSTALLER_OUT="$OUT_DIR/$INSTALLER_BASENAME.exe"
  if [[ ! -f "$INSTALLER_OUT" ]]; then
    echo "package-win-client: iscc reported success but $INSTALLER_OUT is missing" >&2
    exit 7
  fi
  echo "    installer:     $INSTALLER_OUT"
else
  echo "==> [5/5] Inno Setup installer skipped"
fi

# ---------- step 5: SHA256SUMS ----------
# Per-architecture sums file: name is suffixed with the arch so the x64 and
# arm64 matrix legs never race each other in the merged artifact directory,
# and so downstream `sha256sum -c SHA256SUMS-<arch>` can verify one arch's
# downloads in isolation. The publish-release job renames / concatenates
# these if the GitHub Release needs a single canonical SHA256SUMS file.
SUMS_FILE="SHA256SUMS-${ARCH}"
echo "==> emit $SUMS_FILE"
(
  cd "$OUT_DIR"
  # Collect top-level artifact filenames (zip + installer if present);
  # we intentionally do NOT hash the unzipped staging dir contents — the
  # hashes users verify are the downloadable files.
  files=( "$ARTIFACT_BASE.zip" )
  [[ -f "$INSTALLER_BASENAME.exe" ]] && files+=( "$INSTALLER_BASENAME.exe" )

  rm -f "$SUMS_FILE"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${files[@]}" > "$SUMS_FILE"
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "${files[@]}" > "$SUMS_FILE"
  elif command -v powershell >/dev/null 2>&1; then
    # PowerShell fallback — format matches GNU sha256sum so users can
    # `sha256sum -c` downstream regardless of which tool produced the file.
    : > "$SUMS_FILE"
    for f in "${files[@]}"; do
      h=$(powershell -NoLogo -NoProfile -Command "(Get-FileHash -Algorithm SHA256 '$f').Hash.ToLower()")
      printf '%s  %s\n' "$h" "$f" >> "$SUMS_FILE"
    done
  else
    echo "package-win-client: no hashing tool found (tried sha256sum / shasum / powershell)" >&2
    exit 8
  fi
)

echo ""
echo "==> done. artifacts in $OUT_DIR:"
ls -la "$OUT_DIR" | sed 's/^/    /'
