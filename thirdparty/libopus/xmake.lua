package("libopus")
    set_homepage("https://opus-codec.org")
    set_description("Modern audio compression for the internet.")
    set_license("BSD-3-Clause")

    -- Keep the numeric package version while using Opus's v-prefixed Git tags.
    -- The upstream 1.5.1 recipe cannot fall back to Git when Xiph is unavailable.
    add_urls("https://github.com/xiph/opus/archive/refs/tags/v$(version).tar.gz", {alias = "github"})
    add_urls("https://downloads.xiph.org/releases/opus/opus-$(version).tar.gz", {alias = "release"})
    add_urls("https://github.com/xiph/opus.git", {alias = "git"})
    add_versions("github:1.5.1", "7ce44ef3d335a3268f26be7d53bb3bed7205b34eaf80bf92a99e69d490afe9d9")
    add_versions("release:1.5.1", "b84610959b8d417b611aa12a22565e0a3732097c6389d19098d844543e340f85")
    add_versions("git:1.5.1", "v1.5.1")

    add_configs("avx", {description = "AVX supported", default = true, type = "boolean"})
    add_configs("check_avx", {description = "Does runtime check for AVX support", default = true, type = "boolean"})
    add_deps("cmake~host", {host = true})

    on_install(function (package)
        io.replace("CMakeLists.txt", [[set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")]], "", {plain = true})
        io.replace("CMakeLists.txt", [[set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")]], "", {plain = true})

        local configs = {
            "-DCMAKE_BUILD_TYPE=" .. (package:is_debug() and "Debug" or "Release"),
            "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"),
            "-DAVX_SUPPORTED=" .. (package:config("avx") and "ON" or "OFF"),
            "-DOPUS_X86_MAY_HAVE_AVX=" .. (package:config("check_avx") and "ON" or "OFF")
        }
        if not package:gitref() and package:version() then
            table.insert(configs, "-DOPUS_PACKAGE_VERSION=" .. package:version():shortstr())
        end
        if package:is_plat("mingw", "wasm") then
            table.insert(configs, "-DOPUS_STACK_PROTECTOR=OFF")
        elseif package:is_plat("android") then
            table.insert(configs, "-DOPUS_DISABLE_INTRINSICS=ON")
        end
        import("package.tools.cmake").install(package, configs)
    end)
