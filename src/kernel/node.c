#include <kernel.h>

typedef struct {
    // combine(name_hash, parent_hash)
    uint64_t full_hash;
    // hash(name)
    uint64_t name_hash;
    // last byte must be 0
    char name[256];
} Node;

typedef struct {
    // every 4b represents the relative ages of the nodes
    uint64_t ages;
    Node nodes[16];
} NodeCacheSet;

static uint64_t* node_set_ages;
static Node* node_cache;

static void init_nodes(void) {

}

static Node* find_node(const char* path) {
    return NULL;
}
