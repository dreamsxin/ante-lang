cmake_minimum_required(VERSION 3.4.3)
project(ante)

set(PROJECT_VERSION_MAJOR 0)
set(PROJECT_VERSION_MINOR 9)
set(PROJECT_VERSION_PATCH 0)

set(CMAKE_CXX_STANDARD 11)

include_directories(include)
include_directories(src)

# initialize llvm submodule
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${PROJECT_SOURCE_DIR}/.git")
    # Update submodules as needed
    option(GIT_SUBMODULE "Check submodules during build" ON)
    if(GIT_SUBMODULE)
        message(STATUS "Submodule update")
        execute_process(COMMAND ${GIT_EXECUTABLE} submodule update --init --recursive
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                RESULT_VARIABLE GIT_SUBMOD_RESULT)
        if(NOT GIT_SUBMOD_RESULT EQUAL "0")
            message(FATAL_ERROR "git submodule update --init failed with ${GIT_SUBMOD_RESULT}, please checkout submodules")
        endif()
    endif()
endif()

if(NOT EXISTS "${PROJECT_SOURCE_DIR}/llvm/CMakeLists.txt")
    message(FATAL_ERROR "The submodules were not downloaded! GIT_SUBMODULE was turned off or failed. Please update submodules and try again.")
endif()

# Check if llvm 8.0 exists in the system or if the submodule should be built
find_package(LLVM 8.0 CONFIG)
if (NOT LLVM_FOUND)
    message("System LLVM was not found, LLVM will be built from the submodule.")
    set(LLVM_TARGETS_TO_BUILD "X86" CACHE INTERNAL "")
    option(LLVM_BUILD_RUNTIME OFF)
    option(LLVM_BUILD_TOOLS OFF)
    option(LLVM_INCLUDE_DOCS OFF)
    option(LLVM_INCLUDE_EXAMPLES OFF)
    option(LLVM_INCLUDE_TESTS OFF)
    option(LLVM_INCLUDE_TOOLS OFF)
    option(LLVM_INCLUDE_UTILS OFF)
    option(WITH_POLLY OFF)
    option(Wno-dev OFF)
    # LLVM needs to be built out-of-tree
    add_subdirectory(llvm ${CMAKE_BINARY_DIR}/llvm_build)
    set(LLVM_DIR "${CMAKE_BINARY_DIR}/llvm_build/lib/cmake/llvm")

    # now tries to find LLVM again
    find_package(LLVM 8.0 REQUIRED CONFIG)
    if (NOT LLVM_FOUND)
        message(WARNING "Couldn't build LLVM from the submodule. You might need to run `git submodule update --init`")
    endif()
endif()

# Generate parser.cpp and headers if syntax.y changed
set(AN_PARSER_SRC ${CMAKE_SOURCE_DIR}/src/parser.cpp)
set(AN_YYPARSER_H ${CMAKE_SOURCE_DIR}/include/yyparser.h)
set(AN_SYNTAX_Y ${CMAKE_SOURCE_DIR}/src/syntax.y)
add_custom_command(OUTPUT ${AN_PARSER_SRC}
    COMMAND bison ${AN_SYNTAX_Y} -Lc++ -o${AN_PARSER_SRC} --defines=${AN_YYPARSER_H}
    DEPENDS ${AN_SYNTAX_Y}
)

add_custom_target(anteparser ALL
    DEPENDS ${AN_PARSER_SRC}
)

# The (old) parser generation step. Uses existing bison package but rebuilds parser every build
# find_package(BISON)
# bison_target(parser ${AN_SYNTAX_Y} ${AN_PARSER_SRC} COMPILE_FLAGS "-Lc++ -o${AN_PARSER_SRC} --defines=${AN_YYPARSER_H}")

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})

add_definitions(-DAN_LIB_DIR="${PROJECT_SOURCE_DIR}/stdlib/")

# Find the libraries that correspond to the LLVM components
# that we wish to use
llvm_map_components_to_libnames(llvm_libs core orcjit native bitwriter passes target)

add_library(antecommon STATIC
        include/antevalue.h
        include/antype.h
        include/args.h
        include/compapi.h
        include/compiler.h
        include/constraintfindingvisitor.h
        include/declaration.h
        include/error.h
        include/funcdecl.h
        include/function.h
        include/lazystr.h
        include/lexer.h
        include/module.h
        include/nameresolution.h
        include/nodecl.h
        include/nodevisitor.h
        include/parser.h
        include/pattern.h
        include/ptree.h
        include/repl.h
        include/result.h
        include/scopeguard.h
        include/substitutingvisitor.h
        include/target.h
        include/tokens.h
        include/trait.h
        include/typedvalue.h
        include/typeinference.h
        include/typeerror.h
        include/types.h
        include/unification.h
        include/uniontag.h
        include/variable.h
        src/antevalue.cpp
        src/antevisitor.cpp
        src/antype.cpp
        src/args.cpp
        src/compapi.cpp
        src/compiler.cpp
        src/constraintfindingvisitor.cpp
        src/error.cpp
        src/function.cpp
        src/lazystr.cpp
        src/lexer.cpp
        src/module.cpp
        src/nameresolution.cpp
        src/nodeprinter.cpp
        src/operator.cpp
        src/parser.cpp
        src/pattern.cpp
        src/ptree.cpp
        src/repl.cpp
        src/substitutingvisitor.cpp
        src/typeinference.cpp
        src/typeerror.cpp
        src/types.cpp
        src/unification.cpp
        src/util.cpp)

add_dependencies(antecommon anteparser)

target_link_libraries(antecommon ${llvm_libs})

add_executable(ante src/ante.cpp)

target_link_libraries(ante antecommon)

add_executable(antetests
        tests/unit/catch.hpp
        tests/unit/main.cpp
        tests/unit/nameresolutiontests.cpp
        tests/unit/sizeinbits.cpp
        tests/unit/typechecks.cpp
        tests/unit/modulepath.cpp
        tests/unit/unittest.h)

target_link_libraries(antetests antecommon)

# depends on targets: llvm-headers and llvm-libraries intrinsics_gen table_gen
