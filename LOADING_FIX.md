# NetSurf PS4 1.10 — isolated loading fix for 1.9

This branch intentionally starts at the reported failing 1.9 commit d3efee4.
It does not replace independent-engine-poc or its later 3.x work.

## Changes

- Translate the PS4 kernel stat layout field by field. The 0.5.4 SDK layout
  uses the wrong mode width and can read the block count as the byte size.
- Embed core HTML/CSS resources through NetSurf's resource-data API.
- Prevent a failed internal error page from recursively opening itself.
- Interpret HTTP content-length type as an enum (EXIST is zero), detect
  truncated replies, pass redirects to NetSurf and skip inactive fetches.
- Advance scheduled work using the PS4 monotonic process clock.
- Start at Google's lightweight HTML URL; keep TLS certificate verification.
- Write diagnostics to /data/ps4-browser-netsurf.log.

## Validation and limits

Host tests exercise the real fetcher with mocked Sony HTTP APIs and the stat
translation with kernel-layout fixtures. They are not console network tests.
GitHub Actions builds the PS4 ELF, runs those tests, packages the PKG and
verifies its SHA-256. Hardware rendering still needs the user's PS4 test.

This minimal branch has no JavaScript, image decoders, video or background
downloads. It is a loading regression test, not the proposed WebKit2 browser.
Network calls are synchronous and can temporarily block the UI.

Install the PKG, launch it and wait for the Google page. If it remains blank,
send a screenshot including the status bar and, if available, the diagnostic
log above. The existing package title ID is retained (PBNF00001), so this
installs over the NetSurf test app; it does not install alongside that app.

References:
- https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/pull/278
- https://github.com/ps4dev/orbis-ports
- https://github.com/netsurf-browser/netsurf
- https://github.com/etonedemid/netsurf-switch
