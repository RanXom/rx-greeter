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
  ];

  env.DEVSHELL_NAME = "rx-greeter";

  # ---------------------------------------------------------------------------
  # Add these when rx-greeter actually starts needing them, not before.
  #
  # packages = with pkgs; [
  #   json-c                                   # greetd IPC payloads
  #   wayland wayland-protocols libxkbcommon   # Wayland client + keymaps
  #   libGL mesa                               # rendering backend for Clay
  #   freetype harfbuzz                        # text rendering
  # ];
  #
  # Tip: if you later want a task instead of remembering commands:
  #   scripts.build.exec = "make";
  #   scripts.run.exec = "make run";
  # ---------------------------------------------------------------------------
}
