{
  description = "Ore compiler — dev shell + toolchain pin";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    flake-utils.url = "github:numtide/flake-utils";
  };

  outputs = { self, nixpkgs, flake-utils }:
    flake-utils.lib.eachDefaultSystem (system:
      let
        pkgs = import nixpkgs { inherit system; };
      in {
        devShells.default = pkgs.mkShell {
          # Single C toolchain for everything. clang_19 covers:
          #   - main build (`make all`, `make debug-queries`)
          #   - sanitizer smoke tests (`make test` with -fsanitize=address)
          # on both aarch64-darwin and x86_64-linux. clang's
          # libclang_rt ships the ASan runtime for both platforms.
          #
          # Pre-this-iteration we also pinned zig 0.16.0 via
          # mitchellh/zig-overlay. Removed because we never used
          # Zig's cross-compile capability and clang covers all our
          # actual needs (C23, ASan, UBSan, std warnings). Re-adding
          # Zig is a 10-minute change if cross-compilation becomes
          # interesting later.
          #
          # clang-tools is a separate nixpkgs package providing
          # clang-format (used by `make format`).
          packages = [
            pkgs.clang_19
            pkgs.clang-tools
            pkgs.gnumake
            pkgs.bash
            # cJSON for the LSP server's JSON-RPC parsing. Header
            # lives at $cjson/include/cjson/cJSON.h; link with -lcjson.
            # pkg-config metadata is shipped under the same package.
            pkgs.cjson
            pkgs.pkg-config
          ] ++ pkgs.lib.optionals pkgs.stdenv.isLinux [
            # Profiling tools (Linux only).
            #   valgrind  — leak detection + cachegrind/callgrind profiling.
            #   perf      — sampling profiler (uses /proc + kernel perf_event).
            #               linuxPackages.perf tracks the running kernel; on
            #               nix-on-non-NixOS the kernel may be older than the
            #               package, in which case fall back to system perf.
            pkgs.valgrind
            pkgs.linuxPackages.perf
          ];

          shellHook = ''
            echo "ore: clang $(clang --version | head -1 | awk '{print $NF}'), clang-format $(clang-format --version | head -1 | awk '{print $NF}'), cjson $(pkg-config --modversion libcjson 2>/dev/null || echo '?')"
          '';
        };
      });
}
