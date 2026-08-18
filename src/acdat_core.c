#include "acdat_core.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct TrieNode
{
    uint32_t first_edge;
    uint32_t edge_count;
    uint32_t terminal_head;
    uint32_t terminal_tail;
    uint32_t parent;
    uint32_t dat_state;
    uint8_t symbol;
} TrieNode;

typedef struct TrieEdge
{
    uint32_t child;
    uint32_t next;
    uint8_t symbol;
} TrieEdge;

typedef struct PatternWork
{
    const AcdatPatternInput *pattern;
    size_t original_index;
} PatternWork;

typedef struct PatternIdWork
{
    int64_t pattern_id;
    size_t original_index;
} PatternIdWork;

typedef struct AcdatBuilder
{
    AcdatAllocator allocator;

    TrieNode *nodes;
    uint32_t node_count;
    uint32_t node_capacity;

    TrieEdge *edges;
    uint32_t edge_count;
    uint32_t edge_capacity;

    AcdatTerminal *terminals;
    uint32_t terminal_count;
    uint32_t terminal_capacity;

    uint8_t *replacement_arena;
    uint32_t replacement_size;
    uint32_t replacement_capacity;

    uint32_t *base;
    uint32_t *check;
    uint32_t *fail;
    uint32_t *terminal_head;
    uint32_t *output_link;
    uint8_t *used_base;
    uint32_t slot_capacity;
    uint32_t max_used_slot;
    uint32_t next_check_pos;

    uint32_t *queue;
    uint32_t queue_capacity;
} AcdatBuilder;

static void *
default_alloc(void *context, size_t size)
{
    (void) context;
    return malloc(size);
}

static void *
default_realloc(void *context, void *pointer, size_t size)
{
    (void) context;
    return realloc(pointer, size);
}

static void
default_free(void *context, void *pointer)
{
    (void) context;
    free(pointer);
}

AcdatAllocator
acdat_default_allocator(void)
{
    AcdatAllocator allocator;

    allocator.context = NULL;
    allocator.alloc = default_alloc;
    allocator.realloc = default_realloc;
    allocator.free = default_free;
    return allocator;
}

#if defined(__GNUC__) || defined(__clang__)
static void set_error(AcdatError *error, AcdatStatus code,
                      size_t pattern_index, const char *format, ...)
    __attribute__((format(printf, 4, 5)));
#endif

static void
set_error(AcdatError *error, AcdatStatus code, size_t pattern_index,
          const char *format, ...)
{
    va_list arguments;

    if (error == NULL)
        return;

    error->code = code;
    error->pattern_index = pattern_index;
    va_start(arguments, format);
    (void) vsnprintf(error->message, sizeof(error->message), format, arguments);
    va_end(arguments);
}

static bool
valid_allocator(const AcdatAllocator *allocator)
{
    return allocator != NULL && allocator->alloc != NULL &&
           allocator->realloc != NULL && allocator->free != NULL;
}

static void *
builder_alloc(AcdatBuilder *builder, size_t size)
{
    if (size == 0)
        size = 1;
    return builder->allocator.alloc(builder->allocator.context, size);
}

static void *
builder_realloc(AcdatBuilder *builder, void *pointer, size_t size)
{
    if (size == 0)
        size = 1;
    if (pointer == NULL)
        return builder_alloc(builder, size);
    return builder->allocator.realloc(builder->allocator.context, pointer, size);
}

static void
builder_free_pointer(AcdatBuilder *builder, void *pointer)
{
    if (pointer != NULL)
        builder->allocator.free(builder->allocator.context, pointer);
}

static bool
checked_array_size(uint32_t count, size_t element_size, size_t *result)
{
    if (element_size != 0 && (size_t) count > SIZE_MAX / element_size)
        return false;
    *result = (size_t) count * element_size;
    return true;
}

static bool
next_capacity(uint32_t current, uint32_t needed, uint32_t *result)
{
    uint64_t capacity = current == 0 ? 16U : current;

    while (capacity < needed)
    {
        capacity = capacity + capacity / 2U + 8U;
        if (capacity > UINT32_MAX)
        {
            capacity = needed;
            break;
        }
    }

    if (capacity < needed || capacity > UINT32_MAX)
        return false;
    *result = (uint32_t) capacity;
    return true;
}

