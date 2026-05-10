# Firebase Studio (formerly Project IDX) dev environment.
#
# Mirrors the package list in flake.nix — IDX's dev.nix shape is
# adjacent to flakes but not identical (it's a function returning a
# specific record), so we duplicate the small package list rather
# than reaching into the flake. Both files are short enough that
# drift is obvious in code review.
#
# Caveat: nixpkgs's zig version may lag behind the 0.16.0 we pin in
# flake.nix. If IDX gives us an older zig and that breaks the build,
# add a fetchurl-based binary derivation here. Today: cross fingers
# and see what nixpkgs ships.

{ pkgs, ... }: {
  # Pin to a stable nixpkgs channel for reproducibility. Bump as
  # needed; the flake's nixpkgs input drives version expectations.
  channel = "stable-24.05";

  packages = [
    pkgs.zig          # 'zig cc' is the C compiler; version per nixpkgs channel
    pkgs.clang-tools  # clang-format for `make format`
    pkgs.gnumake
    pkgs.bash
  ];

  env = { };

  idx = {
    extensions = [
      # Add IDE extensions here as we identify ones that help. The
      # default IDX shell already gives us VS Code-style editing.
    ];
    workspace = {
      onCreate = {
        # Run on first workspace creation. Could pre-build the
        # binary so the first `make` is warm.
        # default-build = "make all";
      };
      onStart = { };
    };
  };
}
