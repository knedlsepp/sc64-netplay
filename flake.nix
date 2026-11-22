{
  description = "N64 Netplay: statische Webseite (Trystero/WebRTC + SummerCart64 über Web Serial/WebUSB) und N64-Chat-ROM";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    libdragon = {
      url = "github:DragonMinded/libdragon";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, libdragon }:
    let
      # Der vorgebaute libdragon-Toolchain (GCC 16.2.0, mips64-elf) gibt es für Linux x86_64 und aarch64.
      # Achtung: Upstream veröffentlicht ihn als fortlaufende Vorabversion unter einer festen URL.
      # Wird er neu gebaut, ändert sich der Hash – dann hier die Hashes aktualisieren
      # (nix store prefetch-file <url>).
      toolchainDebs = {
        x86_64-linux = {
          url = "https://github.com/DragonMinded/libdragon/releases/download/toolchain-continuous-prerelease/gcc-toolchain-mips64-amd64.deb";
          hash = "sha256-+sjmVySTpmRot9Rd9BsEL5CxpjDslqohi9X7HzIKCk0=";
        };
        aarch64-linux = {
          url = "https://github.com/DragonMinded/libdragon/releases/download/toolchain-continuous-prerelease/gcc-toolchain-mips64-arm64.deb";
          hash = "sha256-JxpvEMiRsxak5HlZaYzH2ajaEmHKOqE+3qDILeIrxLQ=";
        };
      };

      systems = builtins.attrNames toolchainDebs;
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f system nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (system: pkgs:
        let
          inherit (pkgs) lib stdenv;

          n64-toolchain = stdenv.mkDerivation {
            pname = "libdragon-toolchain";
            version = "16.2.0-continuous";
            src = pkgs.fetchurl toolchainDebs.${system};

            nativeBuildInputs = [ pkgs.dpkg pkgs.autoPatchelfHook ];
            buildInputs = [ pkgs.zlib pkgs.zstd stdenv.cc.cc.lib ];

            unpackCmd = "dpkg-deb -x $curSrc source";
            sourceRoot = "source/opt/libdragon";

            installPhase = ''
              runHook preInstall
              cp -r . $out
              # GDB braucht Python 3.10 & Co. und wird zum Bauen nicht benötigt
              rm -f $out/bin/mips64-elf-gdb* $out/bin/mips64-elf-gstack
              runHook postInstall
            '';

            # Die MIPS-Bibliotheken nicht mit dem Host-strip anfassen
            dontStrip = true;

            meta = {
              description = "Vorgebauter mips64-elf-Toolchain für libdragon";
              homepage = "https://github.com/DragonMinded/libdragon";
              platforms = systems;
              sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
            };
          };

          # libdragon installiert sich in denselben Präfix wie der Toolchain (N64_INST),
          # darum wird der Toolchain hier nach $out kopiert und libdragon dazugebaut.
          libdragon-sdk = stdenv.mkDerivation {
            pname = "libdragon";
            version = "unstable-${libdragon.lastModifiedDate or "unknown"}";
            src = libdragon;

            dontConfigure = true;

            buildPhase = ''
              runHook preBuild
              cp -r ${n64-toolchain} $out
              chmod -R u+w $out
              export N64_INST=$out
              make install-mk
              make -j$NIX_BUILD_CORES libdragon tools
              runHook postBuild
            '';

            installPhase = ''
              runHook preInstall
              export N64_INST=$out
              make install tools-install
              runHook postInstall
            '';

            dontStrip = true;

            meta = {
              description = "libdragon-SDK samt Toolchain (N64_INST-Präfix)";
              homepage = "https://libdragon.dev";
              license = lib.licenses.unlicense;
              platforms = systems;
            };
          };

          chat-rom = stdenv.mkDerivation {
            pname = "n64-chat-rom";
            version = "0.1.0";
            src = ./rom;

            dontConfigure = true;
            makeFlags = [ "N64_INST=${libdragon-sdk}" ];
            env.N64_INST = "${libdragon-sdk}";

            installPhase = ''
              runHook preInstall
              install -Dm644 chat.z64 $out/chat.z64
              runHook postInstall
            '';

            dontStrip = true;
            dontFixup = true;

            meta.description = "N64-Chat-ROM für das SummerCart64";
          };

          # Die statische Webseite, wie sie auf GitHub Pages liegt – mit frisch gebauter Chat-ROM
          web = pkgs.runCommand "sc64-netplay-web" { } ''
            mkdir -p $out
            cp ${./index.html} $out/index.html
            cp ${chat-rom}/chat.z64 $out/chat.z64
            touch $out/.nojekyll
          '';

          # Lokaler Webserver: Web Serial braucht https:// oder http://localhost
          serve = pkgs.writeShellApplication {
            name = "sc64-netplay-serve";
            runtimeInputs = [ pkgs.python3 ];
            text = ''
              port="''${PORT:-8000}"
              echo "N64 Netplay läuft auf http://localhost:$port/  (Strg+C beendet)"
              exec python3 -m http.server "$port" --bind 127.0.0.1 --directory ${web}
            '';
          };
        in
        {
          inherit n64-toolchain chat-rom web serve;
          libdragon = libdragon-sdk;
          default = web;
        });

      apps = forAllSystems (system: pkgs: {
        default = {
          type = "app";
          program = nixpkgs.lib.getExe self.packages.${system}.serve;
          meta.description = "Webseite lokal auf http://localhost:8000 ausliefern";
        };
      });

      devShells = forAllSystems (system: pkgs: {
        default = pkgs.mkShell {
          packages = [ self.packages.${system}.libdragon pkgs.gnumake pkgs.python3 ];
          N64_INST = "${self.packages.${system}.libdragon}";
          shellHook = ''
            echo "libdragon bereit (N64_INST=$N64_INST). ROM bauen: make -C rom"
          '';
        };
      });

      checks = forAllSystems (system: pkgs: {
        inherit (self.packages.${system}) chat-rom web;
      });
    };
}
