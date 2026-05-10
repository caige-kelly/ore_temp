{
  description = "Ore compiler — dev shell + toolchain pin";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";

    # Zig version pinning. Mitchell Hashimoto's overlay tracks
    # ziglang.org releases byte-for-byte; we pick a specific tag below
    # rather than following nixpkgs's zig (which lags behind upstream).
    zig-overlay = {
      url = "github:mitchellh/zig-overlay";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { self, nixpkgs, flake-utils, zig-overlay }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };

        # Pinned Zig: 0.16.0. Update this string + flake.lock to bump.
        # The same version resolves on aarch64-darwin / x86_64-darwin /
        # aarch64-linux / x86_64-linux — zig-overlay vendors prebuilt
        # tarballs from ziglang.org for each platform.
        zig = zig-overlay.packages.${system}."0.16.0";
      in {
        devShells.default = pkgs.mkShell {
          # Toolchain. We don't need clang directly — `zig cc` is the
          # C compiler. clang-tools is here for `clang-format` only,
          # used by `make format`.
          packages = [
            zig
            pkgs.clang-tools  # clang-format
            pkgs.gnumake
            pkgs.bash
          ];

          # Force CC to the pinned `zig cc` rather than letting Nix's
          # mkShell-injected `clang` win. Without this, the Makefile's
          # `ifeq ($(origin CC),default)` guard sees CC as
          # "environment" (not "default"), so the `zig cc` fallback
          # never fires — defeating the version pin.
          #
          # ASan: `zig cc -fsanitize=address` works uniformly across
          # platforms because Zig bundles its own runtime — no
          # platform-specific NIX_LDFLAGS injection needed in this
          # shell. The Makefile's NIX_LDFLAGS hook still works for
          # any future env that does need it.
          shellHook = ''
            export CC="zig cc"
            echo "ore: zig $(zig version), clang-format $(clang-format --version | head -1 | awk '{print $NF}')"
          '';
        };
      });
}
