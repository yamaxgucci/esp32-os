# miniz (vendored)

Upstream: https://github.com/richgel999/miniz (MIT / see file headers).

Used by:

- ArgonOS kernel `unzip` builtin — reader + inflate only
  (`MINIZ_NO_STDIO`, `MINIZ_NO_TIME`, `MINIZ_NO_ARCHIVE_WRITING_APIS`)
- `apps/zip` (`ZIP.AXE`) — full reader + writer (`MINIZ_NO_STDIO`, `MINIZ_NO_TIME`)

Do not edit these sources for Argon-specific behaviour; wrap them instead.
