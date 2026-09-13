# OBS Multistream RTMP — Linux artifact

This tarball contains the OBS Multistream RTMP plugin and its data/documentation files. It does **not** bundle OBS Studio, libobs, Qt, or system libraries.

## Supported target

- Linux x86_64.
- OBS Studio **32.2.2**, with the `libobs` and `obs-frontend-api` ABI used by the validated build.
- Qt 6, Qt WebEngine 6 and libsecret runtime provided by the OBS installation or distribution.

The release artifact is validated only against OBS Studio 32.2.2. Compatibility with other OBS versions, including other 32.2.x patch releases, is not guaranteed until separately built and tested. The exact OBS prefix used to build an artifact must be recorded with the release; a tarball built against one OBS ABI is not automatically compatible with every OBS package.

## Installation

Extract the tarball into the OBS installation prefix used by the target system. For a custom OBS prefix:

```bash
tar -xzf obs-multistream-rtmp-<version>-Linux-x86_64.tar.gz -C /path/to/obs-prefix
```

The plugin is installed below `lib/obs-plugins/` and resources below `share/obs/obs-plugins/obs-multistream-rtmp/`.

Close and reopen OBS after installation. Confirm that the `Multistream RTMP` dock is available from the OBS Docks menu.

## Verification

Check that the module exists in the prefix:

```bash
find /path/to/obs-prefix -name obs-multistream-rtmp.so -print
```

If OBS cannot load the plugin, start it with `--verbose` and check for an ABI or missing-library error. Do not copy OBS or Qt libraries from the build machine into the plugin directory.

## Scope and security

Facebook OAuth runs inside an embedded Qt WebEngine view and the plugin captures the callback automatically; copying and pasting a redirect URL is no longer required. The callback URL can contain an access token during internal navigation; never publish it or include it in support logs. The plugin contains no App Secret and stores the token only in the operating system secure credential store.

This artifact is a Linux staging/distribution format, not a signed public release. The project also provides a Debian package for normal OBS 32.2.2 installations. Public releases still require checksums/signing, an exact OBS compatibility record, and manual validation on a clean installation.

See the installed project README and `docs/production-scope.md` for configuration, Meta requirements, limitations, and support procedures.
