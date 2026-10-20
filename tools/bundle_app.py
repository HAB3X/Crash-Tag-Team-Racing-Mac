#!/usr/bin/env python3
"""Makes the Mac app standalone (it runs on a Mac without Homebrew): a copy of the built app in build/dist with every
non-system library it loads (SDL3) copied into Contents/Frameworks, each one's install name @rpath/<name> and every reference
to it rewritten to that, the executable given the rpath @executable_path/../Frameworks, their packages' licence files in
Contents/Resources/licenses, and the whole bundle signed ad hoc (codesign -s -; Apple silicon won't run unsigned code). It then
checks that no library outside the system's is left in the bundle (otool -L) and that the signature verifies. --zip also writes
the app zipped in the same folder (ditto, keeping the signature): named as given, or by default the app's name with its spaces
as hyphens and -macOS.zip (Crash-Tag-Team-Racing-macOS.zip).

    python3 tools/bundle_app.py --app PATH [--name NAME] [--out DIR] [--zip [FILE.zip]]

From the Crash Twinsanity Mac port's native/tools/bundle_app.py (same author), less its FFmpeg and universal builds.
"""
import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build/dist"
SYSTEM = ("/usr/lib/", "/System/")


def run(*command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


def dependencies(binary):
    """The libraries a Mach-O loads (otool -L), its own install name left out."""
    lines = run("otool", "-L", str(binary)).splitlines()[1:]
    paths = [line.strip().split(" (compatibility")[0] for line in lines]
    own = run("otool", "-D", str(binary)).splitlines()[1:]
    return [p for p in paths if p not in own]


def resolve(reference, loader, rpaths):
    """A reference's file: absolute, @loader_path or @rpath (the loader's rpaths, then Homebrew's lib)."""
    if reference.startswith("@loader_path/"):
        return (Path(loader).parent / reference[len("@loader_path/"):]).resolve()
    if reference.startswith("@rpath/"):
        name = reference[len("@rpath/"):]
        for rpath in rpaths + ["/opt/homebrew/lib", "/usr/local/lib"]:
            candidate = Path(rpath.replace("@loader_path", str(Path(loader).parent))) / name
            if candidate.exists():
                return candidate.resolve()
        return None
    return Path(reference).resolve()


def rpaths_of(binary):
    text = run("otool", "-l", str(binary))
    return re.findall(r"cmd LC_RPATH\n\s+cmdsize \d+\n\s+path (\S+)", text)


def standalone(source, out, name=None, runtime=None):
    """A copy of the app in out with its libraries in Contents/Frameworks and their licences: the app, its executable,
    {name: library} and the licences' packages."""
    out.mkdir(parents=True, exist_ok=True)
    app = out / (f"{name}.app" if name else source.name)
    if app.exists():
        shutil.rmtree(app)
    shutil.copytree(source, app, symlinks=True)
    executable = app / "Contents/MacOS" / source.stem
    frameworks = app / "Contents/Frameworks"
    frameworks.mkdir(exist_ok=True)

    # Every non-system library reachable from the executable, copied once by its file name
    copied = {}
    origins = {}
    queue = [executable]
    while queue:
        binary = queue.pop()
        rpaths = rpaths_of(binary)
        for reference in dependencies(binary):
            if reference.startswith(SYSTEM):
                continue
            path = resolve(reference, binary, rpaths)
            if path is None or not path.exists():
                sys.exit(f"{binary.name}: can't find {reference}")
            name = Path(reference).name
            if name not in copied:
                target = frameworks / name
                shutil.copy2(path, target)
                target.chmod(0o755)
                copied[name] = target
                origins[name] = path
                queue.append(target)
            run("install_name_tool", "-change", reference, f"@rpath/{name}", str(binary))

    for name, target in copied.items():
        run("install_name_tool", "-id", f"@rpath/{name}", str(target))
        for rpath in rpaths_of(target):
            run("install_name_tool", "-delete_rpath", rpath, str(target))
        if "@loader_path" not in rpaths_of(target):
            run("install_name_tool", "-add_rpath", "@loader_path", str(target))
    for rpath in rpaths_of(executable):
        if not rpath.startswith("@"):
            run("install_name_tool", "-delete_rpath", rpath, str(executable))
    if "@executable_path/../Frameworks" not in rpaths_of(executable):
        run("install_name_tool", "-add_rpath", "@executable_path/../Frameworks", str(executable))

    # Each library's licence files (its Homebrew package's LICENSE*, COPYING*), in Contents/Resources/licenses/<package>
    licenses = app / "Contents/Resources/licenses"
    packages = set()
    for name, origin in origins.items():
        source_path = None
        for prefix in ("/opt/homebrew/lib", "/usr/local/lib"):
            candidate = Path(prefix) / name
            if candidate.exists():
                source_path = candidate.resolve()
        if source_path is None or "Cellar" not in source_path.parts:
            continue
        cellar = source_path.parts.index("Cellar")
        package_root = Path(*source_path.parts[:cellar + 3])
        packages.add(package_root.parts[cellar + 1])
        for licence in list(package_root.glob("LICENSE*")) + list(package_root.glob("COPYING*")) + list(package_root.glob("LICENCE*")):
            target = licenses / package_root.parts[cellar + 1] / licence.name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(licence, target)

    # The menus' fonts (built into the program) come with their licences: from the runtime the build used, or one in or next to
    # this repository
    places = [Path(runtime)] if runtime else []
    for runtime in places + [ROOT / "ps2-native-runtime", ROOT.parent / "ps2-native-runtime"]:
        fonts = runtime / "runtime/fonts"
        if fonts.is_dir():
            for licence in [*fonts.glob("*.txt"), fonts / "README.md"]:
                target = licenses / "fonts" / licence.name
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(licence, target)
            packages.add("fonts (Luckiest Guy, Fredoka)")
            break

    return app, executable, copied, packages


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True, help="the built app")
    parser.add_argument("--out", default=str(OUT))
    parser.add_argument("--name", help="the app's name in the release (its folder's)")
    parser.add_argument("--zip", nargs="?", const="", help="also zip the app (as this file name, or the app's name)")
    parser.add_argument("--runtime", help="the runtime the build used (its fonts' licences)")
    arguments = parser.parse_args()
    app, executable, copied, packages = standalone(Path(arguments.app), Path(arguments.out), arguments.name, arguments.runtime)

    # Signed ad hoc: the libraries first, then the bundle
    for target in copied.values():
        run("codesign", "--force", "--sign", "-", str(target))
    run("codesign", "--force", "--sign", "-", str(app))

    # Checks: nothing outside the system's left, the signature good
    bad = []
    for binary in [executable, *copied.values()]:
        for reference in dependencies(binary):
            if not reference.startswith(SYSTEM) and not reference.startswith("@rpath/"):
                bad.append(f"{binary.name}: {reference}")
    if bad:
        sys.exit("still outside the bundle:\n" + "\n".join(bad))
    run("codesign", "--verify", "--deep", "--strict", str(app))
    print(f"{app} ({run('lipo', '-archs', str(executable)).strip()}): {len(copied)} libraries in Contents/Frameworks: {', '.join(sorted(copied))}")
    print(f"licences of: {', '.join(sorted(packages))}")
    if arguments.zip is not None:
        archive = Path(arguments.out) / (arguments.zip or f"{app.stem.replace(' ', '-')}-macOS.zip")
        archive.unlink(missing_ok=True)
        run("ditto", "-c", "-k", "--keepParent", str(app), str(archive))
        print(archive)


if __name__ == "__main__":
    main()
