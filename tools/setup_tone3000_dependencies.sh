#!/bin/sh
set -eu
project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repository=https://github.com/tone-3000/embedded.git
revision=b84b60c652cb386ea341be50b8c665ae8a489cff
destination="$project_root/external/tone3000-embedded"
if [ -e "$destination" ] && [ ! -d "$destination/.git" ]; then
  echo "Refusing to replace non-git path: $destination" >&2
  exit 1
fi
if [ ! -d "$destination/.git" ]; then
  git clone --filter=blob:none "$repository" "$destination"
fi
if [ -n "$(git -C "$destination" status --porcelain)" ]; then
  echo "Refusing to change dirty dependency checkout: $destination" >&2
  exit 1
fi
git -C "$destination" fetch --depth 1 origin "$revision"
git -C "$destination" checkout --detach "$revision"
echo "TONE3000 embedded v2.0.0 ready at $revision"
