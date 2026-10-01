package("openh264")
    set_homepage("http://www.openh264.org/")
    set_description("OpenH264 is a codec library which supports H.264 encoding and decoding.")
    set_license("BSD-2-Clause")

    set_urls("https://github.com/cisco/openh264/archive/refs/tags/$(version).tar.gz",
             "https://github.com/cisco/openh264.git")

    add_versions("v2.6.0", "558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69")
    add_versions("v2.4.1", "8ffbe944e74043d0d3fb53d4a2a14c94de71f58dbea6a06d0dc92369542958ea")
    add_versions("v2.1.1", "af173e90fce65f80722fa894e1af0d6b07572292e76de7b65273df4c0a8be678")

    if is_plat("linux") then
        add_syslinks("pthread", "rt")
    end

    add_deps("meson~host", "ninja~host", "nasm~host", {host = true})

    if is_plat("windows") then
        -- Also gives fixed builds a distinct package cache key so existing
        -- scalar-only prebuilt libraries are not silently reused.
        add_configs("windows_x86_asm", {description = "Enable x86 SIMD dispatch on Windows", default = true, type = "boolean"})
    elseif is_plat("linux", "macosx") and is_arch("x86", "x64", "i386", "x86_64") then
        add_configs("unix_x86_asm", {description = "Enable C++ x86 SIMD dispatch on Linux and macOS", default = true, type = "boolean"})
    end

    on_load("windows", function (package)
        if package:is_plat("windows") and package:is_arch("arm.*") and (not package:is_precompiled()) then
            package:add("deps", "strawberry-perl")
        end
    end)

    on_install("windows", "linux", "macosx", "iphoneos", "android", function (package)
        if package:version():ge("2.4.1") then
            import("package.tools.meson")

            if package:is_plat("windows") and package:is_arch("x86", "x64") and package:config("windows_x86_asm") then
                -- OpenH264 2.6's Windows Meson branch assembles SIMD objects,
                -- but omits the C/C++ dispatch defines. Without X86_ASM those
                -- objects are never called and encoding uses scalar routines.
                io.replace("meson.build",
                    "asm_args += ['-DPREFIX', '-DX86_32']",
                    [[asm_args += ['-DPREFIX', '-DX86_32', '-DHAVE_AVX2']
    add_project_arguments('-DX86_ASM', '-DX86_32_ASM', '-DHAVE_AVX2', language: ['c', 'cpp'])]],
                    {plain = true})
                io.replace("meson.build",
                    "asm_args += ['-DWIN64']",
                    [[asm_args += ['-DWIN64', '-DHAVE_AVX2']
    add_project_arguments('-DX86_ASM', '-DHAVE_AVX2', language: ['c', 'cpp'])]],
                    {plain = true})
            elseif package:is_plat("linux", "macosx") and package:config("unix_x86_asm") then
                -- The Unix Meson branch defines X86_ASM for C only, while
                -- CPU detection and SIMD dispatch are implemented in C++.
                io.replace("meson.build",
                    "add_project_arguments('-DHAVE_AVX2', '-DX86_ASM', '-DX86_32_ASM', language: 'c')",
                    "add_project_arguments('-DHAVE_AVX2', '-DX86_ASM', '-DX86_32_ASM', language: ['c', 'cpp'])",
                    {plain = true})
                io.replace("meson.build",
                    "add_project_arguments('-DHAVE_AVX2', '-DX86_ASM', language: 'c')",
                    "add_project_arguments('-DHAVE_AVX2', '-DX86_ASM', language: ['c', 'cpp'])",
                    {plain = true})
                if package:is_plat("macosx") then
                    -- Mach-O C symbols have a leading underscore. Match the
                    -- upstream Darwin Makefile so SIMD calls resolve to the
                    -- NASM objects when C++ dispatch is enabled.
                    io.replace("meson.build",
                        "    asm_format64 = 'macho64'",
                        "    asm_format64 = 'macho64'\n    asm_args += ['-DPREFIX']",
                        {plain = true})
                end
            end

            local opt = {}
            opt.envs = meson.buildenvs(package)
            -- add gas-preprocessor to PATH
            if package:is_plat("windows") and package:is_arch("arm.*") then
                opt.envs.PATH = path.join(os.programdir(), "scripts") .. path.envsep() .. opt.envs.PATH
            end

            if package:is_plat("linux") and package:has_tool("cc", "clang", "clangxx") then
                opt.ldflags = "-lstdc++"
                opt.shflags = "-lstdc++"
            end

            local configs = {"-Dtests=disabled"}
            table.insert(configs, "-Ddefault_library=" .. (package:config("shared") and "shared" or "static"))
            meson.install(package, configs, opt)
        else
            import("package.tools.meson").build(package, {"-Dtests=disabled"}, {buildir = "out"})
            import("package.tools.ninja").install(package, {}, {buildir = "out"})
            if package:config("shared") then
                os.tryrm(path.join(package:installdir("lib"), "libopenh264.a"))
            else
                os.tryrm(path.join(package:installdir("lib"), "libopenh264.so*"))
                os.tryrm(path.join(package:installdir("lib"), "openh264.lib"))
                os.tryrm(path.join(package:installdir("bin"), "openh264-*.dll"))
            end
            if package:is_plat("windows") then
                os.trymv(path.join(package:installdir("lib"), "libopenh264.a"), path.join(package:installdir("lib"), "openh264.lib"))
            end
        end
    end)

    on_test(function (package)
        assert(package:has_cxxfuncs("WelsGetCodecVersion", {includes = "wels/codec_api.h"}))
    end)
