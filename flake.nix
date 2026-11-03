{
  description = "ESP32-S3 camera (ESP-IDF) and image upload server (Go)";

  inputs.esp-dev.url = "github:mirrexagon/nixpkgs-esp-dev";

  outputs =
    { self, esp-dev }:
    let
      system = "x86_64-linux";
      pkgs = esp-dev.inputs.nixpkgs.legacyPackages.${system};
    in
    {
      devShells.${system}.default = pkgs.mkShell {
        buildInputs = [
          esp-dev.packages.${system}.esp-idf-esp32s3
          pkgs.go
        ];
        shellHook = ''
          export ESPPORT=''${ESPPORT:-/dev/ttyUSB0}
          export ESPBAUD=''${ESPBAUD:-921600}
        '';
      };
    };
}
