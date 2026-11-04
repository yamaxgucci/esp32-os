#!/bin/sh
# Build the macOS agent: one universal binary (Intel + Apple silicon).
# Needs only the Command Line Tools (xcode-select --install).
set -e
cd "$(dirname "$0")"
MZ=../../third_party/miniz
clang -Os -Wall -Wno-unused-parameter -arch x86_64 -arch arm64 \
  -mmacosx-version-min=10.13 \
  -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ARCHIVE_APIS -I"$MZ" \
  kvmagent_posix.c "$MZ/miniz.c" "$MZ/miniz_tdef.c" "$MZ/miniz_tinfl.c" \
  -framework IOKit -framework CoreFoundation -framework ApplicationServices \
  -o kvmagent-mac
strip kvmagent-mac
ls -l kvmagent-mac
