if(WIN32 OR CYGWIN)
  set(LLVM_LINK_COMPONENTS Core Support)
endif()

add_llvm_library( LLVMObf MODULE BUILDTREE_ONLY
  Util.cpp
  ObfuscateZero.cpp
  Flattening.cpp
  Connect.cpp

  DEPENDS
  intrinsics_gen
  PLUGIN_TOOL
  opt
  )
