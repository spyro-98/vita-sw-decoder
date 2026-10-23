# Contributing

Contributions are GPL-3.0-only and require a Developer Certificate of Origin
`Signed-off-by` line. Do not submit proprietary Sony material. FFmpeg changes
must retain LGPL compatibility, pin the source hash and ship corresponding
source that can reproduce the static archive.

Before changes, read [runtime ownership](docs/RUNTIME.md). Follow
[validation/release instructions](docs/VALIDATION.md); run `git diff --check`,
compile the example for API changes, and use sibling app regressions for timing
or buffer changes. Hardware fixes require physical evidence; mark unperformed
checks explicitly. Documentation updates must preserve historical measurements
and the difference between app and standalone FFmpeg prefixes. Do not bump the
2.0.0 ABI for a documentation-only refresh.
