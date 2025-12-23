# nix-appimage

Create an AppImage, bundling a derivation and all its dependencies into a single-file executable.
Like [nix-bundle](https://github.com/matthewbauer/nix-bundle), but much faster and without the glibc dependency.

## Getting started

To use this, you will need to have [Nix](https://nixos.org/) available.
Then, run this via the [nix bundle](https://nixos.org/manual/nix/unstable/command-ref/new-cli/nix3-bundle.html) interface, replacing `nixpkgs.hello` with the flake you want to build:

```
$ nix bundle --bundler github:ralismark/nix-appimage nixpkgs#hello
```

This produces `hello.AppImage`, which prints "Hello world!" when run:

```
$ ./hello.AppImage
Hello, world!
```

If you get a `entrypoint ... is not executable` error, or want to specify a different binary to run, you can instead use the `./bundle` script:

```
$ ./bundle dnsutils /bin/dig # or ./bundle dnsutils dig
$ ./dig.AppImage -v
DiG 9.18.14
```

You can also use nix-appimage as a nix library -- the flake provides `lib.<system>.mkAppImage` which supports more options.
See mkAppImage.nix for details.

### Loading the host ld.so.conf

Nixpkgs patches glibc so `ld.so.conf` lives under the glibc prefix, which means host search paths in `/etc/ld.so.conf` are ignored. nix-appimage ships an overlay (`overlays.systemLdConf`) that appends an `include /etc/ld.so.conf` line so the bundled loader can fall back to system libraries when something is missing from the closure.

- The `./bundle` helper imports this overlay automatically.
- When driving `nix bundle` directly, import nixpkgs with the overlay before building your package, for example:

```
nix bundle --impure --bundler github:ralismark/nix-appimage --expr '
  let
    na = builtins.getFlake "github:ralismark/nix-appimage";
    pkgs = import na.inputs.nixpkgs {
      system = builtins.currentSystem;
      overlays = [ na.overlays.systemLdConf ];
    };
  in pkgs.hello
'
```

To rely on host OpenGL drivers instead of bundling Nix’s, exclude the Mesa libs from the squashfs and let the loader fall back to system libraries. For example, when using the flake API:

```
let
  na = builtins.getFlake "github:ralismark/nix-appimage";
  pkgs = na.inputs.nixpkgs.legacyPackages.${builtins.currentSystem};
in na.outputs.lib.${builtins.currentSystem}.mkAppImage {
  program = "${pkgs.somePkg}/bin/app";
  squashfsArgs = [
    "-wildcards"
    "-e /nix/store/*mesa*/lib/libGL.so*"
    "-e /nix/store/*mesa*/lib/libEGL.so*"
  ];
}
```

The `systemLdConf` overlay (enabled by default in `./bundle`) keeps `ld.so.conf` including `/etc/ld.so.conf`, so the bundled app will search host driver paths after removing the Nix copies. Other `-e` patterns can be added as needed for different drivers.


## Caveats

OpenGL apps not being able to run on non-NixOS systems is a **known problem**, see https://github.com/NixOS/nixpkgs/issues/9415 and https://github.com/ralismark/nix-appimage/issues/5.

Additionally, we only make a best-effort attempt to copy in the relevant .desktop files and icons, so they may not be present.
This doesn't affect the running of bundled apps, but might cause issues with showing up correctly in application launchers (e.g. rofi).

The current implementation also has some limitations:

- This requires Linux User Namespaces (i.e. `CAP_SYS_USER_NS`), which are available since Linux 3.8 (released in 2013), but may not be enabled for security reasons.
- Plain files in the root directory aren't visible to the bundled app.

## Under The Hood

nix-appimage creates [type 2 AppImages](https://github.com/AppImage/AppImageSpec/blob/ce1910e6443357e3406a40d458f78ba3f34293b8/draft.md#type-2-image-format), which are essentially just a binary, known as the Runtime, concatenated with a squashfs file system.
When the AppImage is run, the runtime simply mounts the squashfs somewhere and runs the contained `AppRun`.
The squashfs contains all the files needed to run the program:

- `nix/store/...`, containing the closure of the bundled program
- `entrypoint`, a symlink to the actual executable, e.g. `/nix/store/q9cqc10sw293xpx3hca4qpsmbg7hsgzy-hello-2.12.1/bin/hello`
- `AppRun`, which gets started after the squashfs is mounted.
  This isn't the actual bundled executable, but a wrapper that makes the bundled nix/store file visible under /nix/store before executing `entrypoint`.

Runtimes are included within the flake as `packages.<system>.appimage-runtimes.<name>`.
Currently supported are:

- `appimage-type2-runtime` (default)
  This is [AppImage/type2-runtime](https://github.com/AppImage/type2-runtime), a static runtime maintained by the official AppImage team.
- `appimagecrafters`.
  This is [AppImageCrafers/appimage-runtime](https://github.com/AppImageCrafters/appimage-runtime), a similar static runtime that was the old default for nix-appimage.

AppRuns are included within the flake as `packages.<system>.appimage-appruns.<name>`.
Currently supported are:

- `userns-chroot` (default).
  This uses Linux User Namespaces and chroot to make /nix/store appear to have the bundled files, similar to [nix-user-chroot](https://github.com/nix-community/nix-user-chroot).
  There is a known problem of plain files in the root folder not being visible to the bundled app when using this AppRun.
