package("libnice")
    set_kind("library")
    set_homepage("https://libnice.freedesktop.org/")
    set_description("libnice is an implementation of the IETF's Interactive Connectivity Establishment (ICE) standard")

    add_urls("https://gitlab.freedesktop.org/libnice/libnice/-/archive/$(version)/libnice-$(version).tar.gz")

    add_deps("meson", "glib 2.85.0", "openssl 1.1.1-w")

    on_install(function (package)
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
