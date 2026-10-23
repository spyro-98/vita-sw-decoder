# Security policy

Report decoder memory-safety and malformed-media issues privately to the
maintainer before disclosure. Do not attach copyrighted media or console dumps
containing personal data to public issues. Only the current default branch and
latest release are supported.

Reports should identify package commit, actual FFmpeg prefix/version, embedding
app build, source kind and whether the input is seekable or live. A cooperative
abort callback cannot make a non-cooperative read return. Surface/callback
lifetime and malformed MP4 duration belong to the security review; avoid
workarounds that skip validation or free GPU-owned buffers. See
[runtime contract](docs/RUNTIME.md) and [validation](docs/VALIDATION.md).
