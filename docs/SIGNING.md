# Getting the Windows driver signed by Microsoft

Windows 11 loads a kernel driver only if Microsoft has signed it, unless the PC
is in test mode. PhoneMic's driver is signed through **attestation signing**:
you submit the driver package through Microsoft Partner Center, Microsoft
checks and signs it, and you ship the signed files. Once that is done, the
PhoneMic installer installs the driver directly: no test mode, no Secure Boot
change, no warnings.

Submissions are free. What costs time and money is proving who you are, once.

## What you need (once)

1. **An EV code-signing certificate.** Certificate authorities such as
   DigiCert, Sectigo, GlobalSign and SSL.com sell them. They verify your
   identity first (usually a registered business; some also accept a sole
   proprietor), which takes from a few days to a few weeks. Expect to pay
   several hundred US dollars a year. The private key lives on a hardware token
   or in the CA's cloud signing service, never in a plain file.
2. **A Windows Hardware Developer Program account** in Partner Center (free):
   https://partner.microsoft.com/dashboard/registration/hardware. It needs a
   Microsoft Entra ID (work) account, and you prove you own the EV certificate
   by signing a small file Microsoft gives you during registration.

## Each time the driver changes

1. **Get the submission package.** Every CI run on `main` builds
   `phonemic.cab` from the current source and checks the INF with Microsoft's
   InfVerif first. Download it from GitHub: Actions, the latest **build** run
   on `main`, artifact **PhoneMic-driver-submission**.
2. **Sign the CAB with your EV certificate**, on a Windows PC with the Windows
   SDK (or with your CA's own signing tool):

   ```powershell
   signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a phonemic.cab
   ```

3. **Submit it.** In Partner Center, open Hardware and choose
   **Submit new hardware**. Give it a name such as `PhoneMic 0.3.1`, upload the
   signed `phonemic.cab`, and under requested signatures pick the Windows 11
   client x64 versions (22H2 and newer). Don't add HLK test results: this is an
   attestation submission. Processing usually takes minutes to a few hours.
4. **Download the signed files** from the finished submission and put
   `phonemic.sys`, `phonemic.cat` and `phonemic.inf` into
   `pc-app/driver/signed/`. Commit them (a pull request is fine).
5. **Release.** Tag the release as usual. CI checks that the files are signed
   by Microsoft and match the source's `DriverVer`, then ships them in
   `PhoneMic-windows-x64.zip`.

When you change the driver code later, bump `DriverVer` in
`pc-app/driver/phonemic.inf` and repeat these steps. Until the new version is
signed, CI ships the unsigned build, so the installer falls back to test mode
instead of installing a mismatched driver.

## For people who already used test mode

Run `driver\uninstall-driver.ps1` from the old download. It removes the
test-signed driver and its local certificate and offers to turn test mode
off. Then install the new release normally.

## Notes

- Attestation-signed drivers load on Windows 10 and 11 client editions, not on
  Windows Server. PhoneMic's driver needs Windows 11 22H2 or newer anyway.
- The INF's provider ("PhoneMic Project") is what Device Manager shows; the
  signature itself is Microsoft's ("Microsoft Windows Hardware Compatibility
  Publisher").
- Publishing through Windows Update is a separate, optional step. It isn't
  needed: the PhoneMic installer installs the signed driver itself.
