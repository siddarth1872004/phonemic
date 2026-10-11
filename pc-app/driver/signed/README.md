# Microsoft-signed driver

Put the files Microsoft returns from attestation signing here:

- `phonemic.sys`
- `phonemic.cat`
- `phonemic.inf`

When these are present, Microsoft-signed, and their `DriverVer` matches
`../phonemic.inf`, CI ships them in `PhoneMic-windows-x64.zip` instead of the
unsigned build, and the installer installs them without test mode.

If the driver source changes (and `DriverVer` is bumped), these files no
longer match: CI then ships the unsigned build again until the new version is
signed. See [docs/SIGNING.md](../../../docs/SIGNING.md).
