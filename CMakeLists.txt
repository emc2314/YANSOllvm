cmake_minimum_required(VERSION 3.20)
project(yansollvm LANGUAGES C CXX)

find_package(LLVM 21.1.8 REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
include(AddLLVM)

set(CMAKE_CXX_STANDARD 17 CACHE STRING "C++ standard")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${LLVM_INCLUDE_DIRS})
add_definitions(${LLVM_DEFINITIONS})
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/lib/Transforms/Obfuscate)

add_llvm_pass_plugin(yansollvm
  lib/Transforms/Obfuscate/YANSOllvmPlugin.cpp
  lib/Transforms/Obfuscate/Utils.cpp
  lib/Transforms/Obfuscate/CryptoUtils.cpp
  lib/Transforms/Obfuscate/YANSOllvmSeed.cpp
  lib/Transforms/Obfuscate/ObfuscationOptions.cpp
  lib/Transforms/Obfuscate/BogusControlFlow.cpp
  lib/Transforms/Obfuscate/IPObfuscationContext.cpp
  lib/Transforms/Obfuscate/Flattening.cpp
  lib/Transforms/Obfuscate/StringEncryption.cpp
  lib/Transforms/Obfuscate/SplitBasicBlock.cpp
  lib/Transforms/Obfuscate/Substitution.cpp
  lib/Transforms/Obfuscate/IndirectBranch.cpp
  lib/Transforms/Obfuscate/IndirectCall.cpp
  lib/Transforms/Obfuscate/IndirectGlobalVariable.cpp
  lib/Transforms/Obfuscate/YANSOllvmCommon.cpp
  lib/Transforms/Obfuscate/VMVariant.cpp
  lib/Transforms/Obfuscate/VMPass.cpp
  lib/Transforms/Obfuscate/MergePass.cpp
  lib/Transforms/Obfuscate/MFLAPass.cpp
  lib/Transforms/Obfuscate/Func2ModPass.cpp
  lib/Transforms/Obfuscate/BB2FuncPass.cpp
  lib/Transforms/Obfuscate/ConnectPass.cpp
  lib/Transforms/Obfuscate/ObfConPass.cpp
)

find_program(LLVM_LIT llvm-lit HINTS "${LLVM_TOOLS_BINARY_DIR}" REQUIRED)
add_custom_target(check-yansollvm
  COMMAND "${CMAKE_COMMAND}" -E env
          "LLVM_BUILD=${LLVM_TOOLS_BINARY_DIR}/.."
          "YANSOLLVM_PLUGIN=$<TARGET_FILE:yansollvm>"
          "${LLVM_LIT}" -sv "${CMAKE_CURRENT_SOURCE_DIR}/tests"
  DEPENDS yansollvm
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
  USES_TERMINAL
)
