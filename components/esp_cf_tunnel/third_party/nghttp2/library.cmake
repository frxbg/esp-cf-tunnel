# Minimal upstream library only: no executables, TLS, zlib, HTTP clients,
# install rules, or global compiler settings. One printf portability fix is
# documented in README.esp_cf_tunnel.md; protocol sources otherwise unchanged.
set(CF_NGHTTP2_DIR ${CMAKE_CURRENT_LIST_DIR})
set(CF_NGHTTP2_NAMES
    nghttp2_pq nghttp2_map nghttp2_queue nghttp2_frame nghttp2_buf
    nghttp2_stream nghttp2_outbound_item nghttp2_session nghttp2_submit
    nghttp2_helper nghttp2_alpn nghttp2_hd nghttp2_hd_huffman nghttp2_hd_huffman_data
    nghttp2_version nghttp2_priority_spec nghttp2_option nghttp2_callbacks
    nghttp2_mem nghttp2_http nghttp2_rcbuf nghttp2_extpri nghttp2_ratelim
    nghttp2_time nghttp2_debug sfparse)
set(CF_NGHTTP2_SOURCES)
foreach(name IN LISTS CF_NGHTTP2_NAMES)
    list(APPEND CF_NGHTTP2_SOURCES ${CF_NGHTTP2_DIR}/lib/${name}.c)
endforeach()
function(cf_configure_nghttp2 target)
    target_include_directories(${target} PUBLIC ${CF_NGHTTP2_DIR}/lib/includes)
    target_compile_definitions(${target} PUBLIC NGHTTP2_STATICLIB)
    target_compile_definitions(${target} PRIVATE BUILDING_NGHTTP2)
    if(WIN32)
        target_compile_definitions(${target} PRIVATE WIN32 HAVE_WINDOWS_H HAVE_GETTICKCOUNT64)
        if(MSVC)
            target_compile_definitions(${target} PRIVATE ssize_t=ptrdiff_t)
            target_compile_definitions(${target} PUBLIC NGHTTP2_NO_SSIZE_T)
        endif()
    else()
        target_compile_definitions(${target} PRIVATE HAVE_ARPA_INET_H HAVE_NETINET_IN_H
            HAVE_CLOCK_GETTIME HAVE_DECL_CLOCK_MONOTONIC=1 _POSIX_C_SOURCE=200809L)
    endif()
endfunction()
