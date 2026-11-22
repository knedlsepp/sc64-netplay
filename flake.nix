{
  description = "N64 Netplay: static web page (Trystero/WebRTC + SummerCart64 via Web Serial/WebUSB) and an N64 chat ROM";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    libdragon = {
      url = "github:DragonMinded/libdragon";
      flake = false;
    };
  };

  outputs = { self, nixpkgs, libdragon }:
    let
      # The prebuilt libdragon toolchain (GCC 16.2.0, mips64-elf) is available for Linux x86_64 and aarch64.
      # Note: upstream publishes it as a continuous pre-release under a fixed URL.
      # When it gets rebuilt the hash changes – update the hashes here
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
              # GDB needs Python 3.10 and friends and is not needed for building
              rm -f $out/bin/mips64-elf-gdb* $out/bin/mips64-elf-gstack
              runHook postInstall
            '';

            # Don't touch the MIPS libraries with the host strip
            dontStrip = true;

            meta = {
              description = "Prebuilt mips64-elf toolchain for libdragon";
              homepage = "https://github.com/DragonMinded/libdragon";
              platforms = systems;
              sourceProvenance = [ lib.sourceTypes.binaryNativeCode ];
            };
          };

          # libdragon installs itself into the same prefix as the toolchain (N64_INST),
          # so the toolchain is copied to $out here and libdragon is built on top.
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
              description = "libdragon SDK including the toolchain (N64_INST prefix)";
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

            meta.description = "N64 chat ROM for the SummerCart64";
          };

          # The static web page as served on GitHub Pages – with a freshly built chat ROM
          web = pkgs.runCommand "sc64-netplay-web" { } ''
            mkdir -p $out
            cp ${./index.html} $out/index.html
            cp ${chat-rom}/chat.z64 $out/chat.z64
            touch $out/.nojekyll
          '';

          # Local web server: Web Serial requires https:// or http://localhost
          serve = pkgs.writeShellApplication {
            name = "sc64-netplay-serve";
            runtimeInputs = [ pkgs.python3 ];
            text = ''
              port="''${PORT:-8000}"
              echo "N64 Netplay running at http://localhost:$port/  (Ctrl+C to stop)"
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
          meta.description = "Serve the web page locally at http://localhost:8000";
        };
      });

      devShells = forAllSystems (system: pkgs: {
        default = pkgs.mkShell {
          packages = [ self.packages.${system}.libdragon pkgs.gnumake pkgs.python3 ];
          N64_INST = "${self.packages.${system}.libdragon}";
          shellHook = ''
            echo "libdragon ready (N64_INST=$N64_INST). Build the ROM with: make -C rom"
          '';
        };
      });

      checks = forAllSystems (system: pkgs: {
        inherit (self.packages.${system}) chat-rom web;
      });
    };
}
