package("glib")
    set_homepage("https://docs.gtk.org/glib/")
    set_description("Low-level core library that forms the basis for projects such as GTK+ and GNOME.")
    set_license("LGPL-2.1")

    add_urls("https://download.gnome.org/sources/glib/$(version).tar.xz", {alias = "home", version = function (version)
        return format("%d.%d/glib-%s", version:major(), version:minor(), version)
    end, excludes = {"*/COPYING"}})
    add_urls("https://gitlab.gnome.org/GNOME/glib/-/archive/$(version)/glib-$(version).tar.gz", {alias = "gitlab"})
    add_urls("https://gitlab.gnome.org/GNOME/glib.git")
    add_versions("home:2.71.0", "926816526f6e4bba9af726970ff87be7dac0b70d5805050c6207b7bb17ea4fca")
    add_versions("home:2.78.1", "915bc3d0f8507d650ead3832e2f8fb670fce59aac4d7754a7dab6f1e6fed78b2")
    add_versions("home:2.84.1", "2b4bc2ec49611a5fc35f86aca855f2ed0196e69e53092bab6bb73396bf30789a")
    add_versions("home:2.85.0", "97cfb0466ae41fca4fa2a57a15440bee15b54ae76a12fb3cbff11df947240e48")
    add_patches("2.71.0", path.join(os.scriptdir(), "patches", "2.71.0", "macosx.patch"), "a0c928643e40f3a3dfdce52950486c7f5e6f6e9cfbd76b20c7c5b43de51d6399")

    if is_plat("mingw") and is_subhost("msys") then
        add_extsources("pacman::glib2")
    elseif is_plat("linux") then
        add_extsources("apt::libglib2.0-dev", "pacman::glib2")
    elseif is_plat("macosx") then
        add_extsources("brew::glib")
    end

    if not is_subhost("windows") then
        add_extsources("pkgconfig::glib-2.0")
    end

    if is_plat("windows", "mingw") then
        add_syslinks("user32", "shell32", "ole32", "ws2_32", "shlwapi", "iphlpapi", "dnsapi", "uuid")
    elseif is_plat("macosx") then
        add_syslinks("resolv")
        add_frameworks("AppKit", "Foundation", "CoreServices", "CoreFoundation")
    elseif is_plat("iphoneos") then
        add_syslinks("resolv", "iconv")
        add_frameworks("Foundation", "CoreFoundation", "SystemConfiguration")
    elseif is_plat("linux") then
        add_syslinks("pthread", "dl", "resolv")
    end

    add_deps("meson", "ninja", {host = true})
    add_deps("libffi", "zlib")
    if is_plat("linux") then
        add_deps("libiconv")
    elseif is_plat("macosx") then
        add_deps("libiconv", {system = true})
        add_deps("libintl")
    elseif is_plat("windows", "mingw") then
        add_deps("libintl")
        if is_subhost("windows") then
            add_deps("pkgconf")
        else
            add_deps("pkg-config")
        end
    end

    add_includedirs("include/glib-2.0", "lib/glib-2.0/include")
    add_links("gio-2.0", "gobject-2.0", "gthread-2.0", "gmodule-2.0", "glib-2.0")
    if is_plat("iphoneos") then
        -- GLib falls back to its bundled proxy-libintl on iOS. Meson installs
        -- that archive next to GLib, so expose it to consumers and to the
        -- package link test.
        add_links("intl")
    end

    on_fetch("macosx", "linux", function (package, opt)
        if opt.system and package.find_package then
            local result
            for _, name in ipairs({"gio-2.0", "gobject-2.0", "gthread-2.0", "gmodule-2.0", "glib-2.0"}) do
                local pkginfo = package.find_package and package:find_package("pkgconfig::" .. name, opt)
                if pkginfo then
                    if not result then
                        result = table.copy(pkginfo)
                    else
                        local includedirs = pkginfo.sysincludedirs or pkginfo.includedirs
                        result.links = table.wrap(result.links)
                        result.linkdirs = table.wrap(result.linkdirs)
                        result.includedirs = table.wrap(result.includedirs)
                        table.join2(result.includedirs, includedirs)
                        table.join2(result.linkdirs, pkginfo.linkdirs)
                        table.join2(result.links, pkginfo.links)
                    end
                end
            end
            return result
        end
    end)

    on_load(function (package)
        if package:gitref() or package:version():ge("2.74.0") then
          package:add("deps", "pcre2", {system = false, configs = {shared = false}})
        else
            package:add("deps", "pcre", {system = false, configs = {shared = false}})
        end
        -- force static linking of libffi to avoid version conflicts
        package:add("deps", "libffi", {system = false, configs = {shared = false}})
    end)

    on_install("windows", "macosx", "iphoneos", "linux", "cross", "mingw", function (package)
        local configs = {"-Dbsymbolic_functions=false",
                         "-Ddtrace=false",
                         "-Dman=false",
                         "-Dgtk_doc=false",
                         "-Dtests=false",
                         "-Dinstalled_tests=false",
                         "-Dsystemtap=false",
                         "-Dselinux=disabled",
                         "-Dlibmount=disabled",
                         "-Dsysprof=disabled",
                         "-Dintrospection=disabled"}
        if package:is_plat("iphoneos") then
            table.insert(configs, "-Dnls=disabled")
            table.insert(configs, "-Dxattr=false")
        end
        if package:is_plat("macosx") and package:version():le("2.61.0") then
            table.insert(configs, "-Diconv=native")
        elseif package:is_plat("windows") and package:version():le("2.74.0") then
            table.insert(configs, "-Diconv=external")
        end
        table.insert(configs, "-Dglib_debug=" .. (package:debug() and "enabled" or "disabled"))
        table.insert(configs, "-Ddefault_library=" .. (package:config("shared") and "shared" or "static"))
        table.insert(configs, "-Dgio_module_dir=" .. path.join(package:installdir(), "lib/gio/modules"))
        io.gsub("meson.build", "subdir%('tests'%)", "")
        io.gsub("meson.build", "subdir%('fuzzing'%)", "")
        io.gsub("gio/meson.build", "subdir%('tests'%)", "")
        io.replace("meson.build", "glib_conf.set('HAVE_SELINUX', selinux_dep.found())", "", {plain = true})
        if package:is_plat("windows") then
            io.gsub("meson.build", "dependency%('libffi',", "dependency('libffi', modules: ['libffi::ffi'],")
        end
        local opt = {packagedeps = {"libintl", "libiconv", "libffi", "zlib"}}
        if package:is_plat("iphoneos") then
            import("lib.detect.find_tool")
            local host_cc = assert(find_tool("clang"), "host C compiler not found")
            local host_cxx = assert(find_tool("clang++"), "host C++ compiler not found")
            local native_file = os.tmpfile() .. ".ini"
            io.writefile(native_file, format(
                "[binaries]\nc = ['%s']\ncpp = ['%s']\n",
                host_cc.program, host_cxx.program))
            table.insert(configs, "--native-file=" .. native_file)
        end
        import("package.tools.meson").install(package, configs, opt)
        package:addenv("PATH", "bin")
    end)

    on_test(function (package)
        assert(package:has_cfuncs("g_list_append", {includes = "glib.h"}))
    end)
