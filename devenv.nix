{
  pkgs,
  ...
}:

{
  # Use the host toolchain (Gentoo's gcc) rather than a Nix-provided compiler.
  # stdenvNoCC gives devenv the build plumbing it needs without pulling in gcc.
  stdenv = pkgs.stdenvNoCC;

  # Deliberately NOT using `languages.c.enable`. That module is just a bundle:
  #   pkgs.stdenv, clang-tools, ccls, gdb, gnumake, pkg-config, valgrind
  # Gentoo already provides gcc, clangd, clang-format, clang-tidy, make and
  # pkg-config, so only the genuinely missing tools are added here.
  packages = with pkgs; [
    gdb # no debugger installed system-wide (lldb is also absent)
    bear # wraps `make` and writes compile_commands.json for clangd
    lazygit # terminal UI for git

    # rx-greeter build dependencies. These are the libraries pkg-config and
    # wayland-scanner resolve inside the devenv; the final binary records
    # ordinary sonames and runs against the host's libraries outside Nix.
    pkg-config
    wayland
    wayland-scanner
    wayland-protocols
    libxkbcommon
    cairo
    json_c
    fontconfig

    # Test-only tools. Sway itself intentionally comes from the host: the
    # nested test needs the same wlroots/Sway build used by the live setup.
    grim
    python3
  ];

  env.DEVSHELL_NAME = "rx-greeter";

  scripts.build.exec = "make";
  scripts.check.exec = "make check";
  scripts.integration.exec = "make integration";
}
