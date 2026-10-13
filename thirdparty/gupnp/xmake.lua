-- Build the whole GObject stack against minirtc's static GLib. Mixing a
-- Homebrew/system GUPnP with that GLib can load two GObject runtimes.
-- Each Meson consumer needs pkgconf directly, even when GLib is already cached.
package("libsoup")
    set_homepage("https://libsoup.org")
    set_description("HTTP transport for UPnP discovery and SOAP requests")
    set_license("LGPL-2.0-or-later")
    add_urls("https://download.gnome.org/sources/libsoup/3.6/libsoup-$(version).tar.xz",
             "https://sources.buildroot.net/libsoup3/libsoup-$(version).tar.xz")
    add_versions("3.6.5", "6891765aac3e949017945c3eaebd8cc8216df772456dc9f460976fbdb7ada234")
    add_deps("meson~host", "ninja~host", "pkgconf", {host = true})
    add_deps("glib 2.84.1", "nghttp2 1.68.0", "sqlite3 3.50.0+400", "upnp-libpsl 0.21.5", "zlib",
        {system = false, configs = {shared = false}})
    add_includedirs("include/libsoup-3.0")
    add_links("soup-3.0")
    -- iOS uses the proxy-libintl archive and headers bundled with GLib.
    -- The standalone libintl package does not support iphoneos.
    if is_plat("macosx", "windows", "mingw") then
        add_deps("libintl", {system = false, configs = {shared = false}})
    end
    if is_plat("windows", "mingw") then add_syslinks("ws2_32") end
    on_install(function (package)
        io.replace("meson.build", "subdir('examples')", "", {plain = true})
        io.replace("meson.build", "subdir('po')", "", {plain = true})
        local configs = {"-Ddefault_library=static", "-Dtests=false", "-Dinstalled_tests=false",
            "-Dintrospection=disabled", "-Dvapi=disabled", "-Ddocs=disabled",
            "-Dsysprof=disabled", "-Dntlm=disabled", "-Dgssapi=disabled",
            "-Dbrotli=disabled", "-Dpkcs11_tests=disabled", "-Dautobahn=disabled",
            "-Dtls_check=false"}
        -- UPnP uses HTTP on the LAN; no separate glib-networking TLS backend
        -- is needed. CrossDesk signaling/TURN continue to use OpenSSL.
        -- nghttp2's pkg-config file omits the static-library define on Windows.
        local cxflags = package:is_plat("windows", "mingw") and {"-DNGHTTP2_STATICLIB"} or {}
        import("package.tools.meson").install(package, configs,
            {cxflags = cxflags, packagedeps = package:is_plat("iphoneos") and {"glib"} or
                (package:dep("libintl") and {"libintl"} or {})})
    end)

package("gssdp")
    set_homepage("https://gitlab.gnome.org/GNOME/gssdp")
    set_description("GObject SSDP discovery library")
    set_license("LGPL-2.1-or-later")
    add_urls("https://download.gnome.org/sources/gssdp/1.6/gssdp-$(version).tar.xz",
             "https://sources.buildroot.net/gssdp/gssdp-$(version).tar.xz")
    add_versions("1.6.4", "ff97fdfb7f561d3e6813b4f6a2145259e7c2eff43cc0e63f3fd031d0b6266032")
    add_deps("meson~host", "ninja~host", "pkgconf", {host = true})
    add_deps("glib 2.84.1", "libsoup 3.6.5", {system = false, configs = {shared = false}})
    add_includedirs("include/gssdp-1.6")
    add_links("gssdp-1.6")
    if is_plat("windows", "mingw") then add_syslinks("ws2_32", "iphlpapi") end
    on_install(function (package)
        io.replace("meson.build", "subdir('tests')", "", {plain = true})
        if package:is_plat("windows") then
            -- GSSDP 1.6.4 includes this POSIX header unconditionally, although
            -- its Windows implementation does not use it and MSVC lacks it.
            io.replace("libgssdp/gssdp-client.c", "#include <unistd.h>", "", {plain = true})
        end
        import("package.tools.meson").install(package, {"-Ddefault_library=static",
            "-Dintrospection=false", "-Dvapi=false", "-Dgtk_doc=false",
            "-Dsniffer=false", "-Dmanpages=false", "-Dexamples=false"})
    end)

