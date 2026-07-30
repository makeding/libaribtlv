{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  nativeBuildInputs = with pkgs; [
    cmake
    emscripten
    ninja
    nodejs
    pkg-config
    zlib
  ];
}
