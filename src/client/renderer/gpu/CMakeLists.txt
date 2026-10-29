# the engine's gpu layer on top of the rhi; usable on its own for the offscreen render tests
cmake_minimum_required(VERSION 3.28)
if(NOT TARGET perlin_gpu)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        project(perlin_gpu LANGUAGES C CXX)
        set(CMAKE_CXX_STANDARD 23)
        set(CMAKE_CXX_STANDARD_REQUIRED ON)
        set(CMAKE_CXX_EXTENSIONS OFF)
        set(PERLIN_GPU_STANDALONE ON)
        add_compile_definitions(GLM_FORCE_DEPTH_ZERO_TO_ONE)
        set(PERLIN_GLM_DIR "" CACHE PATH "glm checkout for standalone builds")
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../rhi" rhi)
        add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../render" render)
        enable_testing()
    endif()

    add_library(perlin_gpu STATIC
        GpuCore.cpp
        ChunkMeshStore.cpp
        RenderContext_rhi.cpp
        RenderContext_rhi_java.cpp
        RenderContext_rhi_post.cpp
        RenderContext_rhi_shadow.cpp
        RenderContext_rhi_ui.cpp
        RenderContext_rhi_world.cpp)
    target_include_directories(perlin_gpu PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
    target_link_libraries(perlin_gpu PUBLIC perlin_rhi)
    if(PERLIN_GLM_DIR)
        target_include_directories(perlin_gpu PUBLIC "${PERLIN_GLM_DIR}")
    endif()
endif()

if(PERLIN_GPU_STANDALONE)
    set(PERLIN_MCBE_SHADER_SOURCE_DIR "" CACHE PATH "mcbe-shader-codebase checkout for the material catalog")
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(PERLIN_MATERIAL_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/materials")
    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tools/gen_material_catalog.py"
                "${PERLIN_MCBE_SHADER_SOURCE_DIR}" "${PERLIN_MATERIAL_INCLUDE_DIR}/MaterialCatalog.hpp"
        COMMAND_ERROR_IS_FATAL ANY)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../MaterialDb.cpp" APPEND PROPERTY INCLUDE_DIRECTORIES "${PERLIN_MATERIAL_INCLUDE_DIR}")
    add_executable(gpu_context_test
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/gpu_context_test.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/gpu_context_stubs.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../Tessellator.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../SkyGeometry.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../MaterialDb.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../ui/Json.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../util/Paths.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../util/Resources.cpp")
    target_include_directories(gpu_context_test BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../tests/stubs")
    target_link_libraries(gpu_context_test PRIVATE perlin_gpu)
    # a pack built by tools/pack_shaders.py with the targets of the backends enabled here
    set(PERLIN_TEST_PACK "" CACHE FILEPATH "shader pack for gpu_context_test")
    if(PERLIN_TEST_PACK)
        foreach(api VULKAN WEBGPU D3D12 D3D11 METAL)
            if(PERLIN_RHI_${api})
                string(TOLOWER ${api} id)
                add_test(NAME gpu_context_${id} COMMAND gpu_context_test "${PERLIN_TEST_PACK}" ${id})
                set_tests_properties(gpu_context_${id} PROPERTIES SKIP_RETURN_CODE 77)
            endif()
        endforeach()
    endif()
endif()
