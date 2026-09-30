#!/usr/bin/env python3
# Copyright (c) 2025 Hans-Kristian Arntzen / PyroWave vendoring
# SPDX-License-Identifier: MIT
#
# Build-system-agnostic GLSL -> SPIR-V -> C++ generator for the vendored
# PyroWave codec. Replaces WiVRn's CompileGLSL.cmake so the exact same
# generated artifact can be produced from CMake (Sunshine) and qmake
# (moonlight-qt).
#
# It compiles every *.comp and *.glsl shader in the shader directory and emits
# two files in the output directory:
#   pyrowave_shaders.h    - extern declaration of the shader map
#   pyrowave_shaders.cpp  - the shader map definition (SPIR-V word arrays)
#
# The map keys MUST match the names passed to PyroWave::load_shader(), e.g.
#   dwt_0.comp        -> "dwt_0"
#   idwt.vert.glsl    -> "idwt.vert"
#   idwt_0.frag.glsl  -> "idwt_0.frag"
#
# Naming note: WiVRn's CMake uses cmake_path(GET STEM LAST_ONLY) which strips
# only the final extension. So "idwt_0.frag.glsl" -> "idwt_0.frag" and
# "dwt_0.comp" -> "dwt_0". We replicate that exactly.

import argparse
import os
import struct
import subprocess
import sys

# Vulkan pipeline stages we recognise, mirroring CompileGLSL.cmake's regex
# group: (vert|frag|tesc|tese|geom|comp)(.glsl)?$
STAGE_EXTS = ("vert", "frag", "tesc", "tese", "geom", "comp")


def detect_stage(filename):
    """Return (stage, shader_name) for a shader file, replicating the
    CompileGLSL.cmake naming, or None for non-shader files (.inc, etc)."""
    base = os.path.basename(filename)
    # Strip a trailing .glsl if present to inspect the stage extension.
    stem = base[:-5] if base.endswith(".glsl") else base
    # stem is now e.g. "idwt.vert", "dwt_0.comp", "yuv2rgb.frag"
    parts = stem.rsplit(".", 1)
    if len(parts) == 2 and parts[1] in STAGE_EXTS:
        stage = parts[1]
        # cmake_path STEM LAST_ONLY on the ORIGINAL file strips only the final
        # extension: "idwt_0.frag.glsl" -> "idwt_0.frag"; "dwt_0.comp" -> "dwt_0".
        if base.endswith(".glsl"):
            shader_name = base[:-5]               # drop ".glsl" -> "<x>.<stage>"
        else:
            shader_name = stem.rsplit(".", 1)[0]  # drop ".comp" -> "<x>"
        return stage, shader_name
    return None


_spirv_opt_missing_warned = False
_spirv_opt_count = 0


