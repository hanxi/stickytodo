#!/usr/bin/env bash
#
# bump-version.sh — Bump the release version, create an annotated git tag and
# push it to the remote. Pushing a `v*` tag triggers .github/workflows/release-tag.yml,
# which runs the full release pipeline (7 server binaries + macOS DMG + Docker image).
#
# Usage (from repo root or anywhere inside the repo):
#   scripts/bump-version.sh patch                 # v1.2.3 -> v1.2.4
#   scripts/bump-version.sh minor                 # v1.2.3 -> v1.3.0
#   scripts/bump-version.sh major                 # v1.2.3 -> v2.0.0
#   scripts/bump-version.sh 1.4.0                 # explicit version (v prefix optional)
#   scripts/bump-version.sh v1.4.0
#   scripts/bump-version.sh patch --dry-run       # print what would happen, no side effects
#   scripts/bump-version.sh patch --no-push       # create local tag but don't push
#   scripts/bump-version.sh patch --remote origin # override push remote (default: origin)
#   scripts/bump-version.sh patch --message "..." # override tag annotation message
#   scripts/bump-version.sh patch --yes           # skip the final confirmation prompt
#
# Safety checks (all abort on failure):
#   1. Must be run inside a git work tree.
#   2. Working tree must be clean (no staged / unstaged / untracked changes).
#   3. Current branch must be up to date with its upstream (fast-forward check).
#   4. The computed tag must not already exist locally or on the remote.
#   5. Version string must match vMAJOR.MINOR.PATCH (pre-release suffix allowed,
#      e.g. v1.2.3-rc.1). Anything else is rejected.
#
# Exit 0 on success; non-zero on any failure.

set -euo pipefail

# ---------- locate repo root ----------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

# ---------- colors (only when stdout is a tty) ----------
if [ -t 1 ]; then
  C_RED=$'\033[31m'
  C_GREEN=$'\033[32m'
  C_YELLOW=$'\033[33m'
  C_BLUE=$'\033[34m'
  C_BOLD=$'\033[1m'
  C_RESET=$'\033[0m'
else
  C_RED=""
  C_GREEN=""
  C_YELLOW=""
  C_BLUE=""
  C_BOLD=""
  C_RESET=""
fi

log_info()  { printf '%s[info]%s %s\n'  "$C_BLUE"   "$C_RESET" "$*"; }
log_ok()    { printf '%s[ ok ]%s %s\n'  "$C_GREEN"  "$C_RESET" "$*"; }
log_warn()  { printf '%s[warn]%s %s\n'  "$C_YELLOW" "$C_RESET" "$*" >&2; }
log_error() { printf '%s[err ]%s %s\n'  "$C_RED"    "$C_RESET" "$*" >&2; }

usage() {
  sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
}

# ---------- parse args ----------
BUMP_ARG=""
DRY_RUN=0
PUSH=1
REMOTE="origin"
TAG_MESSAGE=""
ASSUME_YES=0
# Allow dirty working tree / non-semver latest tag — mostly for --dry-run demos.
ALLOW_DIRTY=0

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help)
      usage
      exit 0
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    --no-push)
      PUSH=0
      shift
      ;;
    --yes|-y)
      ASSUME_YES=1
      shift
      ;;
    --allow-dirty)
      ALLOW_DIRTY=1
      shift
      ;;
    --remote)
      [ $# -ge 2 ] || { log_error "--remote requires a value"; exit 2; }
      REMOTE="$2"
      shift 2
      ;;
    --remote=*)
      REMOTE="${1#--remote=}"
      shift
      ;;
    --message|-m)
      [ $# -ge 2 ] || { log_error "--message requires a value"; exit 2; }
      TAG_MESSAGE="$2"
      shift 2
      ;;
    --message=*)
      TAG_MESSAGE="${1#--message=}"
      shift
      ;;
    -*)
      log_error "Unknown flag: $1"
      usage
      exit 2
      ;;
    *)
      if [ -n "$BUMP_ARG" ]; then
        log_error "Unexpected extra argument: $1 (already got '$BUMP_ARG')"
        exit 2
      fi
      BUMP_ARG="$1"
      shift
      ;;
  esac
done

if [ -z "$BUMP_ARG" ]; then
  log_error "Missing version argument. Pass major|minor|patch or an explicit version like 1.2.3"
  usage
  exit 2
fi

