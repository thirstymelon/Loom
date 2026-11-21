cmake_minimum_required(VERSION 3.20)
project(Loom VERSION 1.4.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra)
endif()
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

add_executable(loomc src/main.cpp)
add_executable(loom src/driver.cpp)
add_executable(loom-lsp src/lsp.cpp)
add_library(loom_rt STATIC
  src/runtime/rt_core.c
  src/runtime/rt_smart_ptr.c
  src/runtime/rt_collections.c
  src/runtime/rt_threads.c
  src/runtime/rt_net.c
  src/runtime/rt_hash.c
)
target_include_directories(loom_rt PUBLIC src/runtime)
target_compile_options(loom_rt PRIVATE -ffunction-sections -fdata-sections)
install(TARGETS loom loomc loom-lsp RUNTIME DESTINATION bin)
install(TARGETS loom_rt ARCHIVE DESTINATION lib)
target_include_directories(loomc SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(LLVM_LIBS Core Support Target CodeGen MC native aarch64codegen aarch64asmparser aarch64desc aarch64info aarch64utils armcodegen armasmparser armdesc arminfo x86codegen x86asmparser x86desc x86info Passes)
target_link_libraries(loomc PRIVATE ${LLVM_LIBS})

# Toolchain-specific compiler targets
add_executable(loomc-native src/main.cpp)
target_compile_definitions(loomc-native PRIVATE LOOM_TOOLCHAIN_NATIVE_ONLY)
target_include_directories(loomc-native SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
target_link_libraries(loomc-native PRIVATE ${LLVM_LIBS})



include(CTest)
if(BUILD_TESTING)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_test(NAME frontend_suite
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_tests.py $<TARGET_FILE:loomc> ${CMAKE_CURRENT_BINARY_DIR})
  add_test(NAME workspace_cli
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_tests.py $<TARGET_FILE:loom> $<TARGET_FILE:loomc>)
  find_program(LOOM_NODE_EXECUTABLE node)
  if(LOOM_NODE_EXECUTABLE)
    add_test(NAME vscode_extension_smoke
      COMMAND ${LOOM_NODE_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/vscode-loom/test/smoke.js)
  endif()
  add_test(NAME documentation_suite
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/docs_tests.py $<TARGET_FILE:loomc> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME validation_and_differential_suite
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_validation_suite.py $<TARGET_FILE:loomc>)
  add_test(NAME runtime_regressions
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_regressions.py
      $<TARGET_FILE:loomc> ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME modern_features_suite
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/modern_features_tests.py $<TARGET_FILE:loomc>)

  add_test(NAME aiml_benchmark_suite
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/aiml_benchmark_test.py $<TARGET_FILE:loomc>)
  find_program(LOOM_QEMU_AARCH64_EXECUTABLE qemu-system-aarch64)
  if(LOOM_QEMU_AARCH64_EXECUTABLE AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/native-kernel")
    add_test(NAME native_baremetal_qemu
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/native_baremetal_test.py
        $<TARGET_FILE:loom> $<TARGET_FILE:loomc-native>
        ${CMAKE_CURRENT_SOURCE_DIR}/examples/native-kernel ${LOOM_QEMU_AARCH64_EXECUTABLE})
  endif()
endif()
