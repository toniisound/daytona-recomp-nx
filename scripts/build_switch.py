#!/usr/bin/env python3
"""Cross-compile the Nintendo Switch frontend using already generated host game code.

First run the existing setup/recompile pipeline with your own daytona93 ROMs
(python3 scripts/setup.py). This command never runs cross-built importers,
copies ROMs, or packages ROM data. --compile-check builds objects without
ROMs and does not make an NRO.

Requires devkitPro with: switch-dev switch-sdl2 (and devkitpro-pkgbuild-helpers).
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(command, env):
    print("+ " + " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), cwd=ROOT, env=env, check=True)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--set", default="daytona93", choices=["daytona93", "daytona"],
                    help="ROM set the host build was recompiled from (daytona = Revision A 1994)")
    ap.add_argument("--host-build-dir", type=Path, help="default: build (daytona93) or build-daytona (daytona)")
    ap.add_argument("--build-dir", type=Path, help="default: build/switch or build/switch-daytona")
    ap.add_argument("--devkitpro", type=Path, default=os.environ.get("DEVKITPRO", "/opt/devkitpro"))
    ap.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 8))
    ap.add_argument("--icon", type=Path, help="256x256 JPEG for the Homebrew Menu (default: platform/switch/icon.jpg)")
    ap.add_argument("--compile-check", action="store_true")
    ap.add_argument("--diagnostics", action="store_true",
                    help="log startup stages and performance to the SD card (errors are always logged)")
    ap.add_argument("--reference-renderer", action="store_true", help="disable OPT03 renderer changes for comparison")
    args = ap.parse_args(argv)
    if args.jobs < 1:
        ap.error("--jobs must be positive")
    if args.host_build_dir is None:
        args.host_build_dir = Path("build" if args.set == "daytona93" else "build-daytona")
    if args.build_dir is None:
        args.build_dir = Path("build/switch" if args.set == "daytona93" else "build/switch-daytona")
    required = (f"{args.set}/gen_table.cpp", f"{args.set}_tgp/tgp_gen.cpp", f"{args.set}_snd/snd_gen.cpp")
    dkp = args.devkitpro.expanduser().resolve()
    toolchain = dkp / "cmake/Switch.cmake"
    if not toolchain.is_file():
        ap.error(f"missing devkitPro Switch toolchain: {toolchain} (install switch-dev)")
    host = (ROOT / args.host_build_dir).resolve()
    build = (ROOT / args.build_dir).resolve()
    gen = host / "gen"
    if build == host or build == ROOT or build == gen or gen in build.parents:
        ap.error("use a separate cross-build directory, not the host build or its generated sources")
    if not args.compile_check:
        missing = [str(gen / name) for name in required if not (gen / name).is_file()]
        if missing:
            ap.error("missing host-generated code:\n" + "\n".join(missing) +
                     f"\nRun python3 scripts/recompile.py --set {args.set} --build-dir {args.host_build_dir} first.")
    env = os.environ.copy()
    env["DEVKITPRO"] = str(dkp)
    env["PATH"] = os.pathsep.join([str(dkp / "devkitA64/bin"), str(dkp / "tools/bin"), env.get("PATH", "")])
    configure = ["cmake", "-S", ROOT / "platform/switch", "-B", build,
                 f"-DCMAKE_TOOLCHAIN_FILE={toolchain}", "-DCMAKE_BUILD_TYPE=Release",
                 f"-DDAYTONA_GEN_ROOT={gen}", f"-DDAYTONA_SWITCH_ROMSET={args.set}",
                 f"-DDAYTONA_SWITCH_RENDER_OPT={'OFF' if args.reference_renderer else 'ON'}",
                 f"-DDAYTONA_SWITCH_DIAGNOSTICS={'ON' if args.diagnostics else 'OFF'}",
                 f"-DDAYTONA_SWITCH_COMPILE_CHECK={'ON' if args.compile_check else 'OFF'}"]
    if args.icon:
        configure.append(f"-DDAYTONA_SWITCH_ICON={args.icon.expanduser().resolve()}")
    run(configure, env)
    run(["cmake", "--build", build, "--parallel", args.jobs], env)
    if args.compile_check:
        print("Compile check complete. No linked game or NRO was built.")
    else:
        package = build / "daytona_switch.nro"
        if not package.is_file():
            raise RuntimeError(f"build completed without the expected package: {package}")
        print(f"NRO: {package}\nCopy it to sdmc:/switch/{args.set}/ together with {args.set}.zip")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.CalledProcessError, RuntimeError) as error:
        print(f"build_switch: {error}", file=sys.stderr)
        sys.exit(1)
