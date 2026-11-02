cmake_minimum_required(VERSION 3.20)
project(Loom VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

add_executable(loomc src/main.cpp)
add_executable(loom src/driver.cpp)
install(TARGETS loom loomc RUNTIME DESTINATION bin)
target_include_directories(loomc SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
llvm_map_components_to_libnames(LLVM_LIBS Core Support Target CodeGen MC native armcodegen armasmparser armdesc arminfo Passes)
target_link_libraries(loomc PRIVATE ${LLVM_LIBS})

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
  add_test(NAME modern_features_suite
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/modern_features_tests.py $<TARGET_FILE:loomc>)
endif()
