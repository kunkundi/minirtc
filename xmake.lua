set_project("minirtc")
set_version("0.0.1")
set_license("LGPL-3.0")

option("USE_CUDA")
    set_default(false)
    set_showmenu(true)
    set_description("Use CUDA for hardware codec acceleration")
option_end()

add_rules("mode.release", "mode.debug")
set_languages("c++17")
set_encodings("utf-8")

set_installdir("$(projectdir)/out")

add_defines("ASIO_STANDALONE", "ASIO_HAS_STD_TYPE_TRAITS", "ASIO_HAS_STD_SHARED_PTR", 
    "ASIO_HAS_STD_ADDRESSOF", "ASIO_HAS_STD_ATOMIC", "ASIO_HAS_STD_CHRONO", 
    "ASIO_HAS_CSTDINT", "ASIO_HAS_STD_ARRAY",  "ASIO_HAS_STD_SYSTEM_ERROR",
    "NOMINMAX", "WIN32_LEAN_AND_MEAN")
add_defines("USE_CUDA=" .. (is_config("USE_CUDA", true) and "1" or "0"))

add_requires("asio 1.32.0", "nlohmann_json 3.11.3", "spdlog 1.14.1", "libnice 0.1.22", "websocketpp 0.8.2", "libsrtp v2.7.0", "openfec 1.4.2", "libopus 1.5.1", "openh264 2.6.0", "dav1d 1.4.3", "libyuv 2025.8.14", "aom 3.9.0", "svt-av1 v3.0.2", "concurrentqueue 1.0.4", {system = false}, {configs = {shared = false}})
add_packages("asio", "nlohmann_json", "spdlog", "libnice", "websocketpp", "libsrtp", "openfec", "libopus", "openh264", "dav1d", "libyuv", "aom", "svt-av1", "concurrentqueue")

add_requires("libdatachannel 0.23.2")
add_packages("libdatachannel")

includes("thirdparty")

if is_os("windows") then
    add_defines("_WEBSOCKETPP_CPP11_INTERNAL_")
    add_cxflags("/WX")
    set_runtimes("MT")
elseif is_os("linux") then
    add_cxflags("-fPIC", "-Wno-unused-variable") 
    add_syslinks("pthread")
elseif is_os("macosx") then
    -- add_ldflags("-fsanitize=address")
    add_ldflags("-Wl,-ld_classic")
    add_cxflags("-Wno-unused-variable")
    add_frameworks("VideoToolbox")
end

target("log")
    set_kind("object")
    add_files("src/log/log.cpp")
    add_includedirs("src/log", {public = true})

target("common")
    set_kind("object")
    add_deps("log")
    add_files("src/common/common.cpp", 
    "src/common/clock/system_clock.cpp", 
    "src/common/rtc_base/*.cc",
    "src/common/rtc_base/network/*.cc",
    "src/common/rtc_base/numerics/*.cc",
    "src/common/api/units/*.cc",
    "src/common/api/transport/*.cc",
    "src/common/api/clock/*.cc",
    "src/common/api/ntp/*.cc")
    if not is_os("windows") then
        remove_files("src/common/rtc_base/win32.cc")
    end
    add_includedirs("src/common", {public = true})

target("inih")
    set_kind("object")
    add_files("src/inih/ini.c", "src/inih/INIReader.cpp")
    add_includedirs("src/inih", {public = true})

target("ringbuffer")
    set_kind("object")
    add_files("src/ringbuffer/ringbuffer.cpp")
    add_includedirs("src/ringbuffer", {public = true})

target("thread")
    set_kind("object")
    add_deps("log")
    add_files("src/thread/*.cpp")
    add_includedirs("src/thread", {public = true})

target("frame")
    set_kind("object")
    add_deps("common")
    add_files("src/frame/*.cpp")
    add_includedirs("src/frame", {public = true})

target("fec")
    set_kind("object")
    add_deps("log")
    add_files("src/fec/*.cpp")
    add_includedirs("src/fec", {public = true})

target("statistics")
    set_kind("object")
    add_deps("log")
    add_files("src/statistics/*.cpp")
    add_includedirs("src/statistics", {public = true})

