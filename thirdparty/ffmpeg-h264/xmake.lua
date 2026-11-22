package("ffmpeg-h264")
    set_homepage("https://ffmpeg.org/")
    set_description("FFmpeg H.264 software decoder for MiniRTC High 10 streams")
    set_license("LGPL-2.1-or-later")
    add_urls("https://ffmpeg.org/releases/ffmpeg-$(version).tar.xz")
    add_versions("8.1.3", "7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3")
    add_links("avcodec", "avutil")

    if is_plat("windows") then
        add_syslinks("bcrypt")
    end
    if is_arch("x64", "x86_64", "i386") then
        add_deps("nasm", {host = true})
    end
    if is_host("windows") then
        add_deps("msys2", {configs = {msystem = "MINGW64", base_devel = true, make = true}})
    end

    on_install("macosx", "windows", function (package)
        import("package.tools.autoconf")
        local configs = {"--disable-everything", "--disable-autodetect",
            "--disable-programs", "--disable-doc", "--disable-debug", "--disable-network",
            "--disable-avformat", "--disable-avdevice", "--disable-avfilter",
            "--disable-swscale", "--disable-swresample", "--enable-decoder=h264",
            "--disable-shared", "--enable-static"}
        configs.host = ""
        local opt = {}
        if package:is_plat("windows") then
            table.insert(configs, "--target-os=win32")
            table.insert(configs, "--toolchain=msvc")
            table.insert(configs, "--extra-cflags=-" .. package:runtimes())
            opt.envs = autoconf.buildenvs(package)
            opt.envs.AR = nil
        elseif package:is_cross() then
            table.insert(configs, "--enable-cross-compile")
            table.insert(configs, "--target-os=darwin")
            table.insert(configs, "--arch=" .. package:arch())
        end
        autoconf.install(package, configs, opt)
        os.cp("COPYING.LGPLv2.1", package:installdir("share/licenses/ffmpeg-h264"))
    end)

    on_test(function (package)
        assert(package:has_cfuncs("avcodec_find_decoder", {includes = "libavcodec/avcodec.h"}))
    end)