package("gupnp")
    set_homepage("https://gitlab.gnome.org/GNOME/gupnp")
    set_description("GObject UPnP control point library")
    set_license("LGPL-2.1-or-later")
    add_urls("https://download.gnome.org/sources/gupnp/1.6/gupnp-$(version).tar.xz",
             "https://sources.buildroot.net/gupnp/gupnp-$(version).tar.xz")
    add_versions("1.6.9", "2edb6ee3613558e62f538735368aee27151b7e09d4e2e2c51606833da801869b")
    add_deps("meson~host", "ninja~host", "pkgconf", {host = true})
    add_deps("glib 2.84.1", "gssdp 1.6.4", "libsoup 3.6.5", "upnp-libxml2 2.14.6",
        {system = false, configs = {shared = false}})
    add_includedirs("include/gupnp-1.6")
    add_links("gupnp-1.6")
    if is_plat("windows", "mingw") then add_syslinks("ws2_32", "iphlpapi") end
    on_install(function (package)
        io.replace("meson.build", "subdir('tests')", "", {plain = true})
        io.replace("meson.build", "subdir('tools')", "", {plain = true})
        if package:is_plat("windows") then
            io.replace("libgupnp/gupnp-context.c", "#include <unistd.h>", "", {plain = true})
            io.replace("libgupnp/gupnp-context.c", "S_ISDIR (st.st_mode)",
                "((st.st_mode & _S_IFMT) == _S_IFDIR)", {plain = true})
            -- Expand the GNU omitted-middle-operand conditional for MSVC.
            io.replace("libgupnp/gupnp-context.c", "        user_agent = g_strdup_printf",
                "        const char *program_name = g_get_prgname ();\n        user_agent = g_strdup_printf",
                {plain = true})
            io.replace("libgupnp/gupnp-context.c", 'g_get_prgname()? : ""',
                'program_name ? program_name : ""', {plain = true})
            -- MSVC has no GLib g_autoptr cleanup support. Release the local
            -- GBytes reference explicitly after libsoup retains the body.
            io.replace("libgupnp/gupnp-service-proxy.c", "g_autoptr (GBytes) body = NULL;",
                "GBytes *body = NULL;", {plain = true})
            io.replace("libgupnp/gupnp-service-proxy.c",
                "                                                  body);\n        action->msg_str = NULL;",
                "                                                  body);\n        g_bytes_unref (body);\n        action->msg_str = NULL;",
                {plain = true})
        end
        import("package.tools.meson").install(package, {"-Ddefault_library=static",
            "-Dintrospection=false", "-Dvapi=false", "-Dgtk_doc=false", "-Dexamples=false"})
    end)

