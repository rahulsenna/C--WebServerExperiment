// USAGE:
//   #define ARENA_IMPLEMENTATION
//   #include "arena.h"
//
// Optional #define before including:
//   #define ARENA_ENABLE_FREE_LIST 1   // Enable block recycling (default: 1)

#ifndef ARENA_H
#define ARENA_H

#include "defines.h"

#ifdef __cplusplus
#define ARENA_ALIGNOF(T) alignof(T)
#else
#define ARENA_ALIGNOF(T) _Alignof(T)
#endif

#ifdef __cplusplus
extern "C"
{
#endif

  // Configuration
#ifndef ARENA_ENABLE_FREE_LIST
#define ARENA_ENABLE_FREE_LIST 1
#endif

#define ARENA_HEADER_SIZE 128

  typedef u64 ArenaFlags;
  enum
  {
    ArenaFlag_NoChain = (1 << 0),
    ArenaFlag_LargePages = (1 << 1),
  };

  typedef struct Arena Arena;
  struct Arena
  {
    Arena *prev;    // previous arena in chain
    Arena *current; // current arena in chain
    ArenaFlags flags;
    u64 cmt_size;
    u64 res_size;
    u64 base_pos;
    u64 pos;
    u64 cmt;
    u64 res;
#if ARENA_ENABLE_FREE_LIST
    Arena *free_last;
#endif
  };

  typedef struct Temp Temp;
  struct Temp
  {
    Arena *arena;
    u64 pos;
  };

  // API
  Arena *arena_alloc(u64 reserve_size, u64 commit_size, ArenaFlags flags);
  void arena_release(Arena *arena);
  void *arena_push(Arena *arena, u64 size, u64 align, b32 zero);
  u64 arena_pos(Arena *arena);
  void arena_pop_to(Arena *arena, u64 pos);
  void arena_clear(Arena *arena);
  void arena_pop(Arena *arena, u64 amt);
  Temp temp_begin(Arena *arena);
  void temp_end(Temp temp);

  // Helper macros
#define push_array_no_zero(a, T, c) (T *)arena_push((a), sizeof(T) * (c), ARENA_ALIGNOF(T), 0)
#define push_array(a, T, c) (T *)arena_push((a), sizeof(T) * (c), ARENA_ALIGNOF(T), 1)
#define push_struct_no_zero(a, T) push_array_no_zero(a, T, 1)
#define push_struct(a, T) push_array(a, T, 1)

#ifdef __cplusplus
}
#endif

//=============================================================================
// IMPLEMENTATION
//=============================================================================

#ifdef ARENA_IMPLEMENTATION

#include <string.h>
#include <assert.h>

// Platform detection
#if defined(_WIN32)
#define ARENA_WINDOWS 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#elif defined(__APPLE__)
#define ARENA_MACOS 1
#include <sys/mman.h>
#include <unistd.h>
#elif defined(__linux__)
#define ARENA_LINUX 1
#include <sys/mman.h>
#include <unistd.h>
#endif

// Internal helpers
#define AlignPow2(x, b) (((x) + (b) - 1) & (~((b) - 1)))
#define Min(a, b) ((a) < (b) ? (a) : (b))
#define Max(a, b) ((a) > (b) ? (a) : (b))
#define ClampTop(x, a) Min(x, a)
#define ClampBot(a, x) Max(a, x)
#define SLLStackPush_N(f, n, next) ((n)->next = (f), (f) = (n))

// OS abstraction layer
static void *arena__os_reserve(u64 size)
{
#if ARENA_WINDOWS
  void *ptr = VirtualAlloc(0, size, MEM_RESERVE, PAGE_NOACCESS);
  return ptr;
#else
  void *ptr = mmap(0, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (ptr == MAP_FAILED)
    return 0;
  return ptr;
#endif
}

static void arena__os_commit(void *ptr, u64 size)
{
#if ARENA_WINDOWS
  VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE);
#else
  mprotect(ptr, size, PROT_READ | PROT_WRITE);
#endif
}

static void arena__os_release(void *ptr, u64 size)
{
#if ARENA_WINDOWS
  (void)size;
  VirtualFree(ptr, 0, MEM_RELEASE);
#else
  munmap(ptr, size);
#endif
}

static u64 arena__os_page_size(void)
{
#if ARENA_WINDOWS
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwPageSize;
#else
  return (u64)sysconf(_SC_PAGESIZE);
#endif
}

