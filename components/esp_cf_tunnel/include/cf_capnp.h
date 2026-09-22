#ifndef CF_CAPNP_H
#define CF_CAPNP_H
#include "cf_capnp_framing.h"

/* Unpacked Cap'n Proto views borrow the complete frame. No allocation, casts to
 * aligned integers, recursion, or pointer arithmetic before bounds validation.
 * Traversal charges repeated reads; nesting and work are independently bounded. */
#define CF_CAPNP_DEPTH_MAX 32u
#define CF_CAPNP_TRAVERSAL_WORDS 4096u
typedef enum { CF_CP_NULL, CF_CP_STRUCT, CF_CP_LIST, CF_CP_CAP } cf_cp_kind;
typedef struct {
    cf_cp_kind kind;
    uint32_t segment, word, count, cap;
    uint16_t data_words, pointers;
    uint8_t element_size, depth;
} cf_cp_value;
typedef struct {
    cf_bytes segments[CF_CAPNP_MAX_SEGMENTS];
    uint32_t segment_count, budget;
    cf_result error;
} cf_cp_reader;

cf_result cf_cp_read_init(cf_cp_reader *r, cf_bytes frame);
cf_cp_value cf_cp_root(cf_cp_reader *r);
cf_cp_value cf_cp_field(cf_cp_reader *r, cf_cp_value s, uint16_t index);
cf_cp_value cf_cp_at(cf_cp_reader *r, cf_cp_value list, uint32_t index);
uint64_t cf_cp_uint(cf_cp_reader *r, cf_cp_value s, uint16_t byte, uint8_t width);
cf_bytes cf_cp_data(cf_cp_reader *r, cf_cp_value v, bool text);

/* Bounded, single-segment encoder. Objects/slots are segment-relative word
 * indices. The caller owns and erases output containing authentication data.
 * Errors are sticky; cf_cp_build_end returns no bytes after an error. */
typedef struct {
    uint8_t *buffer;
    size_t capacity;
    uint32_t words;
    cf_result error;
} cf_cp_builder;
cf_result cf_cp_build_init(cf_cp_builder *b, uint8_t *out, size_t capacity);
uint32_t cf_cp_struct(cf_cp_builder *b, uint32_t slot, uint16_t data_words, uint16_t pointers);
uint32_t cf_cp_pointer_list(cf_cp_builder *b, uint32_t slot, uint32_t count);
void cf_cp_set_uint(cf_cp_builder *b, uint32_t object, uint16_t byte, uint8_t width, uint64_t value);
void cf_cp_blob(cf_cp_builder *b, uint32_t slot, cf_bytes value, bool text);
cf_result cf_cp_build_end(cf_cp_builder *b, size_t *written);
#endif
