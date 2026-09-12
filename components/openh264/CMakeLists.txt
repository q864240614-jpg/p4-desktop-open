file(GLOB codec_sources
    "upstream/codec/common/src/*.cpp"
    "upstream/codec/decoder/core/src/*.cpp"
    "upstream/codec/decoder/plus/src/*.cpp")
idf_component_register(SRCS ${codec_sources} "p4_h264.cpp"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "upstream/codec/api" "upstream/codec/api/wels" "upstream/codec/common/inc"
        "upstream/codec/decoder/core/inc" "upstream/codec/decoder/plus/inc"
    PRIV_REQUIRES heap pthread esp_timer)
# Upstream portable C++ decoder; no encoder or architecture assembly.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-error=format -fno-strict-aliasing -O3)

target_compile_definitions(${COMPONENT_LIB} PRIVATE GENERATED_VERSION_HEADER)
