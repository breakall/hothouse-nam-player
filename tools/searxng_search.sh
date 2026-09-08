#!/bin/sh
set -eu

SEARXNG_ENDPOINT="${SEARXNG_ENDPOINT:-http://192.168.1.3:8888}"

if [ "$#" -eq 0 ]; then
  echo "usage: $0 search terms..." >&2
  exit 2
fi

curl -fsSG \
  --data-urlencode "q=$*" \
  --data-urlencode "format=json" \
  "$SEARXNG_ENDPOINT/search"
