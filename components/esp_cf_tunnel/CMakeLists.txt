include(${CMAKE_CURRENT_LIST_DIR}/core_sources.cmake)
if(CONFIG_CF_TUNNEL_HTTP2)
    include(${CMAKE_CURRENT_LIST_DIR}/third_party/nghttp2/library.cmake)
    list(APPEND CF_CORE_SOURCES src/transport/cf_h2.c ${CF_NGHTTP2_SOURCES})
endif()
set(CF_PLATFORM_EMBED)
if(CONFIG_CF_TUNNEL_ESP_TRANSPORT)
    list(APPEND CF_CORE_SOURCES src/transport/esp_cf_tunnel.c)
    set(CF_PLATFORM_EMBED certs/cloudflare_edge_roots.pem)
endif()
idf_component_register(
    SRCS ${CF_CORE_SOURCES} third_party/cjson/cf_cJSON.c
    INCLUDE_DIRS include
    PRIV_INCLUDE_DIRS src/protocol third_party/cjson
    REQUIRES esp-tls esp_timer esp_system lwip
    EMBED_TXTFILES ${CF_PLATFORM_EMBED}
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE CJSON_NESTING_LIMIT=16)
if(CONFIG_CF_TUNNEL_HTTP2)
    cf_configure_nghttp2(${COMPONENT_LIB})
endif()
