#ifndef ACDAT_FORMAT_H
#define ACDAT_FORMAT_H

#include "acdat_core.h"

#define ACDAT_FORMAT_MAGIC 0x54444341U
#define ACDAT_FORMAT_MAJOR 1U
#define ACDAT_FORMAT_MINOR 0U
#define ACDAT_FORMAT_HEADER_SIZE 192U
#define ACDAT_FORMAT_TERMINAL_SIZE 40U
#define ACDAT_FINGERPRINT_SIZE 32U

#define ACDAT_ENGINE_BYTE256 1U
#define ACDAT_INPUT_TEXT 1U
#define ACDAT_INPUT_BYTEA 2U

#define ACDAT_HEADER_SOURCE_FINGERPRINT_OFFSET 96U
#define ACDAT_HEADER_BUILD_FINGERPRINT_OFFSET 128U
#define ACDAT_HEADER_CHECKSUM_OFFSET 160U

typedef struct AcdatFormatOptions
{
    uint16_t input_kind;
    uint16_t compiler_major;
    uint16_t compiler_minor;
    uint16_t compiler_patch;
    uint32_t database_encoding;
    uint32_t flags;
    uint8_t source_fingerprint[ACDAT_FINGERPRINT_SIZE];
    uint8_t build_fingerprint[ACDAT_FINGERPRINT_SIZE];
    uint8_t artifact_checksum[ACDAT_FINGERPRINT_SIZE];
} AcdatFormatOptions;

typedef struct AcdatMachineView
{
    const uint8_t *data;
    uint32_t total_size;
    uint16_t format_major;
    uint16_t format_minor;
    uint16_t engine;
    uint16_t input_kind;
    uint16_t compiler_major;
    uint16_t compiler_minor;
    uint16_t compiler_patch;
    uint32_t database_encoding;
    uint32_t flags;
    uint32_t pattern_count;
    uint32_t state_count;
    uint32_t slot_count;
    uint32_t max_pattern_bytes;
    uint32_t max_pattern_chars;
    uint32_t replacement_bytes;
    uint32_t base_offset;
    uint32_t check_offset;
    uint32_t fail_offset;
    uint32_t terminal_head_offset;
    uint32_t output_link_offset;
    uint32_t terminals_offset;
    uint32_t replacements_offset;
    const uint8_t *source_fingerprint;
    const uint8_t *build_fingerprint;
    const uint8_t *artifact_checksum;
} AcdatMachineView;

AcdatStatus acdat_serialized_size(const AcdatAutomaton *automaton,
                                  uint32_t *size,
                                  AcdatError *error);

AcdatStatus acdat_serialize(const AcdatAutomaton *automaton,
                            const AcdatFormatOptions *options,
                            uint8_t *destination,
                            size_t destination_size,
                            AcdatError *error);

AcdatStatus acdat_parse_machine(const uint8_t *data,
                                size_t size,
                                AcdatMachineView *view,
                                AcdatError *error);

AcdatStatus acdat_peek_machine(const uint8_t *data,
                               size_t size,
                               AcdatMachineView *view,
                               AcdatError *error);

AcdatStatus acdat_materialize_machine(const AcdatMachineView *view,
                                      const AcdatAllocator *allocator,
                                      AcdatAutomaton **result,
                                      AcdatError *error);

void acdat_set_artifact_checksum(uint8_t *data,
                                 const uint8_t checksum[ACDAT_FINGERPRINT_SIZE]);

void acdat_set_build_fingerprint(
    uint8_t *data,
    const uint8_t fingerprint[ACDAT_FINGERPRINT_SIZE]);

uint32_t acdat_format_read_u32(const uint8_t *pointer);
uint64_t acdat_format_read_u64(const uint8_t *pointer);

#endif
