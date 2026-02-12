#!/usr/bin/env bash
set -euo pipefail

REMOTE="${REMOTE:-origin}"
BRANCH="${BRANCH:-gh-pages}"
DOXYFILE="${DOXYFILE:-Doxyfile}"
PUBLISH_DIR="${PUBLISH_DIR:-docs/_build/html}"
POSTPROCESS_SCRIPT="${POSTPROCESS_SCRIPT:-docs/doxygen/postprocess-awesome.sh}"

usage() {
  cat <<'EOF'
Usage: ./scripts/publish_api_docs.sh [--dry-run]

Builds Doxygen docs and publishes docs/_build/html to gh-pages.

Environment overrides:
  REMOTE=origin
  BRANCH=gh-pages
  DOXYFILE=Doxyfile
  PUBLISH_DIR=docs/_build/html
  POSTPROCESS_SCRIPT=docs/doxygen/postprocess-awesome.sh
EOF
}

DRY_RUN=0
if [[ "${1:-}" == "--dry-run" ]]; then
  DRY_RUN=1
elif [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
elif [[ -n "${1:-}" ]]; then
  usage
  exit 1
fi

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "error: missing required command: $1" >&2
    exit 1
  fi
}

require_cmd git
require_cmd doxygen
require_cmd rsync

REPO_ROOT="$(git rev-parse --show-toplevel)"
cd "$REPO_ROOT"

if [[ ! -f "$DOXYFILE" ]]; then
  echo "error: missing Doxyfile: $DOXYFILE" >&2
  exit 1
fi

echo "Generating docs with $DOXYFILE..."
doxygen "$DOXYFILE"

if [[ -x "$POSTPROCESS_SCRIPT" ]]; then
  echo "Post-processing docs theme links..."
  "$POSTPROCESS_SCRIPT" "$PUBLISH_DIR"
fi

touch "$PUBLISH_DIR/.nojekyll"

TMP_DIR="$(mktemp -d)"
WORKTREE_DIR="$TMP_DIR/gh-pages"
CLEANUP_WORKTREE=0

cleanup() {
  if [[ "$CLEANUP_WORKTREE" -eq 1 ]]; then
    git worktree remove -f "$WORKTREE_DIR" >/dev/null 2>&1 || true
  fi
  rm -rf "$TMP_DIR"
}
trap cleanup EXIT

echo "Preparing worktree for $REMOTE/$BRANCH..."
git fetch "$REMOTE" "$BRANCH" >/dev/null 2>&1 || true

if git show-ref --verify --quiet "refs/remotes/$REMOTE/$BRANCH"; then
  git worktree add -B "$BRANCH" "$WORKTREE_DIR" "$REMOTE/$BRANCH" >/dev/null
  CLEANUP_WORKTREE=1
else
  git worktree add --detach "$WORKTREE_DIR" HEAD >/dev/null
  CLEANUP_WORKTREE=1
  (
    cd "$WORKTREE_DIR"
    git checkout --orphan "$BRANCH" >/dev/null 2>&1
    git rm -rf . >/dev/null 2>&1 || true
  )
fi

echo "Syncing $PUBLISH_DIR -> $BRANCH worktree..."
# Keep the worktree metadata file intact.
rsync -a --delete --exclude '.git' "$PUBLISH_DIR/" "$WORKTREE_DIR/"

SOURCE_COMMIT="$(git rev-parse --short HEAD)"
TIMESTAMP_UTC="$(date -u +'%Y-%m-%d %H:%M:%S UTC')"
COMMIT_MSG="docs: publish API docs from $SOURCE_COMMIT ($TIMESTAMP_UTC)"

(
  cd "$WORKTREE_DIR"

  if [[ -z "$(git status --porcelain)" ]]; then
    echo "No documentation changes to publish."
    exit 0
  fi

  git add -A
  git commit -m "$COMMIT_MSG" >/dev/null

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "Dry run enabled. Skipping push."
    echo "Created local commit on $BRANCH: $COMMIT_MSG"
  else
    echo "Pushing to $REMOTE/$BRANCH..."
    git push "$REMOTE" "$BRANCH"
  fi
)

echo "Done."