def compile_one(glslang, spirv_opt, target_env, shader_dir, path, stage, out_spv):
    global _spirv_opt_missing_warned
    stage_def = stage.upper() + "_SHADER"
    cmd = [
        glslang, "-V",
        "--target-env", target_env,
        "-S", stage,
        "-D" + stage_def,
        "-I" + shader_dir,
        path,
        "-o", out_spv,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write("pyrowave: failed to compile %s\n%s\n%s\n" %
                         (path, proc.stdout, proc.stderr))
        raise SystemExit(1)

    # Optimize with spirv-opt (as WiVRn does). Unoptimized glslang SPIR-V with
    # heavy subgroup / explicit-16-bit ops can crash some drivers (notably RADV)
    # at pipeline creation. Skip gracefully if spirv-opt is unavailable.
    if not spirv_opt:
        return
    try:
        opt = subprocess.run(
            [spirv_opt, "--target-env=" + target_env, "-O", out_spv, "-o", out_spv],
            capture_output=True, text=True)
    except FileNotFoundError:
        if not _spirv_opt_missing_warned:
            sys.stderr.write("pyrowave: spirv-opt not found; shaders left unoptimized "
                             "(install spirv-tools or set --spirv-opt=)\n")
            _spirv_opt_missing_warned = True
        return
    if opt.returncode != 0:
        sys.stderr.write("pyrowave: spirv-opt failed on %s\n%s\n%s\n" %
                         (out_spv, opt.stdout, opt.stderr))
        raise SystemExit(1)
    global _spirv_opt_count
    _spirv_opt_count += 1


def read_spirv_words(spv_path):
    with open(spv_path, "rb") as f:
        data = f.read()
    if len(data) % 4 != 0:
        raise SystemExit("pyrowave: SPIR-V %s not word-aligned" % spv_path)
    return struct.unpack("<%dI" % (len(data) // 4), data)


def main():
    ap = argparse.ArgumentParser(description="Generate PyroWave SPIR-V C++ tables")
    ap.add_argument("--shader-dir", required=True)
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--glslang", default=os.environ.get("GLSLANG", "glslangValidator"))
    ap.add_argument("--spirv-opt", default=os.environ.get("SPIRV_OPT", "spirv-opt"),
                    help="spirv-opt binary; pass empty to skip optimization")
    ap.add_argument("--target-env", default="vulkan1.1")
    ap.add_argument("--namespace", default="pyrowave")
    args = ap.parse_args()

    shader_dir = os.path.abspath(args.shader_dir)
    out_dir = os.path.abspath(args.output_dir)
    os.makedirs(out_dir, exist_ok=True)
    spv_dir = os.path.join(out_dir, "spv")
    os.makedirs(spv_dir, exist_ok=True)

    shaders = []
    for name in sorted(os.listdir(shader_dir)):
        if not (name.endswith(".comp") or name.endswith(".glsl")):
            continue  # .inc files are includes, never compiled directly
        det = detect_stage(name)
        if det is None:
            continue
        stage, shader_name = det
        src = os.path.join(shader_dir, name)
        spv = os.path.join(spv_dir, shader_name + ".spv")
        compile_one(args.glslang, args.spirv_opt, args.target_env, shader_dir, src, stage, spv)
        shaders.append((shader_name, read_spirv_words(spv)))

    if not shaders:
        raise SystemExit("pyrowave: no shaders found in %s" % shader_dir)

    ns = args.namespace
    header = os.path.join(out_dir, "pyrowave_shaders.h")
    source = os.path.join(out_dir, "pyrowave_shaders.cpp")

    with open(header, "w") as h:
        h.write("// Auto-generated by tools/generate_shaders.py. Do not edit.\n")
        h.write("#pragma once\n#include <cstdint>\n#include <map>\n#include <vector>\n#include <string>\n")
        h.write("namespace %s {\n" % ns)
        h.write("extern const std::map<std::string, std::vector<uint32_t>> shaders;\n")
        h.write("}\n")

    with open(source, "w") as c:
        c.write("// Auto-generated by tools/generate_shaders.py. Do not edit.\n")
        c.write("#include <cstdint>\n#include <map>\n#include <vector>\n#include <string>\n")
        c.write("namespace %s {\n" % ns)
        c.write("extern const std::map<std::string, std::vector<uint32_t>> shaders = {\n")
        for shader_name, words in shaders:
            c.write('{ "%s", {\n' % shader_name)
            for i in range(0, len(words), 8):
                chunk = words[i:i + 8]
                c.write(" " + "".join("0x%08x," % w for w in chunk) + "\n")
            c.write("}},\n")
        c.write("};\n}\n")

    if _spirv_opt_count:
        opt_note = "spirv-opt OPTIMIZED %d" % _spirv_opt_count
    else:
        opt_note = "spirv-opt NOT run - shaders UNOPTIMIZED; install spirv-tools to avoid driver crashes"
    sys.stderr.write("pyrowave: generated %d shaders (%s) -> %s\n" % (len(shaders), opt_note, source))


if __name__ == "__main__":
    main()
