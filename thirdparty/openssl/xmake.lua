-- iOS builds certificates/keys in memory and verifies signaling certificates
-- using Apple Security. No system configuration or filesystem store is needed.
package("openssl3")
    set_homepage("https://www.openssl.org/")
    set_description("OpenSSL with mobile platform build configurations")
    set_license("Apache-2.0")
    add_urls("https://github.com/openssl/openssl/archive/refs/tags/openssl-$(version).zip")
    add_versions("3.3.2", "4cda357946f9dd5541b565dba35348d614288e88aeb499045018970c789c9d61")
    add_configs("shared", {default = false, type = "boolean", readonly = true})
    if is_plat("iphoneos") then
        add_configs("ios_privacy_revision", {default = "1", type = "string", readonly = true})
    elseif is_plat("android") then
        add_syslinks("dl")
    end
    add_links("ssl", "crypto")

    on_install("iphoneos", function (package)
        -- Retain stdio API compatibility for Asio, but exclude metadata calls
        -- and the unused file URI provider. OSSL_STORE file URLs intentionally
        -- fail as unsupported; memory BIO certificate/key APIs are unchanged.
        local stores = "providers/implementations/storemgmt/build.info"
        local registration = "providers/stores.inc"
        assert(io.readfile(stores):find("SOURCE[$STORE_GOAL]=file_store.c file_store_any2obj.c", 1, true),
            "Review OpenSSL store sources before updating the iOS recipe")
        assert(io.readfile(registration):find('STORE("file", "yes", ossl_file_store_functions)', 1, true),
            "Review OpenSSL store registration before updating the iOS recipe")
        io.replace(stores, "SOURCE[$STORE_GOAL]=file_store.c file_store_any2obj.c", "", {plain = true})
        io.replace(registration, 'STORE("file", "yes", ossl_file_store_functions)', "", {plain = true})

        local xcode = package:toolchain("xcode")
        local simulator = xcode and xcode:config("appledev") == "simulator"
        local configs = {
            simulator and "iossimulator-xcrun" or "ios64-cross",
            "no-shared", "no-tests", "no-posix-io", "no-autoload-config",
            -- iOS >= 16 provides getentropy (supported since iOS 10). Do not
            -- compile the /dev/urandom fallback that inspects device metadata.
            "--with-rand-seed=getrandom", "--libdir=lib",
            "--prefix=" .. package:installdir(),
            "--openssldir=" .. package:installdir()
        }
        if package:debug() then
            table.insert(configs, "--debug")
        end
        local buildenvs = import("package.tools.autoconf").buildenvs(package)
        os.vrunv("./Configure", configs, {envs = buildenvs})
        import("package.tools.make").build(package, {CFLAGS = buildenvs.CFLAGS, ASFLAGS = buildenvs.ASFLAGS})
        import("package.tools.make").make(package, {"install_sw"})
    end)

    on_install("android", function (package)
        -- Use the NDK archive indexer, never the host's ranlib.
        local envs = import("package.tools.autoconf").buildenvs(package)
        envs.RANLIB = envs.AR:gsub("llvm%-ar", "llvm-ranlib")
        local configs = {"linux-aarch64", "no-shared", "no-tests", "no-module",
            "--libdir=lib", "--prefix=" .. package:installdir(),
            "--openssldir=" .. package:installdir()}
        os.vrunv("./Configure", configs, {envs = envs})
        import("package.tools.make").build(package, {RANLIB = envs.RANLIB})
        import("package.tools.make").make(package, {"install_sw", "RANLIB=" .. envs.RANLIB})
    end)

    on_test(function (package)
        assert(package:has_cfuncs("SSL_new", {includes = "openssl/ssl.h"}))
    end)
