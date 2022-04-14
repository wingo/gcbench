#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "large-object-space.h"
#include "precise-roots.h"

struct semi_space {
  uintptr_t hp;
  uintptr_t limit;
  uintptr_t base;
  size_t size;
  long count;
};
struct heap {
  struct semi_space semi_space;
  struct large_object_space large_object_space;
};
// One mutator per space, can just store the heap in the mutator.
struct mutator {
  struct heap heap;
  struct handle *roots;
};

static inline struct heap* mutator_heap(struct mutator *mut) {
  return &mut->heap;
}
static inline struct semi_space* heap_semi_space(struct heap *heap) {
  return &heap->semi_space;
}
static inline struct large_object_space* heap_large_object_space(struct heap *heap) {
  return &heap->large_object_space;
}
static inline struct semi_space* mutator_semi_space(struct mutator *mut) {
  return heap_semi_space(mutator_heap(mut));
}

static const uintptr_t ALIGNMENT = 8;

static uintptr_t align_up(uintptr_t addr, size_t align) {
  return (addr + align - 1) & ~(align-1);
}

#define GC_HEADER uintptr_t _gc_header

static inline void clear_memory(uintptr_t addr, size_t size) {
  memset((char*)addr, 0, size);
}

static void collect(struct mutator *mut) NEVER_INLINE;
static void collect_for_alloc(struct mutator *mut, size_t bytes) NEVER_INLINE;

static void visit(void **loc, void *visit_data);

static int semi_space_steal_pages(struct semi_space *space, size_t npages) {
  size_t page_size = getpagesize();

  if (npages & 1) npages++;
  size_t half_size = (npages * page_size) >> 1;
  if (space->limit - space->hp < half_size)
    return 0;

  space->limit -= half_size;
  size_t tospace_offset = space->limit - space->base;
  madvise((void*)(space->base + tospace_offset), half_size, MADV_DONTNEED);
  size_t fromspace_offset =
    (tospace_offset + (space->size >> 1)) & (space->size - 1);
  madvise((void*)(space->base + fromspace_offset), half_size, MADV_DONTNEED);
  return 1;
}

static void flip(struct semi_space *space) {
  uintptr_t split = space->base + (space->size >> 1);
  if (space->hp <= split) {
    space->hp = split;
    space->limit = space->base + space->size;
  } else {
    space->hp = space->base;
    space->limit = split;
  }
  space->count++;
}  

static void* copy(struct semi_space *space, uintptr_t kind, void *obj) {
  size_t size;
  switch (kind) {
#define COMPUTE_SIZE(name, Name, NAME) \
    case ALLOC_KIND_##NAME: \
      size = name##_size(obj); \
      break;
    FOR_EACH_HEAP_OBJECT_KIND(COMPUTE_SIZE)
#undef COMPUTE_SIZE
  default:
    abort ();
  }
  void *new_obj = (void*)space->hp;
  memcpy(new_obj, obj, size);
  *(uintptr_t*) obj = space->hp;
  space->hp += align_up (size, ALIGNMENT);
  return new_obj;
}