// Arena implementation
Arena *arena_alloc(u64 reserve_size, u64 commit_size, ArenaFlags flags)
{
  u64 page_size = arena__os_page_size();

  reserve_size = AlignPow2(reserve_size, page_size);
  commit_size = AlignPow2(commit_size, page_size);

  void *base = arena__os_reserve(reserve_size);
  if (!base)
    return 0;

  arena__os_commit(base, commit_size);

  Arena *arena = (Arena *)base;
  memset(arena, 0, sizeof(Arena));
  arena->current = arena;
  arena->flags = flags;
  arena->cmt_size = commit_size;
  arena->res_size = reserve_size;
  arena->base_pos = 0;
  arena->pos = ARENA_HEADER_SIZE;
  arena->cmt = commit_size;
  arena->res = reserve_size;
#if ARENA_ENABLE_FREE_LIST
  arena->free_last = 0;
#endif

  return arena;
}

void arena_release(Arena *arena)
{
  for (Arena *n = arena->current, *prev = 0; n != 0; n = prev)
  {
    prev = n->prev;
    arena__os_release(n, n->res);
  }
#if ARENA_ENABLE_FREE_LIST
  for (Arena *n = arena->free_last, *prev = 0; n != 0; n = prev)
  {
    prev = n->prev;
    arena__os_release(n, n->res);
  }
#endif
}

void *arena_push(Arena *arena, u64 size, u64 align, b32 zero)
{
  Arena *current = arena->current;
  u64 pos_pre = AlignPow2(current->pos, align);
  u64 pos_pst = pos_pre + size;

  if (current->res < pos_pst && !(arena->flags & ArenaFlag_NoChain))
  {
    Arena *new_block = 0;

#if ARENA_ENABLE_FREE_LIST
    Arena *prev_block = 0;
    for (new_block = arena->free_last; new_block != 0; prev_block = new_block, new_block = new_block->prev)
    {
      if (new_block->res >= AlignPow2(new_block->pos, align) + size)
      {
        if (prev_block)
        {
          prev_block->prev = new_block->prev;
        }
        else
        {
          arena->free_last = new_block->prev;
        }
        break;
      }
    }
#endif

    if (new_block == 0)
    {
      u64 res_size = Max(current->res_size, AlignPow2(size + ARENA_HEADER_SIZE, arena__os_page_size()));
      u64 cmt_size = Max(current->cmt_size, AlignPow2(size + ARENA_HEADER_SIZE, arena__os_page_size()));

      new_block = arena_alloc(res_size, cmt_size, current->flags);
      if (!new_block)
        return 0;
    }

    new_block->base_pos = current->base_pos + current->res;
    SLLStackPush_N(arena->current, new_block, prev);

    current = new_block;
    pos_pre = AlignPow2(current->pos, align);
    pos_pst = pos_pre + size;
  }

  if (current->cmt < pos_pst)
  {
    u64 cmt_pst_aligned = AlignPow2(pos_pst, current->cmt_size);
    u64 cmt_pst_clamped = ClampTop(cmt_pst_aligned, current->res);
    u64 cmt_size = cmt_pst_clamped - current->cmt;
    u8 *cmt_ptr = (u8 *)current + current->cmt;

    arena__os_commit(cmt_ptr, cmt_size);
    current->cmt = cmt_pst_clamped;
  }

  void *result = 0;
  if (current->cmt >= pos_pst)
  {
    result = (u8 *)current + pos_pre;
    current->pos = pos_pst;

    if (zero)
    {
      memset(result, 0, size);
    }
  }

  return result;
}

u64 arena_pos(Arena *arena)
{
  Arena *current = arena->current;
  return current->base_pos + current->pos;
}

void arena_pop_to(Arena *arena, u64 pos)
{
  u64 big_pos = ClampBot(ARENA_HEADER_SIZE, pos);
  Arena *current = arena->current;

#if ARENA_ENABLE_FREE_LIST
  for (Arena *prev = 0; current->base_pos >= big_pos; current = prev)
  {
    prev = current->prev;
    current->pos = ARENA_HEADER_SIZE;
    SLLStackPush_N(arena->free_last, current, prev);
  }
#else
  for (Arena *prev = 0; current->base_pos >= big_pos; current = prev)
  {
    prev = current->prev;
    arena__os_release(current, current->res);
  }
#endif

  arena->current = current;
  u64 new_pos = big_pos - current->base_pos;
  assert(new_pos <= current->pos);
  current->pos = new_pos;
}

void arena_clear(Arena *arena)
{
  arena_pop_to(arena, 0);
}

void arena_pop(Arena *arena, u64 amt)
{
  u64 pos_old = arena_pos(arena);
  u64 pos_new = pos_old > amt ? pos_old - amt : 0;
  arena_pop_to(arena, pos_new);
}

Temp temp_begin(Arena *arena)
{
  Temp temp = {arena, arena_pos(arena)};
  return temp;
}

void temp_end(Temp temp)
{
  arena_pop_to(temp.arena, temp.pos);
}

#endif // ARENA_IMPLEMENTATION
#endif // ARENA_H