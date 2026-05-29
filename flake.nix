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