static AcdatStatus
reserve_array(AcdatBuilder *builder, void **pointer, uint32_t *capacity,
              uint32_t needed, size_t element_size, AcdatError *error)
{
    uint32_t new_capacity;
    size_t bytes;
    void *new_pointer;

    if (needed <= *capacity)
        return ACDAT_OK;
    if (!next_capacity(*capacity, needed, &new_capacity) ||
        !checked_array_size(new_capacity, element_size, &bytes))
    {
        set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                  "array capacity exceeds addressable memory");
        return ACDAT_ERR_SIZE_OVERFLOW;
    }

    new_pointer = builder_realloc(builder, *pointer, bytes);
    if (new_pointer == NULL)
    {
        set_error(error, ACDAT_ERR_OUT_OF_MEMORY, SIZE_MAX,
                  "unable to allocate %zu bytes", bytes);
        return ACDAT_ERR_OUT_OF_MEMORY;
    }

    *pointer = new_pointer;
    *capacity = new_capacity;
    return ACDAT_OK;
}

static AcdatStatus
reserve_nodes(AcdatBuilder *builder, uint32_t needed, AcdatError *error)
{
    return reserve_array(builder, (void **) &builder->nodes,
                         &builder->node_capacity, needed,
                         sizeof(TrieNode), error);
}

static AcdatStatus
reserve_edges(AcdatBuilder *builder, uint32_t needed, AcdatError *error)
{
    return reserve_array(builder, (void **) &builder->edges,
                         &builder->edge_capacity, needed,
                         sizeof(TrieEdge), error);
}

static AcdatStatus
reserve_terminals(AcdatBuilder *builder, uint32_t needed, AcdatError *error)
{
    return reserve_array(builder, (void **) &builder->terminals,
                         &builder->terminal_capacity, needed,
                         sizeof(AcdatTerminal), error);
}

static AcdatStatus
reserve_replacements(AcdatBuilder *builder, uint32_t needed,
                     AcdatError *error)
{
    return reserve_array(builder, (void **) &builder->replacement_arena,
                         &builder->replacement_capacity, needed,
                         sizeof(uint8_t), error);
}

static AcdatStatus
reserve_queue(AcdatBuilder *builder, uint32_t needed, AcdatError *error)
{
    return reserve_array(builder, (void **) &builder->queue,
                         &builder->queue_capacity, needed,
                         sizeof(uint32_t), error);
}

static AcdatStatus
reserve_slots(AcdatBuilder *builder, uint32_t needed, AcdatError *error)
{
    uint32_t old_capacity = builder->slot_capacity;
    uint32_t new_capacity;
    size_t u32_bytes;
    size_t u8_bytes;
    uint32_t *new_u32;
    uint8_t *new_u8;

    if (needed <= old_capacity)
        return ACDAT_OK;
    if (!next_capacity(old_capacity, needed, &new_capacity) ||
        !checked_array_size(new_capacity, sizeof(uint32_t), &u32_bytes) ||
        !checked_array_size(new_capacity, sizeof(uint8_t), &u8_bytes))
    {
        set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                  "DAT slot capacity exceeds addressable memory");
        return ACDAT_ERR_SIZE_OVERFLOW;
    }

#define GROW_U32(member)                                                       \
    do                                                                          \
    {                                                                           \
        new_u32 = builder_realloc(builder, builder->member, u32_bytes);         \
        if (new_u32 == NULL)                                                     \
            goto out_of_memory;                                                  \
        builder->member = new_u32;                                               \
        memset(builder->member + old_capacity, 0,                               \
               (size_t) (new_capacity - old_capacity) * sizeof(uint32_t));      \
    } while (0)

    GROW_U32(base);
    GROW_U32(check);
    GROW_U32(fail);
    GROW_U32(terminal_head);
    GROW_U32(output_link);
#undef GROW_U32

    new_u8 = builder_realloc(builder, builder->used_base, u8_bytes);
    if (new_u8 == NULL)
        goto out_of_memory;
    builder->used_base = new_u8;
    memset(builder->used_base + old_capacity, 0,
           (size_t) (new_capacity - old_capacity));

    builder->slot_capacity = new_capacity;
    return ACDAT_OK;

out_of_memory:
    set_error(error, ACDAT_ERR_OUT_OF_MEMORY, SIZE_MAX,
              "unable to allocate DAT arrays for %u slots", new_capacity);
    return ACDAT_ERR_OUT_OF_MEMORY;
}

static int
compare_pattern_work(const void *left_pointer, const void *right_pointer)
{
    const PatternWork *left = (const PatternWork *) left_pointer;
    const PatternWork *right = (const PatternWork *) right_pointer;
    const AcdatPatternInput *a = left->pattern;
    const AcdatPatternInput *b = right->pattern;
    uint32_t common = a->byte_length < b->byte_length ?
                      a->byte_length : b->byte_length;
    int comparison = common == 0 ? 0 : memcmp(a->pattern, b->pattern, common);

    if (comparison != 0)
        return comparison;
    if (a->byte_length != b->byte_length)
        return a->byte_length < b->byte_length ? -1 : 1;
    if (a->pattern_id != b->pattern_id)
        return a->pattern_id < b->pattern_id ? -1 : 1;
    if (a->priority != b->priority)
        return a->priority < b->priority ? -1 : 1;
    if (a->has_replacement != b->has_replacement)
        return a->has_replacement ? 1 : -1;
    common = a->replacement_length < b->replacement_length ?
             a->replacement_length : b->replacement_length;
    comparison = common == 0 ? 0 :
                 memcmp(a->replacement, b->replacement, common);
    if (comparison != 0)
        return comparison;
    if (a->replacement_length != b->replacement_length)
        return a->replacement_length < b->replacement_length ? -1 : 1;
    return 0;
}

