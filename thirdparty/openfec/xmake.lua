package("openfec")

    set_homepage("http://openfec.inrialpes.fr/")
    set_description("Application-Level Forward Erasure Correction codes.")
    set_license("CeCCIL-C")
    add_versions("1.4.2", "bd1cf0fe466fb7a2ed5c3bcd8840c23dc491dbccfc8e1332989f228f8fb4ec04")

    set_sourcedir(os.scriptdir())

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
