# Steinberg's VST3 validator, built here from the SDK's MIT-licensed sources (vst3_base and vst3_public_sdk, at the
# tag CMakeLists.txt pins), as CMakeLists.txt builds it for Windows. `ctest` runs it on the bundle, and `validate`
# does (`--target validate-vst3`).
#
# The SDK parts come from VST3_SDK_ROOT where it is given (build.sh clones the tag's repositories there, since a cloud
# session's proxy refuses GitHub's archives), else they are downloaded beside the pluginterfaces.

get_filename_component(_fmvibe_sdk_root "${VST3_PLUGINTERFACES_DIR}" DIRECTORY)

if (NOT EXISTS "${_fmvibe_sdk_root}/public.sdk/samples/vst-hosting/validator/CMakeLists.txt")
    set(_fmvibe_archive ${CMAKE_BINARY_DIR}/vst3_public_sdk.tar.gz)
    message(STATUS "Fetching the VST3 SDK's public part (the validator's sources)")
    file(DOWNLOAD
        https://github.com/steinbergmedia/vst3_public_sdk/archive/refs/tags/v3.8.1_build_84.tar.gz
        ${_fmvibe_archive}
        EXPECTED_HASH SHA256=f683db0c831dd6603a1bb8c7747a6e1dd223ac3423e288b08c5706cf30b76ae8)
    file(ARCHIVE_EXTRACT INPUT ${_fmvibe_archive} DESTINATION ${_fmvibe_sdk_root}/.extract)
    file(RENAME ${_fmvibe_sdk_root}/.extract/vst3_public_sdk-3.8.1_build_84 ${_fmvibe_sdk_root}/public.sdk)
endif()

if (NOT EXISTS "${_fmvibe_sdk_root}/base/source/baseiids.cpp")
    set(_fmvibe_archive ${CMAKE_BINARY_DIR}/vst3_base.tar.gz)
    message(STATUS "Fetching the VST3 SDK's base part (the validator's sources)")
    file(DOWNLOAD
        https://github.com/steinbergmedia/vst3_base/archive/refs/tags/v3.8.1_build_84.tar.gz
        ${_fmvibe_archive}
        EXPECTED_HASH SHA256=3ac62b2e271b87863ce50399b40549c204fc21065134247f87d8b84381692ced)
    file(ARCHIVE_EXTRACT INPUT ${_fmvibe_archive} DESTINATION ${_fmvibe_sdk_root}/.extract)
    file(RENAME ${_fmvibe_sdk_root}/.extract/vst3_base-3.8.1_build_84 ${_fmvibe_sdk_root}/base)
endif()

# The sources include each other as pluginterfaces/..., so the SDK root holds that name too.
if (NOT EXISTS "${_fmvibe_sdk_root}/pluginterfaces/base/funknown.h")
    file(CREATE_LINK "${VST3_PLUGINTERFACES_DIR}" "${_fmvibe_sdk_root}/pluginterfaces" SYMBOLIC)
endif()

set(SDK_ROOT ${_fmvibe_sdk_root})
set(SMTG_LINUX ON)
set(SMTG_WIN OFF)

# What the validator's own CMakeLists.txt expects of the SDK's build: its root, the hosting library (built here with
# the SDK's sources, as CMakeLists.txt does for Windows) and a few helpers, which do nothing here.
function(smtg_target_codesign)
endfunction()
function(smtg_target_setup_universal_binary)
endfunction()

add_library(sdk_hosting STATIC
    ${SDK_ROOT}/base/source/baseiids.cpp
    ${SDK_ROOT}/base/source/fbuffer.cpp
    ${SDK_ROOT}/base/source/fdebug.cpp
    ${SDK_ROOT}/base/source/fdynlib.cpp
    ${SDK_ROOT}/base/source/fobject.cpp
    ${SDK_ROOT}/base/source/fstreamer.cpp
    ${SDK_ROOT}/base/source/fstring.cpp
    ${SDK_ROOT}/base/source/timer.cpp
    ${SDK_ROOT}/base/source/updatehandler.cpp
    ${SDK_ROOT}/base/thread/source/fcondition.cpp
    ${SDK_ROOT}/base/thread/source/flock.cpp
    ${SDK_ROOT}/pluginterfaces/base/conststringtable.cpp
    ${SDK_ROOT}/pluginterfaces/base/coreiids.cpp
    ${SDK_ROOT}/pluginterfaces/base/funknown.cpp
    ${SDK_ROOT}/pluginterfaces/base/ustring.cpp
    ${SDK_ROOT}/public.sdk/source/common/commoniids.cpp
    ${SDK_ROOT}/public.sdk/source/common/commonstringconvert.cpp
    ${SDK_ROOT}/public.sdk/source/common/memorystream.cpp
    ${SDK_ROOT}/public.sdk/source/common/threadchecker_linux.cpp
    ${SDK_ROOT}/public.sdk/source/vst/hosting/connectionproxy.cpp
    ${SDK_ROOT}/public.sdk/source/vst/hosting/eventlist.cpp
    ${SDK_ROOT}/public.sdk/source/vst/hosting/hostclasses.cpp
    ${SDK_ROOT}/public.sdk/source/vst/hosting/module.cpp
    ${SDK_ROOT}/public.sdk/source/vst/hosting/parameterchanges.cpp
    ${SDK_ROOT}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
    ${SDK_ROOT}/public.sdk/source/vst/hosting/processdata.cpp
    ${SDK_ROOT}/public.sdk/source/vst/utility/stringconvert.cpp
    ${SDK_ROOT}/public.sdk/source/vst/vstinitiids.cpp
    ${SDK_ROOT}/public.sdk/source/vst/vstpresetfile.cpp
)
target_include_directories(sdk_hosting PUBLIC ${SDK_ROOT})
target_compile_features(sdk_hosting PUBLIC cxx_std_17)
target_compile_definitions(sdk_hosting PUBLIC $<IF:$<CONFIG:Debug>,DEVELOPMENT=1,RELEASE=1>)
target_link_libraries(sdk_hosting PUBLIC Threads::Threads ${CMAKE_DL_LIBS})

add_subdirectory(${SDK_ROOT}/public.sdk/samples/vst-hosting/validator vst3-validator)

add_test(NAME vst3-validator COMMAND validator ${FMVIBE_VST3_BUNDLE})
add_custom_target(validate-vst3
    COMMAND validator ${FMVIBE_VST3_BUNDLE}
    DEPENDS validator FMVibe-vst3
    USES_TERMINAL
)
add_dependencies(validate validate-vst3)
