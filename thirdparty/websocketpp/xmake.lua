package("websocketpp")
    set_kind("library", {headeronly = true})
    set_homepage("http://www.zaphoyd.com/websocketpp")
    set_description("C++ websocket client/server library")
    set_license("BSD-3-Clause")

    add_urls("https://github.com/zaphoyd/websocketpp/archive/refs/tags/$(version).tar.gz",
             "https://github.com/zaphoyd/websocketpp.git")
    add_versions("0.8.2", "6ce889d85ecdc2d8fa07408d6787e7352510750daa66b5ad44aacb47bea76755")
    -- C++20 rejects template-ids in constructor/destructor declarations (CWG 2237).
    add_patches("0.8.2", path.join(os.scriptdir(), "patches", "cpp20_constructors_0.8.2.patch"),
        "00cb9fc3af60d3115ae66dfb6da7b3e3698a29b9644e4c7a5328cee465c98e10")
    -- Use a new package hash so prebuilt images/caches cannot reuse unpatched headers.
    add_configs("cpp20_compat_revision", {default = "1", type = "string", readonly = true,
        description = "C++20-compatible constructor and destructor declarations"})

    add_configs("asio_standalone", {description = "Use standalone asio", default = true, type = "boolean"})

    add_deps("cmake~host", {host = true})

    on_load(function (package)
        package:add("defines", "ASIO_STANDALONE", "_WEBSOCKETPP_CPP11_STL_")
        package:add("deps", "asio 1.32.0")
        --package:add("deps", "boost", {configs = {system = true, regex = true, thread = true, type_traits = true}})
    end)

    on_install("!wasm", function (package)
        local configs = {"-DCMAKE_POLICY_DEFAULT_CMP0057=NEW"}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))
        table.insert(configs, "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"))
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        for _, language in ipairs({"c++17", "c++20"}) do
            assert(package:check_cxxsnippets({test = [[
            #include <websocketpp/config/asio_no_tls.hpp>
            #include <websocketpp/config/asio_no_tls_client.hpp>
            #include <websocketpp/server.hpp>
            #include <websocketpp/client.hpp>
            #ifndef _WIN32
            #include <websocketpp/logger/syslog.hpp>
            #endif
            void test() {
                websocketpp::server<websocketpp::config::asio> server;
                websocketpp::client<websocketpp::config::asio_client> client;
                #ifndef _WIN32
                websocketpp::log::syslog<websocketpp::concurrency::basic,
                    websocketpp::log::alevel> logger(websocketpp::log::alevel::all,
                        websocketpp::log::channel_type_hint::access);
                #endif
            }
            ]]}, {configs = {languages = language}}),
                "WebSocket++ client/server/logger compile check failed for " .. language)
        end
    end)
