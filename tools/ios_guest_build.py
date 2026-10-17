"""Build the iOS ILP32 game image for embedding in the native app.

The portable guest runtime uses 32-bit game pointers. Its ARM64 instructions
are lifted into the iOS address arena; no Android SDK, NDK, or runtime is used.
"""

import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, List

from .embed_assets import hud_assets_build
from .linux_build import (LINUX_PROFILE, MUSL_MATH_DIR, STB_VORBIS_SOURCE, XDK_INCLUDE, compile_launcher,
                          game_defines_and_includes,
                          game_sources, lto_mode, musl_math_cflags, musl_math_sources, pgo_mode, pgo_profile, profile_use_flags,
                          xdk_headers)
from .ninja_syntax import Writer

PORT_DIR = Path("port/runtime")
LINUX_DIR = Path("port/linux")
BUILD = Path("build/ios")
THIRD_PARTY = Path("build/third_party")
# the TOML parser config.toml is read with (port/linux/src/port_config.c)
TOML_DIR = Path("port/third_party/tomlc17")
# the menus' XML parser (port/linux/src/menu_files.c)
EXPAT_DIR = Path("port/third_party/expat")
EXPAT_SOURCES = ("xmlparse.c", "xmlrole.c", "xmltok.c")
KCP_DIR = Path("port/third_party/kcp")
# internet play's signatures, for public games' listings (port/linux/src/p2p_crypto.c)
MONOCYPHER_DIR = Path("port/third_party/monocypher")
# the port's zlib (port/third_party/zlib/zlib_prefixed.h), which inflates the
# HUD's PNGs (hud_hires.c), as upstream's Linux and Windows builds
ZLIB_DIR = Path("port/third_party/zlib")
ZLIB_SOURCES = ("adler32.c", "crc32.c", "inffast.c", "inflate.c", "inftrees.c", "uncompr.c", "zutil.c")
# (its names prefixed, and the one Z_PREFIX leaves, its error messages, which
# the game's zlib names the same)
ZLIB_DEFINES = ("-DZ_PREFIX", "-Dz_errmsg=z_port_errmsg")
MUSL_VERSION = "1.2.5"
MUSL_DIR = THIRD_PARTY / f"musl-{MUSL_VERSION}"
MUSL_URL = f"https://musl.libc.org/releases/musl-{MUSL_VERSION}.tar.gz"
SDL_TAG = "release-3.4.16"
SDL_DIR = THIRD_PARTY / "SDL3"
SDL_URL = "https://github.com/libsdl-org/SDL.git"

# The guest ABI: AArch64 code with 32-bit pointers (clang's only such target
# is Apple's arm64_32, whose Mach-O output is converted afterwards). The
# Darwin environment is hidden from the sources; the C library is musl.
GUEST_ABI_FLAGS = [
    "--target=arm64_32-apple-watchos",
    "-U__APPLE__",
    "-U__MACH__",
    "-fno-define-target-os-macros",
    "-D__linux__=1",
    "-D__unix__=1",
    "-DHALO_ILP32=1",
    # Baseline ARMv8 instructions; Darwin otherwise enables newer features.
    "-mcpu=cortex-a53",
    "-nostdinc",
    "-fshort-wchar",
    "-fno-stack-protector",
    "-fno-unwind-tables",
    "-fno-asynchronous-unwind-tables",
    "-femulated-tls",
    "-mllvm",
    "-aarch64-neon-syntax=generic",
    # no fused multiply-add: the game was written for x87/SSE arithmetic,
    # and its debug assertions (colours within 0..1, unit vectors) trip on
    # the different rounding of fused operations
    "-ffp-contract=off",
    "-O2",
]

# as the Linux build (tools/linux_build.py), minus what only x86 needs
GUEST_CODE_FLAGS = [
    "-fms-extensions",
    "-fcommon",
    "-fno-strict-aliasing",
    "-fwrapv",
    "-fno-delete-null-pointer-checks",
    "-fno-omit-frame-pointer",
    *(f"-fno-builtin-{name}" for name in (
        "wcslen", "wcsnlen", "wcschr", "wcsrchr", "wcscmp", "wcsncmp", "wcscpy",
        "wcsncpy", "wcscat", "wcsncat", "wmemchr", "wmemcmp", "wmemcpy",
        "wmemmove", "wmemset",
    )),
]

