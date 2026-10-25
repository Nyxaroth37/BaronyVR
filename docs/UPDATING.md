# Publishing launcher updates

Public Beta 2 introduces update protocol 1. Users of Public Beta 1 install Beta 2 manually once. Players do not need GitHub accounts or publisher credentials.

## Release contract

The launcher reads the ten most recent releases from `Nyxaroth37/BaronyVR`. Drafts are ignored; compatible prereleases are included. A release without the descriptor below is ignored. It selects the highest compatible numeric sequence above the installed sequence; publishing a source commit or a bare Git tag does nothing to installed players.

Each published update needs these release assets:

- `BaronyVR_<version>_Windows.zip`, with a single root folder containing the complete runtime/install payload.
- `baronyvr-update.json`, a small descriptor matching that release tag and ZIP.
- `SHA256SUMS.txt` for manual verification.

Example descriptor:

```json
{
  "protocol": 1,
  "min_launcher": 1,
  "version": "GA21-PublicBeta3",
  "sequence": 3,
  "tag": "ga21-public-beta3",
  "package_name": "BaronyVR_GA21_PublicBeta3_Windows.zip",
  "package_sha256": "64_HEXADECIMAL_CHARACTERS_FOR_THE_FINAL_ZIP"
}
```

`version` must be a safe folder name, beginning with an alphanumeric character and containing only letters, digits, underscores, periods, or hyphens. Do not reuse an existing version name for different contents. `sequence` is a globally increasing integer, separate from the human-readable name. Public Beta 2 uses sequence 2. Do not publish an update with the same or a lower sequence.

The ZIP's `payload.json` must use `update_protocol: 1`, the exact `version`, matching `release_sequence`, and SHA256 entries for every other included file. It must include `Runtime/barony.exe`, `Common.ps1`, `LaunchGame.ps1`, and the default profile. Rebuild manifests after the final byte changes and calculate the ZIP hash last. Do not include personal `installation.json`, state files, credentials, retail game assets, or proprietary retail DLLs.

GitHub's asset digest must agree with the descriptor and downloaded ZIP. The descriptor itself is checked against its GitHub asset digest. Verification uses HTTPS and the official repository; it is integrity checking, not a separate publisher signing-key system. A compromised publisher account remains a risk: protect account access and release permissions.

## Launcher compatibility

The fixed launcher and update engine live in `Managed/Launcher`, while game packages live in `Managed/Versions/<version>`. Runtime updates do not replace the running fixed launcher. Set `min_launcher` above 1 when a future release requires an incompatible launcher; protocol-1 clients offer the full release installer instead of applying that package automatically.

## State, rollback, and cleanup

`active.json` is switched atomically after the new package passes verification. One previous version is retained. A failed download or validation leaves the active version unchanged. Saved calibration/settings and native game saves remain in retail Barony; rollback does not roll those files back.

Successful download, staging, check, and result work is removed. Abandoned temporary work is capped at three recent owned folders per category and ages out after 24 hours. State backups use fixed names. Cleanup requires ownership markers, refuses linked paths, and keeps unexpected personal files. Only active and previous managed runtime versions are normally retained. Known legacy installer/launcher folders have additional identity and content checks before removal.

## Validation before publication

1. Run the Windows syntax checks and `Test-Updater.ps1`.
2. Test fresh install, upgrade, offline play, rollback, and normal game exit.
3. Confirm calibration/saves remain intact and managed folder retention is bounded.
4. Publish source with its provenance, then upload the final ZIP, descriptor, and checksums to a draft prerelease.
5. Download and hash-check the uploaded assets before publishing.
6. Exercise the public update feed and actual apply worker in a separate temporary installation. If that check fails, return the new release to draft for review.

The Public Beta 2 publishing kit performs the upload/hash checks and isolated public-feed update/rollback check. Its temporary integration test does not launch the game or touch retail data. Failed test fixtures are retained for diagnosis; successful fixtures are removed.
