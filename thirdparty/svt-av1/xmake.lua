package("svt-av1")
    set_homepage("https://gitlab.com/AOMediaCodec/SVT-AV1")
    set_description("Scalable Video Technology for AV1 encoder")
    -- The main project uses Clear BSD; selected bundled files use BSD-2-Clause.
    set_license("BSD-3-Clause-Clear")

    add_urls("https://gitlab.com/AOMediaCodec/SVT-AV1.git")
    add_versions("v3.0.2", "efc905a7c2ed155b3654d7968173622734eeb0c0")

    add_configs("avx512", {description = "Enable AVX-512 code", default = false, type = "boolean"})
    add_configs("minimal_build", {description = "Enable minimal build", default = false, type = "boolean"})
    add_configs("tools", {description = "Build command-line tools", default = false, type = "boolean"})
    add_configs("pgo", {description = "Enable profile-guided optimization", default = false, type = "boolean"})
    add_configs("native", {description = "Build for the host CPU", default = false, type = "boolean"})

    add_deps("cmake~host", "nasm~host", {host = true})

    on_load(function (package)
        -- Mobile AArch64 uses SVT-AV1's own runtime CPU detection: sysctl on
        -- iOS and getauxval on Android. Keep NEON and optional instruction
        -- dispatch without adding the separate cpuinfo cross-build dependency.
        if not package:is_plat("iphoneos", "android") then
            package:add("deps", "cpuinfo")
        end
    end)

    on_install("windows", "linux", "macosx", "iphoneos", "android", function (package)
        local configs = {
            "-DBUILD_TESTING=OFF",
            "-DCOVERAGE=OFF",
            "-DBUILD_SHARED_LIBS=OFF",
            "-DBUILD_APPS=" .. (package:config("tools") and "ON" or "OFF"),
            "-DSVT_AV1_LTO=OFF",
            "-DSVT_AV1_PGO=" .. (package:config("pgo") and "ON" or "OFF"),
            "-DMINIMAL_BUILD=" .. (package:config("minimal_build") and "ON" or "OFF"),
            "-DENABLE_AVX512=" .. (package:config("avx512") and "ON" or "OFF"),
            "-DNATIVE=" .. (package:config("native") and "ON" or "OFF"),
            "-DEXCLUDE_HASH=ON"
        }

        if package:is_plat("iphoneos", "android") then
            table.insert(configs, "-DUSE_CPUINFO=OFF")
        else
            table.insert(configs, "-DUSE_CPUINFO=SYSTEM")
        end

        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
        local cmake = import("package.tools.cmake")
        local opt = {}
        if package:is_plat("android") then
            -- Upstream CMake replaces the NDK's absolute archive tools with
            -- bare llvm-ar/llvm-ranlib names. Resolve those to the same NDK.
            opt.envs = cmake.buildenvs(package)
            opt.envs.PATH = path.directory(package:build_getenv("ar")) ..
                path.envsep() .. (opt.envs.PATH or os.getenv("PATH") or "")
        end
        cmake.install(package, configs, opt)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("svt_av1_enc_init_handle", {
            includes = "svt-av1/EbSvtAv1Enc.h"
        }))
    end)
