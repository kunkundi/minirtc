package("libffi")
    set_homepage("https://sourceware.org/libffi/")
    set_description("Portable Foreign Function Interface for HarmonyOS")
    set_license("MIT")
    add_urls("https://github.com/libffi/libffi/releases/download/v$(version)/libffi-$(version).tar.gz")
    add_versions("3.4.8", "bc9842a18898bfacb0ed1252c4febcc7e78fa139fd27fdc7a3e30d9d9356119b")
    on_install("harmony", function (package)
        import("package.tools.autoconf").install(package, {
            "--host=aarch64-linux-musl", "--disable-shared", "--enable-static",
            "--disable-docs", "--disable-multi-os-directory", "--with-pic"
        })
    end)
    on_test(function (package)
        assert(package:has_cfuncs("ffi_call", {includes = "ffi.h"}))
    end)
