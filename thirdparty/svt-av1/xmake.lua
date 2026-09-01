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
        -- SVT-AV1 already has an Apple/AArch64 runtime feature detector based
        -- on sysctl. Avoid cpuinfo on iOS because the upstream Xmake cpuinfo
        -- package rejects cross builds; desktop builds keep the existing
        -- cpuinfo-backed detection path.
        if not package:is_plat("iphoneos") then
            package:add("deps", "cpuinfo")
        end
    end)

    on_install("windows", "linux", "macosx", "iphoneos", function (package)
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

        if package:is_plat("iphoneos") then
            table.insert(configs, "-DUSE_CPUINFO=OFF")
        else
            table.insert(configs, "-DUSE_CPUINFO=SYSTEM")
        end

        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("svt_av1_enc_init_handle", {
            includes = "svt-av1/EbSvtAv1Enc.h"
        }))
    end)
