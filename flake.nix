{
  description = "GNU Emacs fork with a native Wayland backend";

  # The pin sets the dependency versions used by the package and test
  # harness.  Update it deliberately.
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f:
        nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
      emacsVersion = let
        parts = nixpkgs.lib.splitString "AC_INIT([GNU Emacs], ["
          (builtins.readFile ./configure.ac);
      in builtins.head (nixpkgs.lib.splitString "]" (builtins.elemAt parts 1));
      revision = self.shortRev or self.dirtyShortRev or "source";
      source = builtins.path {
        path = self.outPath;
        name = "emacs-source";
        # Harness and flake edits do not change the Emacs source.
        filter = path: _type: !builtins.elem
          (nixpkgs.lib.removePrefix "${self.outPath}/" path) [
            "test/wayland" "flake.nix" "flake.lock"
          ];
      };
      emacsFor = pkgs: backend:
        let
          withPgtk = backend == "pgtk";
          withX = backend == "x";
          label = {
            wayland = "the native Wayland backend";
            pgtk = "PGTK";
            x = "X11 without a toolkit";
          }.${backend};
          emacs = pkgs.callPackage
            (import (nixpkgs.outPath + "/pkgs/applications/editors/emacs/make-emacs.nix") {
              pname = if withPgtk then "emacs" else "emacs-${backend}";
              version = "${emacsVersion}+${revision}";
              variant = "mainline";
              src = source;
              meta = {
                description = "GNU Emacs with ${label}";
                license = pkgs.lib.licenses.gpl3Plus;
                platforms = systems;
                mainProgram = "emacs";
              };
            }) {
              inherit (pkgs.darwin) sigtool;
              inherit withPgtk withX;
              toolkit = "no";
              withXwidgets = false;
              withCairo = true;
            };
          base = emacs.overrideAttrs (old: {
            configureFlags = old.configureFlags ++ pkgs.lib.optionals withX [
              "--without-libotf" "--without-m17n-flt"
            ];
          });
        in
        if backend != "wayland" then base else base.overrideAttrs (old: {
          nativeBuildInputs = old.nativeBuildInputs ++ [ pkgs.wayland-scanner ];
          buildInputs = old.buildInputs ++ (with pkgs; [
            wayland
            libxkbcommon
            pixman
            fontconfig
            freetype
            giflib
            libjpeg
            libpng
            librsvg
            libtiff
          ]);
          configureFlags = builtins.filter
            (flag: !builtins.elem flag [
              "--without-gif" "--without-jpeg" "--without-png" "--without-tiff"
            ]) old.configureFlags ++ [ "--with-wayland" ];
          postFixup = (old.postFixup or "") + ''
            wrapProgram "$out/bin/emacs" \
              --set-default LOCALE_ARCHIVE ${pkgs.glibcLocales}/lib/locale/locale-archive \
              --run 'if [ -z "''${FONTCONFIG_FILE+x}" ] && [ -r /etc/fonts/fonts.conf ]; then export FONTCONFIG_FILE=/etc/fonts/fonts.conf; fi'
          '';
        });
      emacsWayland = pkgs: emacsFor pkgs "wayland";
      testEnvironment = pkgs: {
        EMACS_WAYLAND_TEST_FONTS = "${pkgs.dejavu_fonts}/share/fonts/truetype";
        EMACS_WAYLAND_TEST_FCITX5 = "${pkgs.fcitx5}";
        EMACS_WAYLAND_TEST_FCITX5_HANGUL = "${pkgs.fcitx5-hangul}";
      };
      checkWayland = pkgs: withReference:
        let
          driftSources = pkgs.lib.fileset.toSource {
            root = ./src;
            fileset = pkgs.lib.fileset.fileFilter (file:
              file.name == "pgtkterm.c"
              || (pkgs.lib.hasPrefix "wayland" file.name && file.hasExt "c")
            ) ./src;
          };
          harness = pkgs.runCommandCC "emacs-wayland-harness" {
            nativeBuildInputs = with pkgs; [ gnumake pkg-config wayland-scanner ];
            buildInputs = with pkgs; [ wayland libxkbcommon cairo pixman ];
          } ''
            mkdir -p "$out/test" "$out/src"
            cp -r ${./test/wayland} "$out/test/wayland"
            cp -r ${./src/wayland-protocols} "$out/src/wayland-protocols"
            cp ${./src/xdisp.c} "$out/src/xdisp.c"
            cp ${driftSources}/*.c "$out/src/"
            chmod -R u+w "$out"
            make -C "$out/test/wayland/mockcomp"
            make -C "$out/test/wayland/observer"
          '';
        in pkgs.writeShellApplication {
          name = "check-wayland";
          runtimeInputs = with pkgs; [
            gnumake
            git # check-runner's build-check fixture
            sway
            grim
            (python3.withPackages (ps: [ ps.pillow ]))
            wl-clipboard
            wtype
            valgrind # check-instructions
          ];
          runtimeEnv = testEnvironment pkgs // {
            EMACS_WAYLAND_TEST_COMMIT = self.rev or self.dirtyRev or revision;
          };
          text = ''
            scratch=$(mktemp -d)
            trap 'rm -rf -- "$scratch"' EXIT
            export EMACS_WAYLAND_TEST_OUTPUT
            : "''${EMACS_WAYLAND_TEST_OUTPUT:=$(mktemp -d -t emacs-wayland-results.XXXXXX)}"
            cp -a ${harness}/test "$scratch/test"
            cp -a ${harness}/src "$scratch/src"
            chmod -R u+w "$scratch"
            mkdir -p "$scratch"/{wayland,pgtk}/{src,lib-src}
            ln -s ${emacsWayland pkgs}/bin/emacs "$scratch/wayland/src/emacs"
            ln -s ${emacsWayland pkgs}/bin/emacsclient "$scratch/wayland/lib-src/emacsclient"
            ln -s ${emacsFor pkgs "pgtk"}/bin/emacs "$scratch/pgtk/src/emacs"
            ${pkgs.lib.optionalString withReference ''
              mkdir -p "$scratch/x/src"
              ln -s ${emacsFor pkgs "x"}/bin/emacs "$scratch/x/src/emacs"
              export X_BUILD_DIR="$scratch/x"
            ''}
            if [ "$#" -eq 0 ]; then
              set -- ${if withReference then "check-perf" else "check-all"}
            fi
            make -C "$scratch/test/wayland" BUILD_DIR="$scratch/wayland" \
              PGTK_BUILD_DIR="$scratch/pgtk" "$@"
          '';
        };
    in
    {
      overlays.default = final: _prev: {
        emacs-wayland = emacsWayland final;
      };

      packages = forAllSystems (pkgs: let package = emacsWayland pkgs; in {
        emacs-wayland = package;
        emacs-x11-reference = emacsFor pkgs "x";
        default = package;
      });

      apps = forAllSystems (pkgs: {
        check-wayland = {
          type = "app";
          program = "${checkWayland pkgs false}/bin/check-wayland";
          meta.description = "Run the native Wayland functional suites";
        };
        check-wayland-perf = {
          type = "app";
          program = "${checkWayland pkgs true}/bin/check-wayland";
          meta.description = "Compare the three matched Emacs builds";
        };
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell (testEnvironment pkgs // {
          # Build inputs of nixpkgs' own X and PGTK Emacs packages, so the
          # baseline configurations in the build matrix also build here.
          inputsFrom = [ pkgs.emacs pkgs.emacs-pgtk ];

          packages = with pkgs; [
            # Building from a git checkout.
            autoconf
            automake
            texinfo
            pkg-config
            ccache

            # The native Wayland backend.
            wayland
            wayland-scanner
            wayland-protocols
            libxkbcommon
            pixman

            # Test harness.
            sway
            grim
            (python3.withPackages (ps: [ ps.pillow ]))
            wl-clipboard
            wtype
            # The input-method matrix: fcitx5 with its Hangul engine.
            fcitx5
            fcitx5-hangul
            # Memcheck runs; configure finds its headers and enables
            # USE_VALGRIND, so the GC's stack scan is not reported.
            valgrind
          ];

          # Recompiling unchanged files is nearly free with ccache, across
          # build directories and after a reconfigure.  configure picks up CC.
          shellHook = ''
            export CC="ccache gcc"
          '';
        });
      });
    };
}
