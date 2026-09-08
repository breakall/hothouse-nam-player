#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
repository=https://github.com/bkshepherd/DaisySeedProjects.git
revision=ccae0f2305881d9f706557404ba22c0675c97bd7
destination="$project_root/external/DaisySeedProjects"
runtime=Software/GuitarPedal/Effect-Modules/Nam/nam_a2_runtime.h

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

if [ ! -f "$destination/$runtime" ]; then
  echo "Pinned A2 runtime is missing: $destination/$runtime" >&2
  exit 1
fi

echo "A2 runtime ready at revision $revision"
