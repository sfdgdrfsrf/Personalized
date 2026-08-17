-- xmake.lua — Personalized mod for LeviLauncher (Android)
-- Build: xmake f -y -p android -a arm64-v8a -m release --ndk=$ANDROID_NDK_ROOT
--        xmake -y

add_rules("mode.debug", "mode.release")
set_policy("package.requires_lock", true)

add_repositories("levimc-repo https://github.com/LiteLDev/xmake-repo.git")

-- Preloader-android provides the mod loading framework (PL_REGISTER_MOD, hooking, etc.)
package("preloader")
    set_homepage("https://github.com/LiteLDev/preloader-android")
    set_description("Preloader Android")
    add_urls("https://github.com/LiteLDev/preloader-android.git")
    add_versions("main", "main")
    add_deps("cmake")
    on_install("android", function (package)
        import("package.tools.cmake").install(package)
    end)
package_end()

add_requires("preloader")
add_requires("nlohmann_json v3.11.3")
add_requires("fmt")
add_requires("magic_enum v0.9.7")
add_requires("glm")
add_requires("pfr 2.1.1")

target("Personalized")
    set_kind("shared")
    set_languages("c++20")
    set_strip("all")

    add_files("src/**.cpp")

    add_includedirs("include", {public = true})
    add_includedirs("src")

    add_packages("preloader", "nlohmann_json", "fmt", "magic_enum", "glm", "pfr")

    if is_plat("android") then
        add_cxflags("-fPIC", "-O2", "-ffunction-sections", "-fdata-sections", "-fexceptions", "-frtti", "-fno-stack-protector", "-w", "-fvisibility=hidden")
        add_cxxflags("-fvisibility-inlines-hidden")
        add_shflags("-Wl,--gc-sections", "-Wl,--icf=all", "-Wl,--hash-style=gnu", "-Wl,-z,max-page-size=16384")
        add_links("android", "log", "EGL", "GLESv3", "GLESv2")
    end

    if is_mode("debug") then
        add_defines("DEBUG", "_DEBUG")
    else
        add_defines("NDEBUG")
    end
