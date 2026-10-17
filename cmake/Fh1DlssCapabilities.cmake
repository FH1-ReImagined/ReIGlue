# Run after linking, so the launcher can distinguish a native-NGX GPU library
# from an older SDK overlay without executing the game or loading foreign DLLs.
file(SHA256 "${FH1_GPU_DLL}" renderer_hash)
file(WRITE "${FH1_CAPABILITIES}"
    "{\"api\":2,\"renderer_sha256\":\"${renderer_hash}\"}\n")
