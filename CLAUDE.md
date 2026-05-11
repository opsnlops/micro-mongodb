# Working in this repo

## Formatting — run before every commit

`clang-format` must be run over all of our C/C++ before any commit. From the
repo root:

```bash
find src -type f \( -name '*.c' -o -name '*.h' \) -exec clang-format -i {} +
```

The style file at the repo root is LLVM-based, 4-space indent, no tabs,
120-column limit.

`src/bson/` has its own `.clang-format` with `DisableFormat: true` because
those files are vendored libbson 1.28.0 — keeping them byte-for-byte upstream
makes diffs against future mongo-c-driver releases legible. The
`find … -exec clang-format` line above respects that and will skip them.

If clang-format changes anything, stage the formatting separately from the
substantive change so reviews stay easy to read.