static uintptr_t scan(struct heap *heap, uintptr_t grey) {
  void *obj = (void*)grey;
  uintptr_t kind = *(uintptr_t*) obj;
  switch (kind) {
#define SCAN_OBJECT(name, Name, NAME) \
    case ALLOC_KIND_##NAME: \
      visit_##name##_fields((Name*)obj, visit, heap); \
      return grey + align_up(name##_size((Name*)obj), ALIGNMENT);
    FOR_EACH_HEAP_OBJECT_KIND(SCAN_OBJECT)
#undef SCAN_OBJECT
  default:
    abort ();
  }
}

static void* forward(struct semi_space *space, void *obj) {
  uintptr_t header_word = *(uintptr_t*)obj;
  switch (header_word) {
#define CASE_ALLOC_KIND(name, Name, NAME) \
    case ALLOC_KIND_##NAME:
    FOR_EACH_HEAP_OBJECT_KIND(CASE_ALLOC_KIND)
#undef CASE_ALLOC_KIND
    return copy(space, header_word, obj);
  default:
    return (void*)header_word;
  }
}  

static void visit_semi_space(struct heap *heap, struct semi_space *space,
                             void **loc, void *obj) {
  *loc = forward(space, obj);
}

static void visit_large_object_space(struct heap *heap,
                                     struct large_object_space *space,
                                     void **loc, void *obj) {
  if (large_object_space_copy(space, (uintptr_t)obj))
    scan(heap, (uintptr_t)obj);
}

static int semi_space_contains(struct semi_space *space, void *obj) {
  return (((uintptr_t)obj) - space->base) < space->size;
}

static void visit(void **loc, void *visit_data) {
  struct heap *heap = visit_data;
  void *obj = *loc;
  if (obj == NULL)
    return;
  if (semi_space_contains(heap_semi_space(heap), obj))
    visit_semi_space(heap, heap_semi_space(heap), loc, obj);
  else if (large_object_space_contains(heap_large_object_space(heap), obj))
    visit_large_object_space(heap, heap_large_object_space(heap), loc, obj);
  else
    abort();
}

static void collect(struct mutator *mut) {
  struct heap *heap = mutator_heap(mut);
  struct semi_space *semi = heap_semi_space(heap);
  struct large_object_space *large = heap_large_object_space(heap);
  // fprintf(stderr, "start collect #%ld:\n", space->count);
  large_object_space_start_gc(large);
  flip(semi);
  uintptr_t grey = semi->hp;
  for (struct handle *h = mut->roots; h; h = h->next)
    visit(&h->v, heap);
  // fprintf(stderr, "pushed %zd bytes in roots\n", space->hp - grey);
  while(grey < semi->hp)
    grey = scan(heap, grey);
  large_object_space_finish_gc(large);
  semi_space_steal_pages(semi, large->live_pages_at_last_collection);
  // fprintf(stderr, "%zd bytes copied\n", (space->size>>1)-(space->limit-space->hp));
}

static void collect_for_alloc(struct mutator *mut, size_t bytes) {
  collect(mut);
  struct semi_space *space = mutator_semi_space(mut);
  if (space->limit - space->hp < bytes) {
    fprintf(stderr, "ran out of space, heap size %zu\n", space->size);
    abort();
  }
}

static const size_t LARGE_OBJECT_THRESHOLD = 8192;
static void* allocate_large(struct mutator *mut, enum alloc_kind kind,
                            size_t size) {
  struct heap *heap = mutator_heap(mut);
  struct large_object_space *space = heap_large_object_space(heap);
  struct semi_space *semi_space = heap_semi_space(heap);

  size_t npages = large_object_space_npages(space, size);
  if (!semi_space_steal_pages(semi_space, npages)) {
    collect(mut);
    if (!semi_space_steal_pages(semi_space, npages)) {
      fprintf(stderr, "ran out of space, heap size %zu\n", semi_space->size);
      abort();
    }
  }

  void *ret = large_object_space_alloc(space, npages);
  if (!ret)
    ret = large_object_space_obtain_and_alloc(space, npages);

  if (!ret) {
    perror("weird: we have the space but mmap didn't work");
    abort();
  }

  *(uintptr_t*)ret = kind;
  return ret;
}

static inline void* allocate(struct mutator *mut, enum alloc_kind kind,
                             size_t size) {
  if (size >= LARGE_OBJECT_THRESHOLD)
    return allocate_large(mut, kind, size);

  struct semi_space *space = mutator_semi_space(mut);
  while (1) {
    uintptr_t addr = space->hp;
    uintptr_t new_hp = align_up (addr + size, ALIGNMENT);
    if (space->limit < new_hp) {
      collect_for_alloc(mut, size);
      continue;
    }
    space->hp = new_hp;
    void *ret = (void *)addr;
    uintptr_t *header_word = ret;
    *header_word = kind;
    // FIXME: Allow allocator to avoid initializing pointerless memory?
    // if (kind == NODE)
    clear_memory(addr + sizeof(uintptr_t), size - sizeof(uintptr_t));
    return ret;
  }
}
static inline void* allocate_pointerless(struct mutator *mut,
                                         enum alloc_kind kind, size_t size) {
  return allocate(mut, kind, size);
}

static inline void init_field(void **addr, void *val) {
  *addr = val;
}
static inline void set_field(void **addr, void *val) {
  *addr = val;
}
static inline void* get_field(void **addr) {
  return *addr;
}

static int initialize_semi_space(struct semi_space *space, size_t size) {
  void *mem = mmap(NULL, size, PROT_READ|PROT_WRITE,
                   MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) {
    perror("mmap failed");
    return 0;
  }

  space->hp = space->base = (uintptr_t) mem;
  space->size = size;
  space->count = -1;
  flip(space);

  return 1;
}
  
static int initialize_gc(size_t heap_size, struct heap **heap,
                         struct mutator **mut) {
  *mut = calloc(1, sizeof(struct mutator));
  if (!*mut) abort();
  *heap = mutator_heap(*mut);

  struct semi_space *space = mutator_semi_space(*mut);
  if (!initialize_semi_space(space, heap_size))
    return 0;
  if (!large_object_space_init(heap_large_object_space(*heap), *heap))
    return 0;
  
  (*mut)->roots = NULL;

  return 1;
}

static struct mutator* initialize_gc_for_thread(uintptr_t *stack_base,
                                                struct heap *heap) {
  fprintf(stderr,
          "Semispace copying collector not appropriate for multithreaded use.\n");
  exit(1);
}
static void finish_gc_for_thread(struct mutator *space) {
}

static void* call_without_gc(struct mutator *mut, void* (*f)(void*),
                             void *data) {
  // Can't be threads, then there won't be collection.
  return f(data);
}

static inline void print_start_gc_stats(struct heap *heap) {
}

static inline void print_end_gc_stats(struct heap *heap) {
  struct semi_space *space = heap_semi_space(heap);
  printf("Completed %ld collections\n", space->count);
  printf("Heap size is %zd\n", space->size);
}
