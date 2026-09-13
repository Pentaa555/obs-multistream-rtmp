# OBS Multistream RTMP — Debian package

This package targets Linux x86_64 systems with **OBS Studio 32.2.2** installed normally through the distribution or the OBS package repository.

## Installation

Close OBS, then install the downloaded package with:

```bash
sudo apt install ./obs-multistream-rtmp_0.1.0_amd64.deb
```

`apt` installs the package dependencies and places the plugin at:

```text
/usr/lib/x86_64-linux-gnu/obs-plugins/obs-multistream-rtmp.so
/usr/share/obs/obs-plugins/obs-multistream-rtmp/
```

Reopen OBS and find **Multistream RTMP** in the **Docks** menu.

## Requirements

- OBS Studio 32.2.2 x86_64.
- Qt 6 runtime libraries supplied by the OBS/distribution installation.
- `libsecret-1-0`.
- An active Linux Secret Service implementation for persistent OAuth tokens.

The package does not include OBS Studio, Qt, or system libraries. It must not be installed over a different OBS ABI without a dedicated rebuild.

## OAuth and security

The Desktop OAuth client credential used by a public release is an application credential, not a per-user password. The binary may contain that public-client credential when Google requires it for the Desktop client. PKCE, `state`, and the loopback redirect protect the authorization flow.

User refresh tokens remain per-user data and are stored only in the operating-system secure credential store. No OBS profile, build directory, access token, refresh token, RTMP URL, or stream key is included in the package.

The package is licensed under GPL-2.0-or-later. Verify the SHA256 checksum before installation.