target("ice")
    set_kind("object")
    add_deps("log", "common", "ws")
    add_files("src/ice/*.cpp")
    add_includedirs("src/ws", "src/ice", {public = true})
    
target("ws")
    set_kind("object")
    add_deps("log")
    add_files("src/ws/*.cpp")
    add_includedirs("src/ws", {public = true})

target("rtp")
    set_kind("object")
    add_deps("log", "common", "frame", "ringbuffer", "thread", "rtcp", "fec", "statistics")
    add_files("src/rtp/rtp_packet/*.cpp",
    "src/rtp/rtp_packetizer/*.cpp")
    add_includedirs("src/rtp/rtp_packet",
    "src/rtp/rtp_packetizer", {public = true})

target("srtp")
    set_kind("object")
    add_deps("log", "common")
    add_files("src/srtp/srtp_engine.cpp")
    add_includedirs("src/srtp", {public = true})

target("rtcp")
    set_kind("object")
    add_deps("log", "common")
    add_files("src/rtcp/*.cpp",
    "src/rtcp/rtcp_packet/*.cpp",
    "src/rtcp/rtp_feedback/*.cpp")
    add_includedirs("src/rtcp",
    "src/rtcp/rtcp_packet",
    "src/rtcp/rtcp_sender",
    "src/rtcp/rtp_feedback", {public = true})

target("qos")
    set_kind("object")
    add_deps("log", "rtp", "rtcp")
    add_files("src/qos/*.cc", 
    "src/qos/*.cpp")
    add_includedirs("src/qos", {public = true})

target("transport")
    set_kind("object")
    add_deps("log", "ws", "ice", "rtp", "rtcp", "srtp", "statistics", "media", "qos")
    add_files("src/transport/*.cpp",
    "src/transport/channel/*.cpp",
    "src/transport/paced_sender/*.cpp")
    add_includedirs("src/transport",
    "src/transport/channel",
    "src/transport/paced_sender", {public = true})

target("media")
    set_kind("object")
    add_deps("log", "frame", "common", "rtp")
    if is_os("windows") then
        add_files("src/media/video/encode/*.cpp",
        "src/media/video/decode/*.cpp",
        "src/media/video/encode/openh264/*.cpp",
        "src/media/video/decode/openh264/*.cpp",
        "src/media/video/encode/aom/*.cpp",
        "src/media/video/encode/avt/*.cpp",
        "src/media/video/decode/dav1d/*.cpp",
        "src/media/video/decode/aom/*.cpp")
        add_includedirs("src/media/video/encode",
        "src/media/video/decode",
        "src/media/video/encode/openh264",
        "src/media/video/decode/openh264",
        "src/media/video/encode/aom",
        "src/media/video/encode/avt",
        "src/media/video/decode/dav1d",
        "src/media/video/decode/aom", {public = true})
        if is_config("USE_CUDA", true) then
            add_files("src/media/video/encode/nvcodec/*.cpp",
            "src/media/video/decode/nvcodec/*.cpp",
            "src/media/nvcodec/*.cpp")
            add_includedirs("src/media/video/encode/nvcodec",
            "src/media/video/decode/nvcodec",
            "src/media/nvcodec",
            "thirdparty/nvcodec/interface", {public = true})
            add_includedirs(path.join(os.getenv("CUDA_PATH"), "include"), {public = true})
        end
    elseif is_os("linux") then
        add_files("src/media/video/encode/*.cpp",
        "src/media/video/decode/*.cpp",
        "src/media/video/encode/openh264/*.cpp",
        "src/media/video/decode/openh264/*.cpp",
        "src/media/video/encode/aom/*.cpp",
        "src/media/video/encode/avt/*.cpp",
        "src/media/video/decode/dav1d/*.cpp",
        "src/media/video/decode/aom/*.cpp")
        add_includedirs("src/media/video/encode",
        "src/media/video/decode",
        "src/media/video/encode/openh264",
        "src/media/video/decode/openh264",
        "src/media/video/encode/aom",
        "src/media/video/encode/avt",
        "src/media/video/decode/dav1d",
        "src/media/video/decode/aom", {public = true})
        if is_arch("x86_64") and is_config("USE_CUDA", true) then
            add_files("src/media/video/encode/nvcodec/*.cpp",
            "src/media/video/decode/nvcodec/*.cpp",
            "src/media/nvcodec/*.cpp")
            add_includedirs("src/media/video/encode/nvcodec",
            "src/media/video/decode/nvcodec",
            "src/media/nvcodec",
            "thirdparty/nvcodec/interface", {public = true})
            add_includedirs(path.join(os.getenv("CUDA_PATH"), "include"), {public = true})
        elseif is_arch("arm64", "aarch64") then
        end
    elseif is_os("macosx") then
        add_files("src/media/video/encode/*.cpp",
        "src/media/video/decode/*.cpp",
        "src/media/video/encode/openh264/*.cpp",
        "src/media/video/decode/openh264/*.cpp",
        "src/media/video/encode/video_toolbox/*.mm",
        "src/media/video/decode/video_toolbox/*.mm",
        "src/media/video/encode/aom/*.cpp",
        "src/media/video/encode/avt/*.cpp",
        "src/media/video/decode/dav1d/*.cpp",
        "src/media/video/decode/aom/*.cpp")
        add_includedirs("src/media/video/encode",
        "src/media/video/decode",
        "src/media/video/encode/openh264",
        "src/media/video/decode/openh264",
        "src/media/video/encode/video_toolbox",
        "src/media/video/decode/video_toolbox",
        "src/media/video/encode/aom",
        "src/media/video/encode/avt",
        "src/media/video/decode/dav1d",
        "src/media/video/decode/aom", {public = true})
    end
    add_files("src/media/audio/encode/*.cpp",
        "src/media/audio/decode/*.cpp",
        "src/media/resolution_adapter/*.cpp",
        "src/media/video/assemble_frame/*.cpp")
    add_includedirs("src/media/audio/encode",
        "src/media/audio/decode",
        "src/media/resolution_adapter",
        "src/media/video/assemble_frame",
        "src/api", "src/media", {public = true})

