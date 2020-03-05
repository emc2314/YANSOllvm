if(WIN32 OR CYGWIN)
  set(LLVM_LINK_COMPONENTS Core Support)
endif()

add_llvm_library( LLVMObf MODULE BUILDTREE_ONLY
  ObfuscateZero.cpp
  Flattening.cpp

  DEPENDS
  intrinsics_gen
  PLUGIN_TOOL
  opt
  )
