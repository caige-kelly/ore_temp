# Firebase Studio (formerly Project IDX) dev environment.
#
# Mirrors the package list in flake.nix — IDX's dev.nix shape is
# adjacent to flakes but not identical (it's a function returning a
# specific record), so we duplicate the small package list rather
# than reaching into the flake. Both files are short enough that
# drift is obvious in code review.

{ pkgs, ... }: {
  # Pin to a stable nixpkgs channel for reproducibility. Bump as
  # needed; the flake's nixpkgs input is on unstable so this may
  # lag.
  channel = "stable-24.11";

  packages = [
    pkgs.clang_19     # main + sanitizer C toolchain (clang's libclang_rt
                      # ships ASan for both darwin and linux)
    pkgs.clang-tools  # clang-format for `make format`
    pkgs.gnumake
    pkgs.bash
  ];

  env = { };

  idx = {
    extensions = [
      "llvm-vs-code-extensions.vscode-clangd"
      "google.gemini-cli-vscode-ide-companion"
    ];
    workspace = {
      onCreate = { };
      onStart = { };
    };
  };
}
