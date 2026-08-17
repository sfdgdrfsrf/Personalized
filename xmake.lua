-- xmake.lua — Build configuration for Personalized (LeviLamina native mod).
--
-- Build modes:
--
--   1. STANDALONE (stub headers, no SDK — default):
--        xmake f -p android -a arm64-v8a --ndk=$ANDROID_NDK_ROOT -m release
--        xmake
--
--   2. WITH real LeviLamina SDK:
--        xmake f -p windows -a x64 -m release --sdk=levilamina
--        xmake
--
--   3. CI / Android cross-compile:
--        xmake f -p android -a arm64-v8a --ndk=$ANDROID_NDK_ROOT -m release -y
--        xmake -y

-- ─────────────────────────────────────────────
--  Project definition
-- ─────────────────────────────────────────────
add_rules("mode.debug", "mode.release")
set_languages("cxx20")

-- ─────────────────────────────────────────────
--  Option: --sdk=levilamina to use real SDK
-- ─────────────────────────────────────────────
option("sdk")
    set_default("standalone")
    set_show(true)
    set_description("SDK mode: 'levilamina' for real SDK, 'standalone' for stub headers")
option_end()

target("Personalized")
    set_kind("shared")
    set_basename("Personalized")

    -- ── Source files ──
    add_files("src/**.cpp")

    -- ── Header search paths ──
    -- src/ is always first (project's own headers)
    add_includedirs("src", {public = true})

    -- ── Stub headers are ALWAYS available as fallback ──
    -- They come LAST in include order so the real SDK wins if present.
    add_includedirs("include/stubs")

    -- Register the --sdk option so get_config works
    add_options("sdk")

    -- ── SDK-specific overrides ──
    -- If --sdk=levilamina, add SDK include dirs BEFORE stubs
    -- so the real headers take priority.
    on_load(function (target)
        local sdk = get_config("sdk")
        if sdk == "levilamina" then
            -- Real SDK mode: add LeviLamina + BDS headers
            -- These paths are checked BEFORE stubs/
            target:add("includedirs", "$(projectdir)/LeviLamina/include", {public = true})
            target:add("includedirs", "$(projectdir)/BDS/include", {public = true})
            target:add("includedirs", "third_party/nlohmann", {public = true})
            target:add("links", "LeviLamina")
            target:add("defines", "PERSONALIZED_USE_REAL_SDK")
        else
            -- Standalone mode — stubs are already on the include path
            target:add("defines", "PERSONALIZED_STANDALONE")
        end
    end)

    -- ── Platform-specific system libraries ──
    if is_plat("windows") then
        add_syslinks("kernel32", "user32", "ntdll")
    elseif is_plat("android") then
        add_syslinks("log")  -- Android logcat
    elseif is_plat("linux") then
        add_syslinks("pthread", "dl")
    end

    -- ── Compiler flags ──
    if is_plat("windows") then
        add_cxxflags("/EHsc", {force = true})
        add_cxflags("/W3", {force = true})
        add_defines("NOMINMAX", "WIN32_LEAN_AND_MEAN")
    else
        -- Clang/GCC flags (Android NDK, Linux, etc.)
        add_cxxflags("-fexceptions", {force = true})
        add_cxxflags("-frtti", {force = true})
        add_cxxflags("-Wall", "-Wextra", "-Wno-unused-parameter", {force = true})
    end

    -- Suppress noisy warnings from stub headers
    add_cxxflags("-Wno-unused-function", "-Wno-unused-variable", "-Wno-format", "-Wno-format-extra-args", {force = true})

    -- Debug-specific
    if is_mode("debug") then
        add_defines("DEBUG", "_DEBUG")
        if is_plat("windows") then
            add_cxxflags("/Zi", "/Od", {force = true})
        else
            add_cxxflags("-g", "-O0", {force = true})
        end
    end

    -- Release-specific
    if is_mode("release") then
        add_defines("NDEBUG")
        if is_plat("windows") then
            add_cxxflags("/O2", "/GL", {force = true})
            add_ldflags("/LTCG", {force = true})
        else
            add_cxxflags("-O2", {force = true})
        end
    end

    -- ── Output configuration ──
    after_build(function (target)
        local output = target:targetdir() .. "/" .. target:basename()
        if is_plat("windows") then
            output = output .. ".dll"
        else
            output = output .. ".so"
        end
        print("Built: " .. output)
        if is_plat("android") then
            print("Deploy to: <device>/plugins/Personalized/Personalized.so")
        else
            print("Deploy to: <server>/plugins/Personalized/Personalized.dll")
        end
    end)

-- ─────────────────────────────────────────────
--  Test target (standalone, no BDS deps)
-- ─────────────────────────────────────────────
target("PersonalizedTest")
    set_kind("binary")
    set_default(false)

    add_files("src/Utils/RandomMapper.cpp")
    add_files("tests/TestRandomMapper.cpp")

    add_includedirs("src")
    add_includedirs("include/stubs")

    if is_plat("windows") then
        add_cxxflags("/EHsc", {force = true})
    else
        add_cxxflags("-fexceptions", "-frtti", {force = true})
    end