static int
compare_pattern_ids(const void *left_pointer, const void *right_pointer)
{
    const PatternIdWork *left = (const PatternIdWork *) left_pointer;
    const PatternIdWork *right = (const PatternIdWork *) right_pointer;

    if (left->pattern_id < right->pattern_id)
        return -1;
    if (left->pattern_id > right->pattern_id)
        return 1;
    return 0;
}

static AcdatStatus
append_node(AcdatBuilder *builder, uint32_t parent, uint8_t symbol,
            uint32_t *node_index, AcdatError *error)
{
    TrieNode *node;
    AcdatStatus status;

    if (builder->node_count == UINT32_MAX)
    {
        set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                  "trie contains too many nodes");
        return ACDAT_ERR_SIZE_OVERFLOW;
    }
    status = reserve_nodes(builder, builder->node_count + 1U, error);
    if (status != ACDAT_OK)
        return status;

    *node_index = builder->node_count++;
    node = &builder->nodes[*node_index];
    memset(node, 0, sizeof(*node));
    node->parent = parent;
    node->symbol = symbol;
    return ACDAT_OK;
}

static AcdatStatus
find_or_add_child(AcdatBuilder *builder, uint32_t parent, uint8_t symbol,
                  uint32_t *child, AcdatError *error)
{
    TrieNode *parent_node = &builder->nodes[parent];
    uint32_t previous = ACDAT_NO_INDEX;
    uint32_t current = parent_node->first_edge;
    uint32_t edge_handle;
    TrieEdge *edge;
    AcdatStatus status;

    while (current != ACDAT_NO_INDEX)
    {
        edge = &builder->edges[current - 1U];
        if (edge->symbol == symbol)
        {
            *child = edge->child;
            return ACDAT_OK;
        }
        if (edge->symbol > symbol)
            break;
        previous = current;
        current = edge->next;
    }

    status = append_node(builder, parent, symbol, child, error);
    if (status != ACDAT_OK)
        return status;
    parent_node = &builder->nodes[parent];
    if (builder->edge_count == UINT32_MAX)
    {
        set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                  "trie contains too many edges");
        return ACDAT_ERR_SIZE_OVERFLOW;
    }
    status = reserve_edges(builder, builder->edge_count + 1U, error);
    if (status != ACDAT_OK)
        return status;

    edge_handle = ++builder->edge_count;
    edge = &builder->edges[edge_handle - 1U];
    edge->child = *child;
    edge->symbol = symbol;
    edge->next = current;
    if (previous == ACDAT_NO_INDEX)
        parent_node->first_edge = edge_handle;
    else
        builder->edges[previous - 1U].next = edge_handle;
    parent_node->edge_count++;
    return ACDAT_OK;
}

static AcdatStatus
append_terminal(AcdatBuilder *builder, uint32_t node_index,
                const AcdatPatternInput *pattern, uint32_t canonical_ordinal,
                AcdatError *error)
{
    TrieNode *node = &builder->nodes[node_index];
    AcdatTerminal *terminal;
    uint32_t terminal_handle;
    uint32_t replacement_offset = 0;
    uint64_t replacement_end;
    AcdatStatus status;

    if (builder->terminal_count == UINT32_MAX)
    {
        set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                  "dictionary contains too many terminal entries");
        return ACDAT_ERR_SIZE_OVERFLOW;
    }
    status = reserve_terminals(builder, builder->terminal_count + 1U, error);
    if (status != ACDAT_OK)
        return status;

    if (pattern->has_replacement)
    {
        replacement_end = (uint64_t) builder->replacement_size +
                          pattern->replacement_length;
        if (replacement_end > UINT32_MAX)
        {
            set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                      "replacement arena exceeds 4 GiB");
            return ACDAT_ERR_SIZE_OVERFLOW;
        }
        replacement_offset = builder->replacement_size;
        status = reserve_replacements(builder, (uint32_t) replacement_end,
                                      error);
        if (status != ACDAT_OK)
            return status;
        if (pattern->replacement_length > 0)
            memcpy(builder->replacement_arena + replacement_offset,
                   pattern->replacement, pattern->replacement_length);
        builder->replacement_size = (uint32_t) replacement_end;
    }

    terminal_handle = ++builder->terminal_count;
    terminal = &builder->terminals[terminal_handle - 1U];
    memset(terminal, 0, sizeof(*terminal));
    terminal->pattern_id = pattern->pattern_id;
    terminal->byte_length = pattern->byte_length;
    terminal->char_length = pattern->char_length;
    terminal->priority = pattern->priority;
    terminal->replacement_offset = replacement_offset;
    terminal->replacement_length = pattern->replacement_length;
    terminal->canonical_ordinal = canonical_ordinal;
    terminal->has_replacement = pattern->has_replacement;

    if (node->terminal_head == ACDAT_NO_INDEX)
        node->terminal_head = terminal_handle;
    else
        builder->terminals[node->terminal_tail - 1U].next_terminal =
            terminal_handle;
    node->terminal_tail = terminal_handle;
    return ACDAT_OK;
}

