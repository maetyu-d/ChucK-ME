cmake_minimum_required(VERSION 3.22)

project(WeldChucK VERSION 0.1.10 LANGUAGES C CXX)
enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(JUCE_PATH "/Users/user/Documents/Fabric/JUCE" CACHE PATH "Path to a JUCE checkout")
add_subdirectory("${JUCE_PATH}" JUCE)

set(CHUCK_CORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/chuck/src/core")

add_library(chuck_core STATIC
    ${CHUCK_CORE_DIR}/chuck_yacc.c
    ${CHUCK_CORE_DIR}/util_network.c
    ${CHUCK_CORE_DIR}/util_raw.c
    ${CHUCK_CORE_DIR}/util_sndfile.c
    ${CHUCK_CORE_DIR}/util_xforms.c
    ${CHUCK_CORE_DIR}/lo/address.c
    ${CHUCK_CORE_DIR}/lo/blob.c
    ${CHUCK_CORE_DIR}/lo/bundle.c
    ${CHUCK_CORE_DIR}/lo/message.c
    ${CHUCK_CORE_DIR}/lo/method.c
    ${CHUCK_CORE_DIR}/lo/pattern_match.c
    ${CHUCK_CORE_DIR}/lo/send.c
    ${CHUCK_CORE_DIR}/lo/server.c
    ${CHUCK_CORE_DIR}/lo/server_thread.c
    ${CHUCK_CORE_DIR}/lo/timetag.c
    ${CHUCK_CORE_DIR}/chuck.cpp
    ${CHUCK_CORE_DIR}/chuck_absyn.cpp
    ${CHUCK_CORE_DIR}/chuck_carrier.cpp
    ${CHUCK_CORE_DIR}/chuck_compile.cpp
    ${CHUCK_CORE_DIR}/chuck_dl.cpp
    ${CHUCK_CORE_DIR}/chuck_emit.cpp
    ${CHUCK_CORE_DIR}/chuck_errmsg.cpp
    ${CHUCK_CORE_DIR}/chuck_frame.cpp
    ${CHUCK_CORE_DIR}/chuck_globals.cpp
    ${CHUCK_CORE_DIR}/chuck_instr.cpp
    ${CHUCK_CORE_DIR}/chuck_io.cpp
    ${CHUCK_CORE_DIR}/chuck_lang.cpp
    ${CHUCK_CORE_DIR}/chuck_oo.cpp
    ${CHUCK_CORE_DIR}/chuck_parse.cpp
    ${CHUCK_CORE_DIR}/chuck_scan.cpp
    ${CHUCK_CORE_DIR}/chuck_stats.cpp
    ${CHUCK_CORE_DIR}/chuck_symbol.cpp
    ${CHUCK_CORE_DIR}/chuck_table.cpp
    ${CHUCK_CORE_DIR}/chuck_type.cpp
    ${CHUCK_CORE_DIR}/chuck_ugen.cpp
    ${CHUCK_CORE_DIR}/chuck_vm.cpp
    ${CHUCK_CORE_DIR}/uana_extract.cpp
    ${CHUCK_CORE_DIR}/uana_xform.cpp
    ${CHUCK_CORE_DIR}/ugen_filter.cpp
    ${CHUCK_CORE_DIR}/ugen_osc.cpp
    ${CHUCK_CORE_DIR}/ugen_stk.cpp
    ${CHUCK_CORE_DIR}/ugen_xxx.cpp
    ${CHUCK_CORE_DIR}/ulib_ai.cpp
    ${CHUCK_CORE_DIR}/ulib_doc.cpp
    ${CHUCK_CORE_DIR}/ulib_machine.cpp
    ${CHUCK_CORE_DIR}/ulib_math.cpp
    ${CHUCK_CORE_DIR}/ulib_opsc.cpp
    ${CHUCK_CORE_DIR}/ulib_std.cpp
    ${CHUCK_CORE_DIR}/util_buffers.cpp
    ${CHUCK_CORE_DIR}/util_console.cpp
    ${CHUCK_CORE_DIR}/util_hid.cpp
    ${CHUCK_CORE_DIR}/util_math.cpp
    ${CHUCK_CORE_DIR}/util_opsc.cpp
    ${CHUCK_CORE_DIR}/util_platforms.cpp
    ${CHUCK_CORE_DIR}/util_serial.cpp
    ${CHUCK_CORE_DIR}/util_string.cpp
    ${CHUCK_CORE_DIR}/util_thread.cpp)

target_include_directories(chuck_core
    SYSTEM PUBLIC
        ${CHUCK_CORE_DIR}
        ${CHUCK_CORE_DIR}/lo)

target_compile_definitions(chuck_core
    PUBLIC
        __MACOSX_CORE__
        __DISABLE_HID__
        __DISABLE_MIDI__
        __DISABLE_SERIAL__
        __DISABLE_SHELL__
        __DISABLE_OTF_SERVER__
        __DISABLE_NETWORK__
        __DISABLE_KBHIT__
        __DISABLE_PROMPTER__
        __USE_CHUCK_YACC__)

target_compile_options(chuck_core
    PRIVATE
        -Wno-deprecated-declarations
        -Wno-format
        -Wno-unused-variable
        -Wno-nontrivial-memcall
        -Wno-deprecated-copy-with-user-provided-copy
        -Wno-zero-as-null-pointer-constant
        -Wno-nested-anon-types
        -Wno-sign-conversion
        -Wno-extra-semi)

add_library(weld_chuck_engine STATIC)
add_library(WeldChucK::Engine ALIAS weld_chuck_engine)

target_sources(weld_chuck_engine
    PRIVATE
        Source/WeldChucK/EmbeddedChucKEngine.cpp
        Source/WeldChucK/EmbeddedChucKEngine.h
        Source/WeldChucK/WeldChucKEngine.h)

target_include_directories(weld_chuck_engine
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Source/WeldChucK
    PRIVATE
        ${JUCE_PATH}/modules)

target_link_libraries(weld_chuck_engine
    PRIVATE
        chuck_core
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
    INTERFACE
        juce::juce_audio_basics
        juce::juce_core
        juce::juce_recommended_config_flags)

target_compile_definitions(weld_chuck_engine
    PRIVATE
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        JUCE_MODULE_AVAILABLE_juce_audio_basics=1
        JUCE_MODULE_AVAILABLE_juce_core=1
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

juce_add_console_app(WeldChucK
    PRODUCT_NAME "Weld ChucK"
    COMPANY_NAME "Codex"
    BUNDLE_ID "com.codex.weldchuck")

target_sources(WeldChucK
    PRIVATE
        Source/Main.cpp)

target_link_libraries(WeldChucK
    PRIVATE
        WeldChucK::Engine
        juce::juce_audio_devices
        juce::juce_data_structures
        juce::juce_events
    PUBLIC
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

juce_add_gui_app(wf
    PRODUCT_NAME "ChucK-ME"
    COMPANY_NAME "Codex"
    BUNDLE_ID "com.codex.chuckme")

target_sources(wf
    PRIVATE
        Source/WfApp/Main.cpp
        Source/WfApp/WfChucKPrograms.h)

target_include_directories(wf
    PRIVATE
        Source/WfApp)

target_link_libraries(wf
    PRIVATE
        WeldChucK::Engine
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_gui_extra
        "-framework CoreAudioKit"
    PUBLIC
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

target_compile_definitions(wf
    PRIVATE
        JUCE_PLUGINHOST_AU=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_PLUGINHOST_VST=0
        JUCE_PLUGINHOST_LADSPA=0
        JUCE_PLUGINHOST_LV2=0)

if(APPLE)
    add_custom_command(TARGET wf POST_BUILD
        COMMAND /usr/bin/codesign --force --deep --sign - "$<TARGET_BUNDLE_DIR:wf>"
        COMMENT "Ad-hoc signing ChucK-ME.app")
endif()

add_executable(WeldChucKEngineBoundaryTest
    Tests/EngineBoundaryTest.cpp)

target_link_libraries(WeldChucKEngineBoundaryTest
    PRIVATE
        WeldChucK::Engine
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

add_executable(WfProgramTest
    Tests/WfProgramTest.cpp)

target_include_directories(WfProgramTest
    PRIVATE
        Source/WfApp)

target_link_libraries(WfProgramTest
    PRIVATE
        WeldChucK::Engine
        juce::juce_audio_formats
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

add_test(NAME WeldChucKSelfTest COMMAND WeldChucK --self-test)
add_test(NAME WeldChucKStressTest COMMAND WeldChucK --stress-test)
add_test(NAME WeldChucKCallbackTest COMMAND WeldChucK --callback-test)
add_test(NAME WeldChucKFuzzTest COMMAND WeldChucK --fuzz-test)
add_test(NAME WeldChucKProgramTest COMMAND WeldChucK --program-test)
add_test(NAME WeldChucKParameterTest COMMAND WeldChucK --parameter-test)
add_test(NAME WeldChucKAsyncProgramTest COMMAND WeldChucK --async-program-test)
add_test(NAME WeldChucKBoundaryTest COMMAND WeldChucK --boundary-test)
add_test(NAME WeldChucKConcurrencyTest COMMAND WeldChucK --concurrency-test)
add_test(NAME WeldChucKEngineBoundaryTest COMMAND WeldChucKEngineBoundaryTest)
add_test(NAME WfProgramTest COMMAND WfProgramTest)

set_tests_properties(WeldChucKSelfTest
                     WeldChucKStressTest
                     WeldChucKCallbackTest
                     WeldChucKFuzzTest
                     WeldChucKProgramTest
                     WeldChucKParameterTest
                     WeldChucKAsyncProgramTest
                     WeldChucKBoundaryTest
                     WeldChucKConcurrencyTest
                     WeldChucKEngineBoundaryTest
                     WfProgramTest
    PROPERTIES
        TIMEOUT 30)

add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    DEPENDS WeldChucK wf WeldChucKEngineBoundaryTest WfProgramTest)
