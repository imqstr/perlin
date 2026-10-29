# Slang shaders: every program is compiled for every graphics api and packed into one file that the engine embeds.
set(PERLIN_SLANG_VERSION "2026.20" CACHE STRING "Slang release used to compile the shaders")
set(PERLIN_DXC_RELEASE "v1.8.2502" CACHE STRING "DirectX Shader Compiler release (DXIL output)")
set(PERLIN_DXC_STAMP "2025_02_20" CACHE STRING "Date stamp in the DXC release file names")
set(PERLIN_SLANGC "" CACHE FILEPATH "slangc to compile shaders with; fetched for the build machine when empty")
set(PERLIN_DXC_LIBS "" CACHE PATH "directory with dxcompiler and dxil for DXIL output; fetched when empty")

if(NOT PERLIN_SLANGC)
    if(CMAKE_HOST_WIN32)
        set(_slang_os windows)
        set(_slang_ext zip)
    elseif(CMAKE_HOST_APPLE)
        set(_slang_os macos)
        set(_slang_ext zip)
    else()
        set(_slang_os linux)
        set(_slang_ext tar.gz)
    endif()
    if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
        set(_slang_arch aarch64)
    else()
        set(_slang_arch x86_64)
    endif()
    CPMAddPackage(NAME slang
        URL "https://github.com/shader-slang/slang/releases/download/v${PERLIN_SLANG_VERSION}/slang-${PERLIN_SLANG_VERSION}-${_slang_os}-${_slang_arch}.${_slang_ext}"
        DOWNLOAD_ONLY YES)
    if(CMAKE_HOST_WIN32)
        set(PERLIN_SLANGC "${slang_SOURCE_DIR}/bin/slangc.exe" CACHE FILEPATH "" FORCE)
    else()
        set(PERLIN_SLANGC "${slang_SOURCE_DIR}/bin/slangc" CACHE FILEPATH "" FORCE)
    endif()
endif()

set(PERLIN_SHADER_TARGETS spirv hlsl msl wgsl)
if(NOT PERLIN_DXC_LIBS AND NOT CMAKE_HOST_APPLE)
    if(CMAKE_HOST_WIN32)
        CPMAddPackage(NAME dxc
            URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${PERLIN_DXC_RELEASE}/dxc_${PERLIN_DXC_STAMP}.zip"
            DOWNLOAD_ONLY YES)
        set(PERLIN_DXC_LIBS "${dxc_SOURCE_DIR}/bin/x64" CACHE PATH "" FORCE)
    elseif(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
        CPMAddPackage(NAME dxc
            URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${PERLIN_DXC_RELEASE}/linux_dxc_${PERLIN_DXC_STAMP}.x86_64.tar.gz"
            DOWNLOAD_ONLY YES)
        set(PERLIN_DXC_LIBS "${dxc_SOURCE_DIR}/lib" CACHE PATH "" FORCE)
    endif()
endif()
set(_dxc_args)
if(PERLIN_DXC_LIBS)
    list(APPEND PERLIN_SHADER_TARGETS dxil)
    set(_dxc_args --library-path "${PERLIN_DXC_LIBS}")
else()
    message(STATUS "Perlin: no DXC for this machine, the shader pack will not carry DXIL (Direct3D 12 needs it)")
endif()
list(JOIN PERLIN_SHADER_TARGETS "," _targets)

set(PERLIN_SLANG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/slang")
file(GLOB PERLIN_SLANG_SOURCES CONFIGURE_DEPENDS "${PERLIN_SLANG_ROOT}/*.slang" "${PERLIN_SLANG_ROOT}/programs/*.slang"
     "${PERLIN_RD_SHADER_DIR}/*.slang")
set(PERLIN_SHADER_PACK "${CMAKE_CURRENT_BINARY_DIR}/shaders/perlin_shaders.pack")
add_custom_command(
    OUTPUT "${PERLIN_SHADER_PACK}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/shaders"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_shaders.py"
            --slangc "${PERLIN_SLANGC}" -I "${PERLIN_SLANG_ROOT}" -I "${PERLIN_RD_SHADER_DIR}" ${_dxc_args}
            --slots "${PERLIN_SLANG_ROOT}/texture_slots.json" --targets "${_targets}"
            --cache "${CMAKE_CURRENT_BINARY_DIR}/shaders/cache"
            -o "${PERLIN_SHADER_PACK}" "${PERLIN_SLANG_ROOT}/programs" "${PERLIN_RD_SHADER_DIR}"
    DEPENDS ${PERLIN_SLANG_SOURCES} "${PERLIN_SLANG_ROOT}/texture_slots.json" "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_shaders.py"
    COMMENT "Compiling slang shaders (${_targets})"
    VERBATIM)
add_custom_target(perlin_shader_pack DEPENDS "${PERLIN_SHADER_PACK}")