static AcdatStatus
build_trie(AcdatBuilder *builder, const PatternWork *work,
           uint32_t pattern_count, uint32_t *max_pattern_bytes,
           uint32_t *max_pattern_chars, AcdatError *error)
{
    uint32_t root;
    uint32_t pattern_index;
    AcdatStatus status;

    status = append_node(builder, 0, 0, &root, error);
    if (status != ACDAT_OK)
        return status;
    if (root != 0)
    {
        set_error(error, ACDAT_ERR_BUILD_FAILED, SIZE_MAX,
                  "internal root node invariant failed");
        return ACDAT_ERR_BUILD_FAILED;
    }

    *max_pattern_bytes = 0;
    *max_pattern_chars = 0;
    for (pattern_index = 0; pattern_index < pattern_count; pattern_index++)
    {
        const AcdatPatternInput *pattern = work[pattern_index].pattern;
        uint32_t node = root;
        uint32_t byte_index;

        for (byte_index = 0; byte_index < pattern->byte_length; byte_index++)
        {
            status = find_or_add_child(builder, node,
                                       pattern->pattern[byte_index],
                                       &node, error);
            if (status != ACDAT_OK)
                return status;
        }
        status = append_terminal(builder, node, pattern, pattern_index, error);
        if (status != ACDAT_OK)
            return status;
        if (pattern->byte_length > *max_pattern_bytes)
            *max_pattern_bytes = pattern->byte_length;
        if (pattern->char_length > *max_pattern_chars)
            *max_pattern_chars = pattern->char_length;
    }
    return ACDAT_OK;
}

static AcdatStatus
find_base(AcdatBuilder *builder, const TrieNode *node, uint32_t *base_value,
          AcdatError *error)
{
    const TrieEdge *first_edge =
        &builder->edges[node->first_edge - 1U];
    uint32_t minimum_code = (uint32_t) first_edge->symbol + 1U;
    uint64_t position = builder->next_check_pos;
    uint64_t scan_start;
    uint64_t occupied_positions = 0;

    if (position < minimum_code)
        position = minimum_code;
    scan_start = position;

    for (;; position++)
    {
        uint64_t candidate64;
        uint32_t candidate;
        uint32_t edge_handle;
        bool available = true;

        if (position > UINT32_MAX)
        {
            set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                      "DAT base search exceeded uint32 range");
            return ACDAT_ERR_SIZE_OVERFLOW;
        }
        candidate64 = position - minimum_code;
        if (candidate64 > UINT32_MAX)
        {
            set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                      "DAT base value exceeded uint32 range");
            return ACDAT_ERR_SIZE_OVERFLOW;
        }
        candidate = (uint32_t) candidate64;

        if (candidate >= builder->slot_capacity)
        {
            AcdatStatus status = reserve_slots(builder, candidate + 1U, error);
            if (status != ACDAT_OK)
                return status;
        }
        if (position < builder->slot_capacity &&
            builder->check[(uint32_t) position] != 0)
        {
            occupied_positions++;
            continue;
        }
        if (builder->used_base[candidate] != 0)
            continue;
        edge_handle = node->first_edge;
        while (edge_handle != ACDAT_NO_INDEX)
        {
            const TrieEdge *edge = &builder->edges[edge_handle - 1U];
            uint64_t slot64 = candidate64 + (uint32_t) edge->symbol + 1U;
            AcdatStatus status;

            if (slot64 >= UINT32_MAX)
            {
                set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                          "DAT slot exceeded uint32 range");
                return ACDAT_ERR_SIZE_OVERFLOW;
            }
            status = reserve_slots(builder, (uint32_t) slot64 + 1U, error);
            if (status != ACDAT_OK)
                return status;
            if (builder->check[(uint32_t) slot64] != 0)
            {
                available = false;
                break;
            }
            edge_handle = edge->next;
        }

        if (available)
        {
            uint64_t scanned_positions = position - scan_start + 1U;

            /*
             * Dense prefixes otherwise make every later node rescan the same
             * isolated holes.  Once a search window is at least 95% occupied,
             * advance the global starting point to the successful position.
             */
            if (occupied_positions * 100U >= scanned_positions * 95U)
                builder->next_check_pos = (uint32_t) position;
            builder->used_base[candidate] = 1;
            *base_value = candidate;
            return ACDAT_OK;
        }
    }
}

