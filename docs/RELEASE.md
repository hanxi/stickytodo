# Release Guide

How stickytodo gets from a git commit to downloadable binaries / DMG / Docker images.

## Artifact matrix

Every release (tag or branch pre-release) produces the following:

| Platform | Artifact | Built by |
|---|---|---|
| linux/amd64      | `stickytodo-server-<v>-linux-amd64`       | `scripts/package-server.sh` |
| linux/arm64      | `stickytodo-server-<v>-linux-arm64`       | `scripts/package-server.sh` |
| linux/armv7      | `stickytodo-server-<v>-linux-armv7`       | `scripts/package-server.sh` |
| darwin/amd64     | `stickytodo-server-<v>-darwin-amd64`      | `scripts/package-server.sh` |
| darwin/arm64     | `stickytodo-server-<v>-darwin-arm64`      | `scripts/package-server.sh` |
| windows/amd64    | `stickytodo-server-<v>-windows-amd64.exe` | `scripts/package-server.sh` |
| windows/arm64    | `stickytodo-server-<v>-windows-arm64.exe` | `scripts/package-server.sh` |
| macOS universal  | `stickytodo-<v>-macos-universal.dmg`      | `scripts/package-mac-client.sh` |
| Docker multi-arch | `docker.io/hanxi/stickytodo:<v>` (amd64/arm64/armv7) | `.github/workflows/_build-all.yml` (buildx) |
| Checksums        | `SHA256SUMS`                              | both `package-server.sh` and `package-mac-client.sh` |

All server binaries are produced with `CGO_ENABLED=0` thanks to the pure-Go SQLite driver (`glebarez/sqlite`), so no cross-compiler toolchain is needed.

## Local packaging

Run from the repo root. Output lands in `dist/`.

```bash
# 1. Web SPA → server/internal/webui/dist/ (embedded by go:embed)
bash scripts/package-web.sh

# 2. Seven cross-compiled server binaries → dist/server/
VERSION=dev-local bash scripts/package-server.sh

# 3. macOS universal DMG → dist/mac-client/
#    Requires Xcode + create-dmg (brew install create-dmg) or falls back to hdiutil.
VERSION=dev-local bash scripts/package-mac-client.sh

# 4. Docker image for the current host platform only → local daemon
#    Multi-arch builds live in CI (see below); this script is for local smoke.
VERSION=dev-local bash scripts/package-docker.sh
```

## GitHub Actions

### Tag release (automatic)

Pushing a tag matching `v*` triggers `.github/workflows/release-tag.yml`, which delegates to the reusable `_build-all.yml`:

```bash
git tag v1.2.3
git push origin v1.2.3
# → Actions runs build-web, build-server (7 bins), build-mac-dmg,
#   build-docker (3 archs, pushed to docker.io/hanxi/stickytodo:v1.2.3 + :latest),
#   then publish-release attaches every artifact to the GitHub Release.
```

### Branch pre-release (manual)

For hotfix rehearsals / QA hand-offs, trigger `release-branch.yml` from the Actions tab (`workflow_dispatch`). Optionally pass a `branch` input (defaults to the dispatched ref).

The workflow:

1. Deletes any prior `branch-<name>` release **and** the underlying git tag via a **3-stage fallback** in `cleanup-old-release` (see `release-branch.yml`):
   - Stage 1 (modern `gh` ≥ 2.28): `gh release delete "$TAG" --yes --cleanup-tag` — one-shot delete of release + tag.
   - Stage 2 (older `gh` without `--cleanup-tag`): `gh release delete "$TAG" --yes` to drop the release, then `git push --delete origin "$TAG"` to drop the tag separately.
   - Stage 3 (first-ever run — neither release nor tag exists): each command is guarded with `|| true` / explicit `rev-parse` checks so the job still exits 0.
2. Re-runs the full `_build-all` pipeline with `prerelease: true` and a tag named `branch-<name>` (slashes in branch names are sanitised to `-` by the bash substitution `branch-${branch//\//-}` because git tag refs can't contain `/` at arbitrary positions).
3. Pushes the Docker image as `docker.io/hanxi/stickytodo:branch-<name>` but **does not** update `:latest` (enforced by `tag_latest: false` in the caller's `with:` block).

### Reusable workflow

`_build-all.yml` accepts these inputs:

| Input | Required | Default | Purpose |
|---|---|---|---|
| `version`      | yes | —               | Baked into filenames + Docker tag |
| `tag_name`     | yes | —               | Git tag the GitHub Release is bound to |
| `prerelease`   | no  | `false`         | Marks the release as pre-release |
| `docker_image` | no  | `docker.io/hanxi/stickytodo` | Set to empty string to skip the Docker job (the `detect-docker-creds` job itself is guarded by `if: inputs.docker_image != ''`, so an empty value short-circuits the whole docker path) |
| `tag_latest`   | no  | `false`         | Also push `:latest` (only used by tag releases) |

## Required secrets

Set these in the repo's *Settings → Secrets and variables → Actions*:

| Secret | Purpose |
|---|---|
| `DOCKERHUB_USERNAME` | Docker Hub login for `docker/login-action`. Leave unset to skip Docker push. |
| `DOCKERHUB_TOKEN`    | Docker Hub access token (preferably a scoped one with `Read, Write, Delete` on `hanxi/stickytodo`). |

### How the Docker job is skipped

There are **two independent skip paths** — either one alone is enough to keep the image out of the registry:

1. **Input-driven** (`docker_image == ''`): the caller of `_build-all.yml` passes an empty string → the `detect-docker-creds` job's job-level `if:` evaluates false → job never runs → downstream `build-docker` (which requires `needs.detect-docker-creds.outputs.have == 'true'`) is also skipped.
2. **Secret-driven** (`DOCKERHUB_USERNAME` empty): `detect-docker-creds` runs, its inline bash emits `have=false` → `build-docker` sees `have != 'true'` and is skipped.

`publish-release` uses `needs.build-docker.result == 'success' || needs.build-docker.result == 'skipped'`, so in either skip path the GitHub Release still gets published with the server binaries + DMG attached — just without a Docker push.

`GITHUB_TOKEN` is provided automatically and has `contents: write` via the `permissions:` block.

## macOS Gatekeeper note (ad-hoc signing)

We do **not** pay for an Apple Developer ID, so `stickytodo-<v>-macos-universal.dmg` is signed ad-hoc (`codesign -s -`). On first launch users will see:

> "stickytodo" can't be opened because Apple cannot check it for malicious software.

Workaround: right-click the app → **Open** → confirm. This only needs to happen once per machine; subsequent launches use Launch Services' approval cache.

If you later acquire a Developer ID, replace the `codesign -s -` line in `scripts/package-mac-client.sh` with your signing identity and wire up notarization (`xcrun notarytool submit`).

## Troubleshooting

- **`package-server.sh` fails on Linux with "undefined: syscall.XXX"** — the armv7 target needs a recent-enough Go (1.21+). We pin 1.25 in `server/go.mod`.
- **Docker build hangs on `docker buildx` step** — ensure the runner has `docker/setup-qemu-action` registered binfmt handlers. Our workflow does; local buildx may need `docker run --privileged --rm tonistiigi/binfmt --install all`.
- **`gh release delete --cleanup-tag` unknown flag** — older `gh` versions lack it; the workflow falls back to `git push --delete origin <tag>` already.
- **DMG step errors with "hdiutil: Resource busy"** — usually means a previous run's mount is still referenced by Finder. `hdiutil detach` any `/Volumes/stickytodo *` and retry; the script also retries up to 3 times with 2s back-off.