target("pc")
    set_kind("object")
    add_deps("log", "ws", "ice", "transport", "inih", "common")
    add_files("src/pc/*.cpp")
    add_includedirs("src/pc", "src/api", {public = true})

target("minirtc")
    set_kind("static")
    add_deps("log", "pc")
    add_files("src/rtc/*.cpp")
    add_includedirs("src/rtc", "src/api")

    if is_os("windows") then
        if is_config("USE_CUDA", true) then
            add_linkdirs("thirdparty/nvcodec/lib/x64")
            add_linkdirs(path.join(os.getenv("CUDA_PATH"), "lib/x64"))
        end
        add_links("Shell32", "Advapi32", "Dnsapi", "Shlwapi", "Crypt32", 
        "ws2_32", "windowsapp", "User32", "Strmiids", "Mfuuid",
        "Secur32", "Bcrypt")
        -- add_links("cuda", "nvencodeapi", "nvcuvid")
    elseif is_os("linux") then
        if is_arch("x86_64") and is_config("USE_CUDA", true) then
            add_linkdirs("thirdparty/nvcodec/lib/x64")
            add_linkdirs("thirdparty/nvcodec/lib/linux/stubs/x86_64")
            add_linkdirs(path.join(os.getenv("CUDA_PATH"), "lib64"))
            -- add_links("cuda", "nvidia-encode", "nvcuvid")
        elseif is_arch("arm64", "aarch64") then
        end
    elseif is_os("macosx") then

    end

    add_installfiles("src/api/*.h", {prefixdir = "include"})
    -- add_rules("utils.symbols.export_list", {symbols = {
    --     "CreatePeer",
    --     "DestroyPeer",
    --     "Init",
    --     "CreateConnection",
    --     "JoinConnection",
    --     "LeaveConnection",
    --     "SendData"}})


    -- add_rules("utils.symbols.export_all", {export_classes = true})
-- after_install(function (target)
--     os.rm("$(projectdir)/out/lib/*.a")
--     os.rm("$(projectdir)/out/include/log.h")
-- end)ss