static uint32_t
direct_transition_arrays(const AcdatBuilder *builder, uint32_t state,
                         uint8_t symbol)
{
    uint64_t slot;

    if (state == 0 || state >= builder->slot_capacity)
        return 0;
    slot = (uint64_t) builder->base[state] + (uint32_t) symbol + 1U;
    if (slot >= builder->slot_capacity)
        return 0;
    if (builder->check[(uint32_t) slot] != state)
        return 0;
    return (uint32_t) slot;
}

static AcdatStatus
pack_dat(AcdatBuilder *builder, AcdatError *error)
{
    uint32_t queue_head = 0;
    uint32_t queue_tail = 0;
    AcdatStatus status;

    status = reserve_slots(builder, 512U, error);
    if (status != ACDAT_OK)
        return status;
    status = reserve_queue(builder, builder->node_count, error);
    if (status != ACDAT_OK)
        return status;

    builder->nodes[0].dat_state = ACDAT_ROOT_STATE;
    builder->check[ACDAT_ROOT_STATE] = ACDAT_ROOT_STATE;
    builder->fail[ACDAT_ROOT_STATE] = ACDAT_ROOT_STATE;
    builder->max_used_slot = ACDAT_ROOT_STATE;
    builder->next_check_pos = ACDAT_ROOT_STATE + 1U;
    builder->queue[queue_tail++] = 0;

    while (queue_head < queue_tail)
    {
        uint32_t node_index = builder->queue[queue_head++];
        TrieNode *node = &builder->nodes[node_index];
        uint32_t edge_handle;
        uint32_t base_value;

        if (node->edge_count == 0)
            continue;
        status = find_base(builder, node, &base_value, error);
        if (status != ACDAT_OK)
            return status;
        builder->base[node->dat_state] = base_value;

        edge_handle = node->first_edge;
        while (edge_handle != ACDAT_NO_INDEX)
        {
            TrieEdge *edge = &builder->edges[edge_handle - 1U];
            uint32_t slot = base_value + (uint32_t) edge->symbol + 1U;
            TrieNode *child = &builder->nodes[edge->child];

            builder->check[slot] = node->dat_state;
            child->dat_state = slot;
            if (slot > builder->max_used_slot)
                builder->max_used_slot = slot;
            builder->queue[queue_tail++] = edge->child;
            edge_handle = edge->next;
        }

        while (builder->next_check_pos < builder->slot_capacity &&
               builder->check[builder->next_check_pos] != 0)
            builder->next_check_pos++;
    }

    if (queue_tail != builder->node_count)
    {
        set_error(error, ACDAT_ERR_BUILD_FAILED, SIZE_MAX,
                  "DAT traversal visited %u of %u trie nodes",
                  queue_tail, builder->node_count);
        return ACDAT_ERR_BUILD_FAILED;
    }

    for (queue_head = 0; queue_head < queue_tail; queue_head++)
    {
        TrieNode *node = &builder->nodes[builder->queue[queue_head]];

        builder->terminal_head[node->dat_state] = node->terminal_head;
    }

    for (queue_head = 1; queue_head < queue_tail; queue_head++)
    {
        TrieNode *node = &builder->nodes[builder->queue[queue_head]];
        uint32_t state = node->dat_state;
        uint32_t parent_state = builder->nodes[node->parent].dat_state;
        uint32_t failure;

        if (parent_state == ACDAT_ROOT_STATE)
            failure = ACDAT_ROOT_STATE;
        else
        {
            uint32_t candidate = builder->fail[parent_state];

            for (;;)
            {
                uint32_t transition = direct_transition_arrays(builder,
                                                               candidate,
                                                               node->symbol);
                if (transition != 0 && transition != state)
                {
                    failure = transition;
                    break;
                }
                if (candidate == ACDAT_ROOT_STATE)
                {
                    failure = ACDAT_ROOT_STATE;
                    break;
                }
                candidate = builder->fail[candidate];
            }
        }

        builder->fail[state] = failure;
        builder->output_link[state] =
            builder->terminal_head[failure] != ACDAT_NO_INDEX ?
            failure : builder->output_link[failure];
    }
    return ACDAT_OK;
}