MUSL_DIRECTORIES = [
    "conf", "ctype", "dirent", "env", "errno", "exit", "fcntl", "internal",
    "locale", "malloc", "malloc/mallocng", "math", "mman", "multibyte",
    "prng", "sched", "select", "signal", "stat", "stdio", "stdlib", "string",
    "time", "unistd",
]
MUSL_FILES = [
    "thread/__lock.c", "thread/__wait.c", "thread/__timedwait.c",
    "thread/__syscall_cp.c", "thread/vmlock.c", "thread/pthread_self.c",
    "thread/pthread_equal.c", "thread/pthread_once.c",
    "thread/pthread_setcancelstate.c", "thread/pthread_testcancel.c",
    "thread/default_attr.c", "thread/lock_ptc.c", "misc/getauxval.c",
    "linux/sysinfo.c", "misc/basename.c", "misc/dirname.c",
    "misc/realpath.c", "misc/uname.c", "misc/ioctl.c", "misc/getrlimit.c",
    "misc/syscall.c", "network/htonl.c", "network/htons.c", "network/ntohl.c",
    "network/ntohs.c", "network/inet_addr.c", "network/inet_aton.c",
    "network/inet_ntoa.c", "network/inet_pton.c", "network/inet_ntop.c",
]
MUSL_THREAD_PREFIXES = (
    "pthread_attr_", "pthread_cond", "pthread_mutex", "pthread_rwlock",
    "pthread_spin", "sem_",
)
# replaced by the guest runtime (port/runtime/guest/runtime)
MUSL_EXCLUDE = {
    "env/__stack_chk.c", "env/__init_tls.c", "env/__libc_start_main.c",
    "env/__reset_tls.c", "malloc/oldmalloc", "thread/pthread_create.c",
    # unused, and its compiler barrier is an inline assembly statement
    "string/explicit_bzero.c",
    # a byte at a time: guest_string.c compares 8 bytes at a time
    "string/memcmp.c",
}
# game files that call variadic functions without a prototype in scope, which
# only works under x86's calling convention (tools/guest_abi_check.py)
VARIADIC_PROTOTYPE_FILES = {
    "source/ai/action_uncover.c", "source/ai/ai.c", "source/ai/ai_debug.c",
    "source/bungie_net/common/public_key_crypt.c", "source/camera/editor_flying_camera.c",
    "source/game/cheats.c", "source/game/game_engine.c", "source/game/players.c",
    "source/hs/hs.c", "source/interface/hud_nav_points.c",
    "source/networking/telnet_console.c", "source/rasterizer/xbox/rasterizer_xbox_errors.c",
    "source/render/render.c",
}


def _quote(path: Any) -> str:
    text = str(path).replace(os.sep, "/")
    return f'"{text}"' if " " in text else text


def fetch_third_party() -> None:
    """Download musl and SDL3 (configure time, once)."""
    THIRD_PARTY.mkdir(parents=True, exist_ok=True)
    if not MUSL_DIR.is_dir():
        print(f"Downloading {MUSL_URL}")
        archive = THIRD_PARTY / f"musl-{MUSL_VERSION}.tar.gz"
        subprocess.run(["curl", "-sSfL", "-o", str(archive), MUSL_URL], check=True)
        subprocess.run(["tar", "xzf", archive.name], cwd=THIRD_PARTY, check=True)
        archive.unlink()
    if not SDL_DIR.is_dir():
        print(f"Cloning SDL3 {SDL_TAG}")
        subprocess.run(["git", "clone", "-q", "--depth", "1", "--branch", SDL_TAG, SDL_URL, str(SDL_DIR)],
                       check=True)


def _musl_sources() -> List[Path]:
    src = MUSL_DIR / "src"
    result = set()
    for directory in MUSL_DIRECTORIES:
        for path in (src / directory).glob("*.c"):
            result.add(path)
    for name in MUSL_FILES:
        result.add(src / name)
    for path in (src / "thread").glob("*.c"):
        if path.name.startswith(MUSL_THREAD_PREFIXES):
            result.add(path)
    sources = []
    for path in sorted(result):
        relative = path.relative_to(src).as_posix()
        if relative in MUSL_EXCLUDE or any(relative.startswith(e + "/") for e in MUSL_EXCLUDE):
            continue
        sources.append(path)
    return sources