# ---------- sanity: must be a git repo ----------
if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  log_error "Not inside a git work tree: $REPO_ROOT"
  exit 1
fi

# ---------- sanity: working tree clean ----------
if [ -n "$(git status --porcelain)" ]; then
  if [ $ALLOW_DIRTY -eq 1 ] && [ $DRY_RUN -eq 1 ]; then
    log_warn "Working tree is dirty, continuing anyway because --allow-dirty --dry-run."
    git status --short | sed 's/^/        /' >&2
  else
    log_error "Working tree is dirty. Commit or stash your changes first."
    git status --short | sed 's/^/        /'
    exit 1
  fi
fi

# ---------- sanity: branch upstream fast-forward ----------
CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [ "$CURRENT_BRANCH" = "HEAD" ]; then
  log_error "Detached HEAD. Check out a branch before bumping."
  exit 1
fi

UPSTREAM="$(git rev-parse --abbrev-ref --symbolic-full-name '@{u}' 2>/dev/null || true)"
if [ -n "$UPSTREAM" ]; then
  # Make sure we have the latest remote refs for an accurate comparison.
  log_info "Fetching $REMOTE (tags + refs) ..."
  git fetch --tags --quiet "$REMOTE" || log_warn "git fetch $REMOTE failed, proceeding with local refs"

  LOCAL_HEAD="$(git rev-parse HEAD)"
  REMOTE_HEAD="$(git rev-parse "$UPSTREAM")"
  BASE="$(git merge-base HEAD "$UPSTREAM" 2>/dev/null || true)"

  if [ "$LOCAL_HEAD" != "$REMOTE_HEAD" ]; then
    if [ "$BASE" = "$LOCAL_HEAD" ]; then
      log_error "Local branch '$CURRENT_BRANCH' is behind '$UPSTREAM'. Pull first."
      exit 1
    elif [ "$BASE" = "$REMOTE_HEAD" ]; then
      log_warn "Local branch '$CURRENT_BRANCH' is ahead of '$UPSTREAM' (has unpushed commits)."
      # Not fatal: user may intentionally want the tag to include local commits.
      # But we'd better surface it.
    else
      log_error "Local branch '$CURRENT_BRANCH' and '$UPSTREAM' have diverged. Resolve first."
      exit 1
    fi
  fi
else
  log_warn "Current branch '$CURRENT_BRANCH' has no upstream; skipping fast-forward check."
fi

# ---------- discover latest tag ----------
# Only consider semver-ish tags: vX.Y.Z (optionally with pre-release / build suffix).
SEMVER_RE='^v[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.-]+)?(\+[0-9A-Za-z.-]+)?$'

LATEST_TAG="$(git tag --list 'v*' --sort=-v:refname | awk 'NR==1')"
if [ -z "$LATEST_TAG" ]; then
  log_info "No existing v* tag found; treating baseline as v0.0.0."
  LATEST_TAG="v0.0.0"
else
  log_info "Latest tag: $LATEST_TAG"
fi

# Strip pre-release / build metadata for the numeric bump math; we bump on the
# released (X.Y.Z) portion only.
LATEST_CORE="${LATEST_TAG#v}"
LATEST_CORE="${LATEST_CORE%%-*}"
LATEST_CORE="${LATEST_CORE%%+*}"

if ! [[ "$LATEST_CORE" =~ ^([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  log_error "Latest tag '$LATEST_TAG' does not parse as semver; refuse to guess bump."
  exit 1
fi
LATEST_MAJOR="${BASH_REMATCH[1]}"
LATEST_MINOR="${BASH_REMATCH[2]}"
LATEST_PATCH="${BASH_REMATCH[3]}"

# ---------- compute new version ----------
case "$BUMP_ARG" in
  major)
    NEW_MAJOR=$((LATEST_MAJOR + 1))
    NEW_VERSION="v${NEW_MAJOR}.0.0"
    ;;
  minor)
    NEW_MINOR=$((LATEST_MINOR + 1))
    NEW_VERSION="v${LATEST_MAJOR}.${NEW_MINOR}.0"
    ;;
  patch)
    NEW_PATCH=$((LATEST_PATCH + 1))
    NEW_VERSION="v${LATEST_MAJOR}.${LATEST_MINOR}.${NEW_PATCH}"
    ;;
  *)
    # Explicit version; tolerate leading v.
    CANDIDATE="$BUMP_ARG"
    [[ "$CANDIDATE" == v* ]] || CANDIDATE="v$CANDIDATE"
    NEW_VERSION="$CANDIDATE"
    ;;
