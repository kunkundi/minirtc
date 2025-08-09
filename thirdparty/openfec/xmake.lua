package("openfec")

    set_homepage("http://openfec.inrialpes.fr/")
    set_description("Application-Level Forward Erasure Correction codes.")
    set_license("CeCCIL-C")

    add_urls("https://github.com/kunkundi/openfec/archive/refs/tags/$(version).tar.gz",
             "https://github.com/kunkundi/openfec.git")

    add_versions("1.4.2", "5aa19dc56038f4b19927efb53ca28e896fb05f068a4715de512f0709420f03be")

    add_deps("cmake")

    on_install(function (package)
        local configs = {}
        table.insert(configs, "-DDEBUG=" .. (package:debug() and "ON" or "OFF"))
        table.insert(configs, "-DCMAKE_INSTALL_PREFIX=" .. package:installdir())
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))

        import("package.tools.cmake").install(package, configs)

        package:add("includedirs", "include")
    end)

    on_test(function (package)
        assert(package:has_cfuncs("of_create_codec_instance", {includes = "lib_common/of_openfec_api.h"}))
    end)
