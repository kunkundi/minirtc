package("libnice")
    set_kind("library")
    set_homepage("https://libnice.freedesktop.org/")
    set_description("libnice is an implementation of the IETF's Interactive Connectivity Establishment (ICE) standard")

    add_urls("https://gitlab.freedesktop.org/libnice/libnice/-/archive/$(version)/libnice-$(version).tar.gz")

    add_deps("meson", "glib 2.84.1", "openssl3 3.3.2")

    on_install(function (package)
        if package:is_plat("windows") then
            io.replace("meson.build",
                "syslibs += [cc.find_library('ws2_32')]",
                "syslibs += [cc.find_library('ws2_32')]\n  syslibs += [cc.find_library('crypt32')]",
                {plain = true})
        end

        local  configs = {
            "-Ddefault_library=static",
            "-Dgstreamer=disabled",
            "-Dexamples=disabled",
            "-Dtests=disabled",
            "-Dgtk_doc=disabled",
            "-Dcrypto-library=openssl",
            "-Dintrospection=disabled"
        }

        import("package.tools.meson").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("nice_agent_new", {includes = "nice/agent.h"}))
    end)
