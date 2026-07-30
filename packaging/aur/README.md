# AUR packaging

The `geas` package on the AUR builds from a tagged release tarball. This
directory carries the PKGBUILD the AUR copy is cut from; the AUR repository
itself holds only the PKGBUILD and the generated .SRCINFO.

## Cutting a release

1. Tag the release and push the tag: `git tag vX.Y.Z && git push origin vX.Y.Z`.
2. In the AUR checkout (`git clone ssh://aur@aur.archlinux.org/geas.git`),
   copy this PKGBUILD in and bump `pkgver`.
3. Pin the tarball hash and regenerate the metadata:

   ```sh
   updpkgsums
   makepkg --printsrcinfo > .SRCINFO
   ```

4. Build once from scratch to prove it: `makepkg -sfc`.
5. Commit PKGBUILD and .SRCINFO to the AUR repository and push.

## Layout the package installs

| Path | What |
|---|---|
| `/usr/bin/geas` | the compiler |
| `/usr/lib/libgeasrt.so` | the runtime |
| `/usr/include/geas/*.h` | the C ABI headers the emitted C compiles against |
| `/usr/share/geas/std/*.geas` | the std modules the loader's system probe finds |

The installed toolchain needs no environment variables: the compiler probes
for a repository checkout first and falls back to the system paths above.
`GEAS_ROOT` and `GEAS_HOME` still override both, in that order of specificity,
for out of tree development.
