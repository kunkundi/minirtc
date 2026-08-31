package("libyuv")

    set_homepage("https://chromium.googlesource.com/libyuv/libyuv/")
    set_description("libyuv is an open source project that includes YUV scaling and conversion functionality.")
    set_license("BSD-3-Clause")
    set_urls("https://github.com/kunkundi/libyuv.git")
    add_versions("2025.8.14", "ec10b61c58ee4b8fbe648d2744d9dad9ccba6430")

    add_deps("cmake")

    on_install("windows", "linux", "macosx", "iphoneos", "android", "cross", "bsd", "mingw", function (package)
        local configs = {"-DUNIT_TEST=OFF"}
        table.insert(configs, "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"))

        if package:is_plat("iphoneos") then
            -- libyuv's upstream CMake file does not recognize Xcode's iOS
            -- toolchain processor consistently, which omits the arm64 NEON
            -- objects and then fails while linking its optional dylib.
            io.replace("CMakeLists.txt",
                [[string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" arch_lowercase)]],
                [[set(arch_lowercase "arm64")]], {plain = true})
            io.replace("CMakeLists.txt",
                [[add_library( ${ly_lib_shared} SHARED ${ly_lib_parts})]],
                [[if(NOT IOS)
add_library( ${ly_lib_shared} SHARED ${ly_lib_parts})]], {plain = true})
            io.replace("CMakeLists.txt",
                [[if(WIN32)
  set_target_properties( ${ly_lib_shared} PROPERTIES IMPORT_PREFIX "lib" )
endif()]],
                [[if(WIN32)
  set_target_properties( ${ly_lib_shared} PROPERTIES IMPORT_PREFIX "lib" )
endif()
endif()]], {plain = true})
            io.replace("CMakeLists.txt",
                [[install ( TARGETS ${ly_lib_shared} LIBRARY DESTINATION lib RUNTIME DESTINATION bin ARCHIVE DESTINATION lib )]],
                [[if(NOT IOS)
install ( TARGETS ${ly_lib_shared} LIBRARY DESTINATION lib RUNTIME DESTINATION bin ARCHIVE DESTINATION lib )
endif()]], {plain = true})
            io.replace("CMakeLists.txt",
                [[  target_link_libraries( ${ly_lib_shared} ${JPEG_LIBRARY} )]],
                [[  if(TARGET ${ly_lib_shared})
    target_link_libraries( ${ly_lib_shared} ${JPEG_LIBRARY} )
  endif()]], {plain = true})
        end

        -- Recent libyuv releases unconditionally enable I8MM and SVE2 for
        -- AArch64. Older compilers (for example GCC 9) reject those -march
        -- modifiers, so retain the optimized objects only when the compiler
        -- accepts their flags.
        io.replace("CMakeLists.txt",
            "    target_compile_options(${ly_lib_name}_neon64 PRIVATE -march=armv8.2-a+dotprod+i8mm)",
            [[    include(CheckCXXCompilerFlag)
    check_cxx_compiler_flag("-march=armv8.2-a+dotprod+i8mm" LIBYUV_CAN_COMPILE_I8MM)
    if(LIBYUV_CAN_COMPILE_I8MM)
      target_compile_options(${ly_lib_name}_neon64 PRIVATE -march=armv8.2-a+dotprod+i8mm)
    else()
      target_compile_options(${ly_lib_name}_neon64 PRIVATE -march=armv8.2-a+dotprod)
      target_compile_definitions(${ly_lib_name}_neon64 PRIVATE LIBYUV_NEEDS_I8MM_ASM_DIRECTIVE)
    endif()]],
            {plain = true})
        io.replace("CMakeLists.txt",
            [[    # Enable AArch64 SVE kernels.
    add_library(${ly_lib_name}_sve OBJECT
      ${ly_src_dir}/row_sve.cc)
    target_compile_options(${ly_lib_name}_sve PRIVATE -march=armv8.5-a+i8mm+sve2)
    list(APPEND ly_lib_parts $<TARGET_OBJECTS:${ly_lib_name}_sve>)]],
            [[    # Enable AArch64 SVE kernels when supported by the compiler.
    check_cxx_compiler_flag("-march=armv8.5-a+i8mm+sve2" LIBYUV_CAN_COMPILE_SVE2)
    if(LIBYUV_CAN_COMPILE_SVE2)
      add_library(${ly_lib_name}_sve OBJECT
        ${ly_src_dir}/row_sve.cc)
      target_compile_options(${ly_lib_name}_sve PRIVATE -march=armv8.5-a+i8mm+sve2)
      list(APPEND ly_lib_parts $<TARGET_OBJECTS:${ly_lib_name}_sve>)
    else()
      add_definitions(-DLIBYUV_DISABLE_SVE)
    endif()]],
            {plain = true})
        io.replace("source/row_neon64.cc",
            [[#include "libyuv/row.h"]],
            [[#include "libyuv/row.h"

#if defined(LIBYUV_NEEDS_I8MM_ASM_DIRECTIVE)
asm(".arch_extension i8mm");
#endif]],
            {plain = true})

        io.replace("CMakeLists.txt", "INSTALL ( PROGRAMS ${CMAKE_BINARY_DIR}/yuvconvert			DESTINATION bin )", "", {plain = true})
        import("package.tools.cmake").install(package, configs)
        
        if package:is_plat("linux", "android") then
            if package:config("shared") then 
                os.tryrm(package:installdir("lib", "*.a"))
            else 
                os.tryrm(package:installdir("lib", "*.so"))
            end
        end
        if package:is_plat("macosx") then
            if package:config("shared") then 
                os.tryrm(package:installdir("lib", "*.a"))
            else 
                os.tryrm(package:installdir("lib", "*.dylib"))
            end
        end
    end)

    on_test(function (package)
        assert(package:has_cfuncs("I420Rotate", {includes = "libyuv/rotate.h"}))
    end)
