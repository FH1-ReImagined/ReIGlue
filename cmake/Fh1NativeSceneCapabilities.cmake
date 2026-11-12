# The marker describes the bytes actually shipped beside it, not a build-tree
# binary that may later have its RPATH changed by CMake install.
file(REMOVE "${FH1_CAPABILITIES}")
if(NOT FH1_ENABLED)
    return()
endif()
if(NOT EXISTS "${FH1_GPU_PLUGIN}")
    message(FATAL_ERROR "Native scene capability: renderer does not exist")
endif()
file(SHA256 "${FH1_GPU_PLUGIN}" renderer_hash)
file(WRITE "${FH1_CAPABILITIES}" "{\"api\":1,\"renderer_sha256\":\"${renderer_hash}\"}\n")