def ios_guest_configure_inputs() -> List[Path]:
    return [Path(__file__), PORT_DIR / "guest" / "runtime", Path("port/ios/host"), LINUX_DIR / "src"]


def stable_converter_interpreter(executable: str) -> Path:
    """Resolve a venv executable to its base interpreter for the stdlib-only converter."""
    return Path(executable).resolve()


def generate_ios_guest_build(n: Writer, sln: Any) -> None:
    if not getattr(sln, "ios", False):
        return
    config_path = LINUX_DIR / "port.json"
    if not config_path.is_file():
        return
    try:
        fetch_third_party()
    except (subprocess.CalledProcessError, OSError) as error:
        raise RuntimeError(f"Cannot fetch musl/SDL3: {error}") from error
    import json
    config: Dict[str, Any] = json.loads(config_path.read_text(encoding="utf-8"))

    toolchain = Path(sln.ios_llvm)
    guest_cc = str(toolchain / "bin/clang")
    converter = "tools/ios_asm_convert.py"

    guest_dir = BUILD / "guest"
    obj_dir = guest_dir / "obj"
    gen_dir = guest_dir / "gen"
    libc_include = guest_dir / "libc_include"
    libc_internal = guest_dir / "libc_internal"
    arch = PORT_DIR / "guest" / "libc" / "arch" / "arm64_32"
    semantics_header = Path("build/linux/halo_msvc_semantics.h")
    platform_semantics_header = Path("build/linux/platform_msvc_semantics.h")
    prefix_header = LINUX_DIR / "include" / "halo_linux_prefix.h"
    image = BUILD / "halo_guest.elf"
    python = "$python"

    n.comment("iOS guest (ninja ios_guest); see port/ios/README.md")
    n.variable("guest_cc", guest_cc)
    n.variable("guest_tool_bin", str(toolchain / "bin"))
    n.variable("guest_linker", sln.ios_lld)
    # The assembly converter uses only the standard library. Resolve uv's temporary venv symlink so a new
    # environment does not change every guest C compile command in Ninja's build log.
    n.variable("python_converter", f'"{stable_converter_interpreter(sys.executable)}"')

    # ---------- generated headers and sources

    alltypes = libc_include / "bits" / "alltypes.h"
    syscall_h = libc_include / "bits" / "syscall.h"
    version_h = libc_internal / "version.h"
    n.rule(
        name="guest_alltypes",
        command=f"sed -f {MUSL_DIR}/tools/mkalltypes.sed $in > $out",
        description="IOS MUSL $out",
    )
    n.build(outputs=alltypes, rule="guest_alltypes",
            inputs=[arch / "bits" / "alltypes.h.in", MUSL_DIR / "include" / "alltypes.h.in"])
    n.rule(
        name="guest_syscall_h",
        command="cp $in $out && sed -n -e s/__NR_/SYS_/p < $in >> $out",
        description="IOS MUSL $out",
    )
    n.build(outputs=syscall_h, rule="guest_syscall_h", inputs=arch / "bits" / "syscall.h.in")
    n.rule(
        name="guest_version_h",
        command=f"echo '#define VERSION \"{MUSL_VERSION}\"' > $out",
        description="IOS MUSL $out",
    )
    n.build(outputs=version_h, rule="guest_version_h")

    guest_posix_c = gen_dir / "guest_posix.c"
    posix_imports = gen_dir / "posix_imports.list"
    n.rule(
        name="guest_posix_stubs",
        command=f"{python} tools/guest_posix_stubs.py {LINUX_DIR}/src/posix.h {guest_posix_c} {posix_imports}",
        description="IOS POSIX STUBS",
    )
    n.build(outputs=[guest_posix_c, posix_imports], rule="guest_posix_stubs",
            implicit=[Path("tools/guest_posix_stubs.py"), LINUX_DIR / "src" / "posix.h"])

    imports_s = gen_dir / "imports.s"
    host_table_c = BUILD / "host" / "host_import_table.c"
    host_imports_list = PORT_DIR / "host_imports.list"
    n.rule(
        name="guest_imports",
        command=f"{python} tools/guest_imports.py --host-table {host_table_c} {imports_s} $in",
        description="IOS IMPORTS",
    )
    # render validation's two imports exist only in a build configured with it (gpu_validate.h)
    validation = bool(getattr(sln, "render_validation", False))
    n.build(outputs=[imports_s, host_table_c], rule="guest_imports",
            inputs=[host_imports_list, posix_imports] + ([PORT_DIR / "host_imports_validation.list"] if validation else []),
            implicit=[Path("tools/guest_imports.py")])

    generated_headers = [*xdk_headers(), alltypes, syscall_h, version_h,
                         semantics_header, platform_semantics_header]

    # ---------- guest compilation: C -> Darwin assembly -> ELF assembly -> object

    n.rule(
        name="guest_cc",
        command=(f"{compile_launcher(sln)}$guest_cc -MMD -MF $out.d $cflags -S $in -o $out.darwin.s && "
                 f"$python_converter {converter} $out.darwin.s $out.s && "
                 f"$guest_cc --target=aarch64-none-elf -c $out.s -o $out"),
        description="IOS CC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="guest_bc",
        command=f"{compile_launcher(sln)}$guest_cc -MMD -MF $out.d $cflags -flto=full -c $in -o $out",
        description="IOS BC $out",
        depfile="$out.d",
        deps="gcc",
    )
    n.rule(
        name="guest_as",
        command="$guest_cc --target=aarch64-none-elf -c $in -o $out",
        description="IOS AS $out",
    )

    libc_includes = [
        f"-isystem {libc_include}", f"-isystem {arch}", f"-isystem {MUSL_DIR}/arch/generic",
        f"-isystem {MUSL_DIR}/include",
    ]
    # Link-time optimization (--lto=full; thin is taken as full): the game,
    # the platform layer and the bundled libraries are compiled to bitcode
    # and optimized as one module (tools/ios_guest_lto.py). The C library,
    # the guest runtime and the import table stay outside it.
    lto = lto_mode(sln) != "off"
    # profile-guided optimisation with the Linux build's profile (committed,
    # or trained by the Linux build with --pgo=train): the game and platform
    # code are the same, and functions that differ simply go without
    profile = pgo_profile(sln, LINUX_PROFILE if pgo_mode(sln) == "train" else None, [LINUX_PROFILE], guest_cc)
    profile_flags = " ".join(profile_use_flags(profile))
    # The converter turns each memory access and indirect branch into three
    # instructions (tools/ios_asm_convert.py), so a conditional branch that
    # LLVM placed within reach can end up out of it in the larger functions
    # that a profile or inlining across units makes ("fixup value out of
    # range"). LLVM then relaxes those branches at a quarter of their reach
    # instead. With neither, the flags stay out, so a --pgo=off --lto=off
    # build is the code it always was.
    branch_ranges = ["-mllvm", "-aarch64-tbz-offset-bits=12", "-mllvm", "-aarch64-cbz-offset-bits=17",
                     "-mllvm", "-aarch64-bcc-offset-bits=17"] if lto or profile else []
    guest_abi = " ".join(GUEST_ABI_FLAGS + ["-DHALO_IOS=1", "-ffixed-x15", "-ffixed-x27", "-mllvm", "-aarch64-enable-compress-jump-tables=false"] + branch_ranges + (["-DHALO_RELEASE"] if getattr(sln, "port_release", False) else []) + (["-DHALO_RENDER_VALIDATION=1"] if validation else []))
    guest_code = " ".join(GUEST_CODE_FLAGS)
    tool_implicit = [Path(converter), Path("tools/guest_asm_convert.py"), *generated_headers]
    bitcode_implicit = [*generated_headers]
    if profile:
        tool_implicit.append(profile)
        bitcode_implicit.append(profile)

    def guest_object(source: Path, cflags: str, prefix: str = "", whole: bool = False) -> Path:
        """the unit's object; with link-time optimization, a unit that is part
        of the whole (whole) is compiled to bitcode (.bc) instead"""
        obj = obj_dir / prefix / Path(str(source).lstrip("/")).with_suffix(".o")
        if str(source).startswith(str(BUILD)):
            obj = obj_dir / prefix / source.relative_to(BUILD).with_suffix(".o")
        if lto and whole:
            obj = obj.with_suffix(".bc")
            n.build(outputs=obj, rule="guest_bc", inputs=source, implicit=bitcode_implicit,
                    variables={"cflags": cflags})
            return obj
        n.build(outputs=obj, rule="guest_cc", inputs=source, implicit=tool_implicit,
                variables={"cflags": cflags})
        return obj

    # musl
    musl_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-w",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{PORT_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    musl_objects = [guest_object(source, musl_cflags, "musl") for source in _musl_sources()]
    libguestc = guest_dir / "libguestc.a"
    n.rule(
        name="guest_ar",
        command="rm -f $out && $guest_tool_bin/llvm-ar rcs $out @$out.rsp",
        description="IOS AR $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=libguestc, rule="guest_ar", inputs=musl_objects)

    # the game
    objects: List[Path] = []
    game_flags = [
        "-std=gnu89", "-D__STRICT_ANSI__", "-w",
        "-Wno-error=incompatible-pointer-types",
        "-Wno-error=incompatible-function-pointer-types",
        "-Wno-error=int-conversion",
        "-Wno-error=implicit-function-declaration",
        "-Wno-error=implicit-int",
        "-Wno-error=return-type",
    ]
    game_cflags = " ".join([
        guest_abi, guest_code, " ".join(game_flags), profile_flags,
        f"-include {prefix_header}", f"-include {semantics_header}",
        f"-I{LINUX_DIR}/include",
        # the headers of the port's own game units (port/linux/game), for the
        # game sources that call them, as upstream's Linux and Android builds
        f"-iquote {Path(config['game_sources'])}",
        game_defines_and_includes(config), *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    for source in game_sources(config):
        cflags = game_cflags
        if source.as_posix() in VARIADIC_PROTOTYPE_FILES:
            cflags += f" -include {PORT_DIR}/include/halo_guest_variadic_prototypes.h"
        objects.append(guest_object(source, cflags, whole=True))
    for source in sorted(Path(config["game_sources"]).glob("*.c")):
        objects.append(guest_object(source, game_cflags, whole=True))
    # (the Custom Edition maps' Ogg Vorbis decoder, as upstream's builds)
    objects.append(guest_object(STB_VORBIS_SOURCE, game_cflags, whole=True))

    # the platform layer shared with Linux, and the guest runtime
    platform_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE", "-DHALO_LINUX_PLATFORM_LAYER", "-w", profile_flags,
        f"-include {prefix_header}", f"-include {platform_semantics_header}",
        f"-I{LINUX_DIR}/src", f"-I{LINUX_DIR}/include", f"-I{PORT_DIR}/guest/runtime",
        f"-I{PORT_DIR}/include", f"-I{TOML_DIR}", f"-I{EXPAT_DIR}", f"-I{KCP_DIR}", f"-I{MONOCYPHER_DIR}",
        f"-I{ZLIB_DIR}", "-Isource -Isource/cseries",
        f"-I{SDL_DIR}/include", *libc_includes, f"-idirafter {XDK_INCLUDE}",
    ])
    # memory_watch.c is replaced by guest_memory_watch.c and bink_null.c by guest_bink.c; the GPU
    # backend runs in the host (step 4)
    guest_host_only = {"memory_watch.c", "bink_null.c", "gpu_gl.c", "gl_functions.c"}
    for source in sorted((LINUX_DIR / "src").glob("*.c")):
        if source.name.startswith("posix_") or source.name in guest_host_only:
            continue
        objects.append(guest_object(source, platform_cflags, whole=True))
    # the high-res HUD's, text's and titles' assets (port/assets; hud_hires.c, text_hires.c)
    for source in hud_assets_build(n, "ios", obj_dir / "gen" / "hud_hires_assets.c"):
        objects.append(guest_object(source, platform_cflags, whole=True))
    # the settings file's parser (port/third_party/tomlc17)
    objects.append(guest_object(TOML_DIR / "tomlc17.c", platform_cflags, whole=True))
    # the menus' XML parser (port/third_party/expat; menu_files.c)
    for name in EXPAT_SOURCES:
        objects.append(guest_object(EXPAT_DIR / name, platform_cflags, whole=True))
    # the game's sin, pow and the rest, the same on every port (port/include/halo_math.h)
    for source in musl_math_sources():
        objects.append(guest_object(source, " ".join([guest_abi, profile_flags, *libc_includes,
                                                      musl_math_cflags("")])))
    # internet play's reliable streams (port/third_party/kcp; p2p.c)
    objects.append(guest_object(KCP_DIR / "ikcp.c", platform_cflags, whole=True))
    # internet play's signatures, for public games' listings
    # (port/third_party/monocypher; p2p_crypto.c)
    for name in ("monocypher.c", "monocypher-ed25519.c"):
        objects.append(guest_object(MONOCYPHER_DIR / name, platform_cflags, whole=True))
    # the port's zlib, without the ARM CRC instructions: the guest's assembly
    # is assembled for plain aarch64, which does not take them
    for name in ZLIB_SOURCES:
        objects.append(guest_object(ZLIB_DIR / name, " ".join([platform_cflags, *ZLIB_DEFINES,
                                                              "-U__ARM_FEATURE_CRC32"])))
    runtime_internal_cflags = " ".join([
        guest_abi, "-std=c99", "-ffreestanding", "-fno-common", "-D_XOPEN_SOURCE=700", "-D_GNU_SOURCE",
        # guest_host.h includes gpu.h (port/linux/src)
        f"-I{PORT_DIR}/guest/runtime", f"-I{PORT_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{arch}", f"-I{MUSL_DIR}/arch/generic", f"-I{libc_internal}",
        f"-I{PORT_DIR}/guest/libc/src_include", f"-I{MUSL_DIR}/src/include",
        f"-I{MUSL_DIR}/src/internal", f"-I{libc_include}", f"-I{MUSL_DIR}/include",
    ])
    runtime_cflags = " ".join([
        guest_abi, guest_code, "-std=gnu11", "-D_GNU_SOURCE",
        f"-I{PORT_DIR}/guest/runtime", f"-I{PORT_DIR}/include", f"-I{LINUX_DIR}/src",
        f"-I{SDL_DIR}/include", *libc_includes,
    ])
    runtime_dir = PORT_DIR / "guest" / "runtime"
    for source in sorted(runtime_dir.glob("*.c")):
        if source.name in ("guest_thread.c", "guest_start.c"):
            objects.append(guest_object(source, runtime_internal_cflags))
        elif source.name in ("guest_memory_watch.c", "guest_bink.c"):
            objects.append(guest_object(source, platform_cflags))
        else:
            objects.append(guest_object(source, runtime_cflags))
    objects.append(guest_object(guest_posix_c, runtime_cflags))
    imports_o = obj_dir / "gen" / "imports.o"
    n.build(outputs=imports_o, rule="guest_as", inputs=imports_s)
    objects.append(imports_o)

    # ---------- link-time optimization: the bitcode units as one object

    bitcode = [obj for obj in objects if obj.suffix == ".bc"]
    if bitcode:
        objects = [obj for obj in objects if obj.suffix != ".bc"]
        # what the units outside the whole refer to stays visible in it
        keep = guest_dir / "lto" / "keep.txt"
        n.rule(
            name="guest_lto_keep",
            command=("$guest_tool_bin/llvm-nm --undefined-only --format=just-symbols --no-sort @$out.rsp "
                     "| grep --invert-match --extended-regexp ':$$|^$$' | sort --unique > $out"),
            description="IOS LTO KEEP $out",
            rspfile="$out.rsp",
            rspfile_content="$in_newline",
        )
        n.build(outputs=keep, rule="guest_lto_keep", inputs=[*objects, libguestc])
        whole_o = obj_dir / "lto" / "whole.o"
        n.rule(
            name="guest_lto",
            command=(f"{python} tools/ios_guest_lto.py --llvm-bin $guest_tool_bin --keep {keep} --cflags '$cflags' "
                     f"--work {guest_dir}/lto --out $out.darwin.s @$out.rsp && "
                     f"{python} {converter} $out.darwin.s $out.s && "
                     f"$guest_cc --target=aarch64-none-elf -c $out.s -o $out"),
            description="IOS LTO $out",
            rspfile="$out.rsp",
            rspfile_content="$in_newline",
        )
        n.build(outputs=whole_o, rule="guest_lto", inputs=bitcode,
                implicit=[keep, Path("tools/ios_guest_lto.py"), *tool_implicit],
                variables={"cflags": guest_abi})
        objects.append(whole_o)

    # ---------- the guest image

    linker_script = Path("port/ios/guest.ld")
    n.rule(
        name="guest_link",
        command=(f"$guest_linker -m aarch64linux -static -nostdlib -T {linker_script} "
                 f"-Map $out.map -o $out @$out.rsp {libguestc}"),
        description="IOS LINK $out",
        rspfile="$out.rsp",
        rspfile_content="$in_newline",
    )
    n.build(outputs=image, rule="guest_link", inputs=objects, implicit=[libguestc, linker_script])

    n.build(outputs="ios_guest", rule="phony", inputs=image)
