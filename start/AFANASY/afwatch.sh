#!/bin/bash
# Name=Start Watch...
source "`dirname "$0"`/_setup.sh"
if [ ! -z "$AF_WATCH_CMD" ]; then
   "$AF_WATCH_CMD" "$@"
else
   "$AF_ROOT/bin/afwatch" "$@"
fi