package("gupnp-igd")
    set_homepage("https://gitlab.gnome.org/GNOME/gupnp-igd")
    set_description("UPnP IGD mapping, renewal and cleanup for libnice")
    set_license("LGPL-2.1-or-later")
    add_urls("https://download.gnome.org/sources/gupnp-igd/1.6/gupnp-igd-$(version).tar.xz",
             "https://mirrors.nju.edu.cn/gentoo/distfiles/c5/gupnp-igd-$(version).tar.xz")
    add_versions("1.6.0", "4099978339ab22126d4968f2a332b6d094fc44c78797860781f1fc2f11771b74")
    add_patches("1.6.0", path.join(os.scriptdir(), "patches", "gupnp_igd_action_lifetime_1.6.0.patch"),
        "8cac12a9b72b7025011031331950f4846728071b8374fd6d5ba512c174d3f110")
    -- Invalidate cached archives built with leaking actions/unbounded retries.
    add_configs("action_lifetime_revision", {default = "1", type = "string", readonly = true,
        description = "Release asynchronous actions and bound port conflict retries"})
    add_deps("meson~host", "ninja~host", "pkgconf", {host = true})
    add_deps("glib 2.84.1", "gupnp 1.6.9", "gssdp 1.6.4",
        {system = false, configs = {shared = false}})
    add_includedirs("include/gupnp-igd-1.6")
    add_links("gupnp-igd-1.6")
    if is_plat("windows", "mingw") then add_syslinks("ws2_32") end
    on_install(function (package)
        io.replace("meson.build", "subdir('tests')", "", {plain = true})
        if package:is_plat("windows") then
            -- inet_pton and the address types come from Winsock on Windows.
            io.replace("libgupnp-igd/gupnp-simple-igd.c", "#include <arpa/inet.h>",
                "#include <winsock2.h>\n#include <ws2tcpip.h>", {plain = true})
            io.replace("libgupnp-igd/gupnp-simple-igd.c", "#include <netinet/in.h>", "", {plain = true})
            io.replace("libgupnp-igd/gupnp-simple-igd.c", "#include <sys/socket.h>", "", {plain = true})
        end
        import("package.tools.meson").install(package, {"-Ddefault_library=static",
            "-Dintrospection=false", "-Dgtk_doc=false"})
    end)
    on_test(function (package)
        assert(package:has_cfuncs("gupnp_simple_igd_thread_new",
            {includes = "libgupnp-igd/gupnp-simple-igd-thread.h"}))
    end)

package("upnp-libpsl")
    set_homepage("https://github.com/rockdaboot/libpsl")
    set_description("Public suffix library for the bundled UPnP HTTP stack")
    set_license("MIT")
    add_urls("https://github.com/rockdaboot/libpsl/releases/download/$(version)/libpsl-$(version).tar.gz")
    add_versions("0.21.5", "1dcc9ceae8b128f3c0b3f654decd0e1e891afc6ff81098f227ef260449dae208")
    add_deps("meson~host", "ninja~host", {host = true})
    add_links("psl")
    if is_plat("windows", "mingw") then
        add_defines("PSL_STATIC")
        add_syslinks("ws2_32")
    end
    on_install(function (package)
        import("package.tools.meson").install(package, {"-Ddefault_library=static",
            "-Druntime=no", "-Dbuiltin=false", "-Dtests=false"})
    end)

package("upnp-libxml2")
    set_homepage("https://gitlab.gnome.org/GNOME/libxml2")
    set_description("XML parser for the bundled UPnP stack")
    set_license("MIT")
    add_urls("https://download.gnome.org/sources/libxml2/2.14/libxml2-$(version).tar.xz",
             "https://dev-www.libreoffice.org/src/libxml2-$(version).tar.xz")
    add_versions("2.14.6", "7ce458a0affeb83f0b55f1f4f9e0e55735dbfc1a9de124ee86fb4a66b597203a")
    add_deps("cmake~host", {host = true})
    add_includedirs("include/libxml2")
    if is_plat("windows", "mingw") then
        add_defines("LIBXML_STATIC")
        add_syslinks("ws2_32", "bcrypt")
    elseif is_plat("linux") then
        add_syslinks("m")
    end
    on_install(function (package)
        if package:is_plat("windows") then
            -- CMake names MSVC static archives libxml2s[d].lib, while the
            -- upstream pkg-config template still asks Meson for xml2.lib.
            local linkname = package:is_debug() and "libxml2sd" or "libxml2s"
            io.replace("libxml-2.0.pc.in", "@XML_LIBS@", "-l" .. linkname, {plain = true})
        end
        import("package.tools.cmake").install(package, {"-DBUILD_SHARED_LIBS=OFF",
            "-DLIBXML2_WITH_PYTHON=OFF", "-DLIBXML2_WITH_TESTS=OFF",
            "-DLIBXML2_WITH_PROGRAMS=OFF", "-DLIBXML2_WITH_ICONV=OFF",
            "-DLIBXML2_WITH_ZLIB=OFF", "-DLIBXML2_WITH_LZMA=OFF"})
    end)