static void
free_builder(AcdatBuilder *builder)
{
    builder_free_pointer(builder, builder->nodes);
    builder_free_pointer(builder, builder->edges);
    builder_free_pointer(builder, builder->terminals);
    builder_free_pointer(builder, builder->replacement_arena);
    builder_free_pointer(builder, builder->base);
    builder_free_pointer(builder, builder->check);
    builder_free_pointer(builder, builder->fail);
    builder_free_pointer(builder, builder->terminal_head);
    builder_free_pointer(builder, builder->output_link);
    builder_free_pointer(builder, builder->used_base);
    builder_free_pointer(builder, builder->queue);
    memset(builder, 0, sizeof(*builder));
}

AcdatStatus
acdat_build(const AcdatPatternInput *patterns, size_t pattern_count,
            const AcdatAllocator *allocator_argument, AcdatAutomaton **result,
            AcdatError *error)
{
    AcdatBuilder builder;
    AcdatAllocator allocator;
    PatternWork *work = NULL;
    PatternIdWork *ids = NULL;
    AcdatAutomaton *automaton = NULL;
    uint32_t max_pattern_bytes = 0;
    uint32_t max_pattern_chars = 0;
    uint32_t count32;
    uint32_t index;
    size_t bytes;
    AcdatStatus status = ACDAT_OK;

    if (result != NULL)
        *result = NULL;
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (result == NULL)
    {
        set_error(error, ACDAT_ERR_INVALID_ARGUMENT, SIZE_MAX,
                  "result pointer is NULL");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    if (pattern_count > 0 && patterns == NULL)
    {
        set_error(error, ACDAT_ERR_INVALID_ARGUMENT, SIZE_MAX,
                  "patterns pointer is NULL");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    if (pattern_count > UINT32_MAX)
    {
        set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                  "pattern count exceeds uint32 range");
        return ACDAT_ERR_SIZE_OVERFLOW;
    }

    allocator = allocator_argument == NULL ?
                acdat_default_allocator() : *allocator_argument;
    if (!valid_allocator(&allocator))
    {
        set_error(error, ACDAT_ERR_INVALID_ARGUMENT, SIZE_MAX,
                  "allocator callbacks are incomplete");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }

    memset(&builder, 0, sizeof(builder));
    builder.allocator = allocator;
    count32 = (uint32_t) pattern_count;

    if (count32 > 0)
    {
        if (!checked_array_size(count32, sizeof(PatternWork), &bytes))
        {
            status = ACDAT_ERR_SIZE_OVERFLOW;
            set_error(error, status, SIZE_MAX,
                      "pattern work array exceeds addressable memory");
            goto cleanup;
        }
        work = builder_alloc(&builder, bytes);
        if (work == NULL)
        {
            status = ACDAT_ERR_OUT_OF_MEMORY;
            set_error(error, status, SIZE_MAX,
                      "unable to allocate pattern work array");
            goto cleanup;
        }
        if (!checked_array_size(count32, sizeof(PatternIdWork), &bytes))
        {
            status = ACDAT_ERR_SIZE_OVERFLOW;
            set_error(error, status, SIZE_MAX,
                      "pattern ID array exceeds addressable memory");
            goto cleanup;
        }
        ids = builder_alloc(&builder, bytes);
        if (ids == NULL)
        {
            status = ACDAT_ERR_OUT_OF_MEMORY;
            set_error(error, status, SIZE_MAX,
                      "unable to allocate pattern ID array");
            goto cleanup;
        }
    }

    for (index = 0; index < count32; index++)
    {
        const AcdatPatternInput *pattern = &patterns[index];

        if (pattern->byte_length == 0)
        {
            status = ACDAT_ERR_EMPTY_PATTERN;
            set_error(error, status, index, "pattern %u is empty", index);
            goto cleanup;
        }
        if (pattern->pattern == NULL)
        {
            status = ACDAT_ERR_INVALID_ARGUMENT;
            set_error(error, status, index,
                      "pattern %u has a NULL byte pointer", index);
            goto cleanup;
        }
        if (pattern->has_replacement && pattern->replacement_length > 0 &&
            pattern->replacement == NULL)
        {
            status = ACDAT_ERR_INVALID_ARGUMENT;
            set_error(error, status, index,
                      "pattern %u has a NULL replacement pointer", index);
            goto cleanup;
        }
        if (!pattern->has_replacement && pattern->replacement_length != 0)
        {
            status = ACDAT_ERR_INVALID_ARGUMENT;
            set_error(error, status, index,
                      "pattern %u has replacement bytes but replacement is disabled",
                      index);
            goto cleanup;
        }
        work[index].pattern = pattern;
        work[index].original_index = index;
        ids[index].pattern_id = pattern->pattern_id;
        ids[index].original_index = index;
    }

    if (count32 > 1)
    {
        qsort(work, count32, sizeof(PatternWork), compare_pattern_work);
        qsort(ids, count32, sizeof(PatternIdWork), compare_pattern_ids);
        for (index = 1; index < count32; index++)
        {
            if (ids[index - 1U].pattern_id == ids[index].pattern_id)
            {
                status = ACDAT_ERR_DUPLICATE_PATTERN_ID;
                set_error(error, status, ids[index].original_index,
                          "pattern ID %lld is duplicated",
                          (long long) ids[index].pattern_id);
                goto cleanup;
            }
        }
    }

    status = build_trie(&builder, work, count32, &max_pattern_bytes,
                        &max_pattern_chars, error);
    if (status != ACDAT_OK)
        goto cleanup;
    status = pack_dat(&builder, error);
    if (status != ACDAT_OK)
        goto cleanup;

    automaton = allocator.alloc(allocator.context, sizeof(*automaton));
    if (automaton == NULL)
    {
        status = ACDAT_ERR_OUT_OF_MEMORY;
        set_error(error, status, SIZE_MAX,
                  "unable to allocate automaton descriptor");
        goto cleanup;
    }
    memset(automaton, 0, sizeof(*automaton));
    automaton->allocator = allocator;
    automaton->slot_count = builder.max_used_slot + 1U;
    automaton->state_count = builder.node_count;
    automaton->pattern_count = builder.terminal_count;
    automaton->max_pattern_bytes = max_pattern_bytes;
    automaton->max_pattern_chars = max_pattern_chars;
    automaton->replacement_bytes = builder.replacement_size;
    automaton->base = builder.base;
    automaton->check = builder.check;
    automaton->fail = builder.fail;
    automaton->terminal_head = builder.terminal_head;
    automaton->output_link = builder.output_link;
    automaton->terminals = builder.terminals;
    automaton->replacement_arena = builder.replacement_arena;

    builder.base = NULL;
    builder.check = NULL;
    builder.fail = NULL;
    builder.terminal_head = NULL;
    builder.output_link = NULL;
    builder.terminals = NULL;
    builder.replacement_arena = NULL;
    *result = automaton;

cleanup:
    builder_free_pointer(&builder, work);
    builder_free_pointer(&builder, ids);
    free_builder(&builder);
    if (status != ACDAT_OK && automaton != NULL)
        acdat_free(automaton);
    return status;
}

void
acdat_free(AcdatAutomaton *automaton)
{
    AcdatAllocator allocator;

    if (automaton == NULL)
        return;
    allocator = automaton->allocator;
    if (!valid_allocator(&allocator))
        return;

    if (automaton->base != NULL)
        allocator.free(allocator.context, automaton->base);
    if (automaton->check != NULL)
        allocator.free(allocator.context, automaton->check);
    if (automaton->fail != NULL)
        allocator.free(allocator.context, automaton->fail);
    if (automaton->terminal_head != NULL)
        allocator.free(allocator.context, automaton->terminal_head);
    if (automaton->output_link != NULL)
        allocator.free(allocator.context, automaton->output_link);
    if (automaton->terminals != NULL)
        allocator.free(allocator.context, automaton->terminals);
    if (automaton->replacement_arena != NULL)
        allocator.free(allocator.context, automaton->replacement_arena);
    allocator.free(allocator.context, automaton);
}

uint32_t
acdat_direct_transition(const AcdatAutomaton *automaton, uint32_t state,
                        uint8_t symbol)
{
    uint64_t slot;

    if (automaton == NULL || state == 0 || state >= automaton->slot_count)
        return 0;
    slot = (uint64_t) automaton->base[state] + (uint32_t) symbol + 1U;
    if (slot >= automaton->slot_count)
        return 0;
    if (automaton->check[(uint32_t) slot] != state)
        return 0;
    return (uint32_t) slot;
}

static uint32_t
advance_state(const AcdatAutomaton *automaton, uint32_t state, uint8_t symbol)
{
    uint32_t transition = acdat_direct_transition(automaton, state, symbol);

    while (transition == 0 && state != ACDAT_ROOT_STATE)
    {
        state = automaton->fail[state];
        transition = acdat_direct_transition(automaton, state, symbol);
    }
    return transition == 0 ? ACDAT_ROOT_STATE : transition;
}

bool
acdat_contains(const AcdatAutomaton *automaton, const uint8_t *input,
               size_t input_length)
{
    return acdat_contains_ex(automaton, input, input_length, NULL, NULL);
}

bool
acdat_contains_ex(const AcdatAutomaton *automaton, const uint8_t *input,
                  size_t input_length,
                  AcdatProgressCallback progress_callback,
                  void *progress_context)
{
    uint32_t state = ACDAT_ROOT_STATE;
    size_t index;

    if (automaton == NULL || (input_length > 0 && input == NULL) ||
        input_length > UINT32_MAX)
        return false;

    for (index = 0; index < input_length; index++)
    {
        if (progress_callback != NULL && (index & 65535U) == 0)
            progress_callback(progress_context);
        state = advance_state(automaton, state, input[index]);
        if (automaton->terminal_head[state] != ACDAT_NO_INDEX ||
            automaton->output_link[state] != ACDAT_NO_INDEX)
            return true;
    }
    return false;
}

AcdatStatus
acdat_scan(const AcdatAutomaton *automaton, const uint8_t *input,
           size_t input_length, AcdatMatchCallback callback,
           void *callback_context, uint64_t max_matches,
           uint64_t *match_count, AcdatError *error)
{
    return acdat_scan_ex(automaton, input, input_length, callback,
                         callback_context, max_matches, match_count,
                         NULL, NULL, error);
}

AcdatStatus
acdat_scan_ex(const AcdatAutomaton *automaton, const uint8_t *input,
              size_t input_length, AcdatMatchCallback callback,
              void *callback_context, uint64_t max_matches,
              uint64_t *match_count,
              AcdatProgressCallback progress_callback,
              void *progress_context, AcdatError *error)
{
    uint32_t state = ACDAT_ROOT_STATE;
    uint64_t count = 0;
    size_t input_index;

    if (match_count != NULL)
        *match_count = 0;
    if (error != NULL)
        memset(error, 0, sizeof(*error));
    if (automaton == NULL || (input_length > 0 && input == NULL))
    {
        set_error(error, ACDAT_ERR_INVALID_ARGUMENT, SIZE_MAX,
                  "automaton or input pointer is NULL");
        return ACDAT_ERR_INVALID_ARGUMENT;
    }
    if (input_length > UINT32_MAX)
    {
        set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                  "input length exceeds uint32 range");
        return ACDAT_ERR_SIZE_OVERFLOW;
    }

    for (input_index = 0; input_index < input_length; input_index++)
    {
        uint32_t output_state;

        if (progress_callback != NULL && (input_index & 65535U) == 0)
            progress_callback(progress_context);
        state = advance_state(automaton, state, input[input_index]);
        output_state = state;
        for (;;)
        {
            uint32_t terminal_handle =
                automaton->terminal_head[output_state];

            while (terminal_handle != ACDAT_NO_INDEX)
            {
                const AcdatTerminal *terminal =
                    &automaton->terminals[terminal_handle - 1U];
                AcdatMatch match;

                if (max_matches != 0 && count >= max_matches)
                {
                    set_error(error, ACDAT_ERR_SIZE_OVERFLOW, SIZE_MAX,
                              "match count exceeds limit %llu",
                              (unsigned long long) max_matches);
                    if (match_count != NULL)
                        *match_count = count;
                    return ACDAT_ERR_SIZE_OVERFLOW;
                }

                match.pattern_id = terminal->pattern_id;
                match.start_byte =
                    (uint32_t) input_index + 2U - terminal->byte_length;
                match.byte_length = terminal->byte_length;
                match.char_length = terminal->char_length;
                match.priority = terminal->priority;
                match.canonical_ordinal = terminal->canonical_ordinal;
                match.terminal_index = terminal_handle - 1U;
                count++;
                if (callback != NULL && !callback(&match, callback_context))
                {
                    if (match_count != NULL)
                        *match_count = count;
                    return ACDAT_OK;
                }
                terminal_handle = terminal->next_terminal;
            }

            output_state = automaton->output_link[output_state];
            if (output_state == ACDAT_NO_INDEX)
                break;
        }
    }

    if (match_count != NULL)
        *match_count = count;
    return ACDAT_OK;
}

const char *
acdat_status_name(AcdatStatus status)
{
    switch (status)
    {
        case ACDAT_OK:
            return "ok";
        case ACDAT_ERR_INVALID_ARGUMENT:
            return "invalid_argument";
        case ACDAT_ERR_EMPTY_PATTERN:
            return "empty_pattern";
        case ACDAT_ERR_DUPLICATE_PATTERN_ID:
            return "duplicate_pattern_id";
        case ACDAT_ERR_OUT_OF_MEMORY:
            return "out_of_memory";
        case ACDAT_ERR_SIZE_OVERFLOW:
            return "size_overflow";
        case ACDAT_ERR_BUILD_FAILED:
            return "build_failed";
    }
    return "unknown";
}
