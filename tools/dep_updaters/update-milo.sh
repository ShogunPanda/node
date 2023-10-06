#!/bin/sh
set -e

# Shell script to update llhttp in the source tree to specific version

BASE_DIR=$(cd "$(dirname "$0")/../.." && pwd)
DEPS_DIR="${BASE_DIR}/deps"

[ -z "$NODE" ] && NODE="$BASE_DIR/out/Release/node"
[ -x "$NODE" ] || NODE=$(command -v node)

# shellcheck disable=SC1091
. "$BASE_DIR/tools/dep_updaters/utils.sh"

NEW_VERSION="$("$NODE" --input-type=module <<'EOF'
const res = await fetch('https://api.github.com/repos/ShogunPanda/milo/releases/latest',
  process.env.GITHUB_TOKEN && {
    headers: {
      "Authorization": `Bearer ${process.env.GITHUB_TOKEN}`
    },
  });
if (!res.ok) throw new Error(`FetchError: ${res.status} ${res.statusText}`, { cause: res });
const { tag_name } = await res.json();
console.log(tag_name.replace('release/v', ''));
EOF
)"

CURRENT_VERSION=$(grep "#define MILO_VERSION" ./deps/milo/milo.h | sed -n "s/^.*VERSION \"\(.*\)\"/\1/p")

# This function exit with 0 if new version and current version are the same
compare_dependency_version "milo" "$NEW_VERSION" "$CURRENT_VERSION"

cleanup () {
  EXIT_CODE=$?
  [ -d "$WORKSPACE" ] && rm -rf "$WORKSPACE"
  exit $EXIT_CODE
}

echo "Making temporary workspace ..."
WORKSPACE=$(mktemp -d 2> /dev/null || mktemp -d -t 'tmp')
trap cleanup INT TERM EXIT

cd "$WORKSPACE"

MILO_REF="v$NEW_VERSION"
MILO_ZIP="milo-${MILO_REF}.zip"

echo "Fetching milo archive ..."
curl -sL -o "$MILO_ZIP" "https://github.com/ShogunPanda/milo/releases/download/$MILO_ZIP"
log_and_verify_sha256sum "milo" "$MILO_ZIP"
unzip "$MILO_ZIP"
rm "$MILO_ZIP"

echo "Copying milo release ..."
rm -rf "$DEPS_DIR/milo"
mv "$WORKSPACE" "$DEPS_DIR/milo"

# Update the version number on maintaining-dependencies.md
# and print the new version as the last line of the script as we need
# to add it to $GITHUB_ENV variable
finalize_version_update "milo" "$NEW_VERSION"