esac

if ! [[ "$NEW_VERSION" =~ $SEMVER_RE ]]; then
  log_error "Computed version '$NEW_VERSION' is not valid semver (expected vMAJOR.MINOR.PATCH[-pre][+build])."
  exit 1
fi

# ---------- sanity: tag must not already exist ----------
if git rev-parse -q --verify "refs/tags/$NEW_VERSION" >/dev/null; then
  log_error "Tag $NEW_VERSION already exists locally."
  exit 1
fi
if git ls-remote --tags --exit-code "$REMOTE" "refs/tags/$NEW_VERSION" >/dev/null 2>&1; then
  log_error "Tag $NEW_VERSION already exists on remote '$REMOTE'."
  exit 1
fi

# ---------- default tag message ----------
if [ -z "$TAG_MESSAGE" ]; then
  TAG_MESSAGE="Release $NEW_VERSION"
fi

# ---------- summary ----------
echo
printf '%sRelease summary%s\n' "$C_BOLD" "$C_RESET"
printf '  Repo        : %s\n' "$REPO_ROOT"
printf '  Branch      : %s\n' "$CURRENT_BRANCH"
printf '  Remote      : %s\n' "$REMOTE"
printf '  Prev tag    : %s\n' "$LATEST_TAG"
printf '  New  tag    : %s%s%s\n' "$C_GREEN" "$NEW_VERSION" "$C_RESET"
printf '  Tag message : %s\n' "$TAG_MESSAGE"
printf '  Push to     : %s\n' "$([ $PUSH -eq 1 ] && echo "$REMOTE (will trigger release-tag.yml)" || echo '(skipped, local only)')"
printf '  Dry run     : %s\n' "$([ $DRY_RUN -eq 1 ] && echo yes || echo no)"
echo

# ---------- confirm ----------
if [ $DRY_RUN -eq 0 ] && [ $ASSUME_YES -eq 0 ]; then
  if [ -t 0 ]; then
    printf 'Proceed? [y/N] '
    read -r REPLY
    case "$REPLY" in
      y|Y|yes|YES) : ;;
      *) log_warn "Aborted by user."; exit 1 ;;
    esac
  else
    log_error "Non-interactive shell; pass --yes to confirm."
    exit 1
  fi
fi

# ---------- act ----------
if [ $DRY_RUN -eq 1 ]; then
  log_info "[dry-run] git tag -a $NEW_VERSION -m \"$TAG_MESSAGE\""
  [ $PUSH -eq 1 ] && log_info "[dry-run] git push $REMOTE $NEW_VERSION"
  log_ok "Dry run complete; no changes made."
  exit 0
fi

log_info "Creating annotated tag $NEW_VERSION ..."
git tag -a "$NEW_VERSION" -m "$TAG_MESSAGE"
log_ok "Tag created locally."

if [ $PUSH -eq 1 ]; then
  log_info "Pushing $NEW_VERSION to $REMOTE ..."
  if ! git push "$REMOTE" "$NEW_VERSION"; then
    log_error "Push failed. The local tag is still present; inspect with 'git tag' and retry or 'git tag -d $NEW_VERSION' to roll back."
    exit 1
  fi
  log_ok "Pushed $NEW_VERSION to $REMOTE."
  log_info "GitHub Actions should now be running .github/workflows/release-tag.yml for $NEW_VERSION."
  # Try to print a direct link to the Actions page if we can infer the GitHub repo slug.
  REMOTE_URL="$(git remote get-url "$REMOTE" 2>/dev/null || true)"
  if [[ "$REMOTE_URL" =~ github\.com[:/]+([^/]+)/([^/.]+)(\.git)?$ ]]; then
    OWNER="${BASH_REMATCH[1]}"
    REPO="${BASH_REMATCH[2]}"
    printf '  Actions: https://github.com/%s/%s/actions\n' "$OWNER" "$REPO"
    printf '  Release: https://github.com/%s/%s/releases/tag/%s\n' "$OWNER" "$REPO" "$NEW_VERSION"
  fi
else
  log_warn "Skipped push (--no-push). To push later: git push $REMOTE $NEW_VERSION"
fi
