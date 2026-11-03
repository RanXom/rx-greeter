
{
  description = "rx-greeter development environment";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = nixpkgs.lib.genAttrs systems;
    in
    {
      devShells = forAllSystems (system:
        let
          pkgs = import nixpkgs {
            inherit system;
          };
        in
        {
          default = pkgs.mkShell {
            packages = with pkgs; [
              gcc
              clang
              cmake
              ninja
              pkg-config

              yyjson

              gdb
              valgrind
              clang-tools
            ];

            shellHook = ''
              echo "rx-greeter development environment"
              echo "compiler: $(cc --version | head -n1)"
              echo "cmake: $(cmake --version | head -n1)"
            '';
          };
        });
    };
}

