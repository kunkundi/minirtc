package("websocketpp")
    set_kind("library", {headeronly = true})
    set_homepage("http://www.zaphoyd.com/websocketpp")
    set_description("C++ websocket client/server library")
    set_license("BSD-3-Clause")

    add_urls("https://github.com/zaphoyd/websocketpp/archive/refs/tags/$(version).tar.gz",
             "https://github.com/zaphoyd/websocketpp.git")
    add_versions("0.8.2", "6ce889d85ecdc2d8fa07408d6787e7352510750daa66b5ad44aacb47bea76755")
    
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
        assert(package:check_cxxsnippets({test = [[
            #include <websocketpp/config/asio_no_tls.hpp>
            #include <websocketpp/server.hpp>
            typedef websocketpp::server<websocketpp::config::asio> server;
            void test() {
                server echo_server;
            }
        ]]}, {configs = {languages = "c++17"}}))
    end)
