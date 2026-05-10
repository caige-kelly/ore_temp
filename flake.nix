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
          # Two C toolchains in scope:
          #
          #   zig (zig cc)  — primary compiler for `make all` /
          #     `make debug-queries`. Picked because it cross-compiles
          #     trivially and bundles its own libc, which keeps the
          #     main build identical across systems.
          #
          #   clang_19      — secondary compiler used only for the
          #     sanitizer smoke tests (`make test`). zig cc's bundled
          #     compiler-rt is missing some macOS ASan symbols
          #     (`___asan_version_mismatch_check_v8`, B22), so we
          #     route ASan-instrumented builds through real clang
          #     instead. Works portably: clang_19 ships
          #     libclang_rt.asan_{osx_dynamic,x86_64,aarch64}.so for
          #     both darwin and linux.
          #
          # clang-tools is here for `clang-format` (used by `make
          # format`) — separate package from clang_19 in nixpkgs.
          packages = [
            zig
            pkgs.clang_19
            pkgs.clang-tools
            pkgs.gnumake
            pkgs.bash
          ];

          shellHook = ''
            # Force CC to the pinned `zig cc` rather than letting Nix's
            # mkShell-injected `clang` win. Without this, the Makefile's
            # `ifeq ($(origin CC),default)` guard sees CC as
            # "environment" (not "default"), so the `zig cc` fallback
            # never fires — defeating the version pin.
            export CC="zig cc"

            # TEST_CC is the C compiler used by `make test` for the
            # ASan smoke-test builds. Routed to real clang because zig
            # cc is broken for sanitizer builds on macOS (B22). Same
            # binary works on both platforms.
            export TEST_CC="clang"

            echo "ore: zig $(zig version), clang-format $(clang-format --version | head -1 | awk '{print $NF}')"
          '';
        };
      });
}
