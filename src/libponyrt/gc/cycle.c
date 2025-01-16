#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include "cycle.h"
#include "../actor/actor.h"
#include "../sched/cpu.h"
#include "../sched/scheduler.h"
#include "../ds/stack.h"
#include "../ds/hash.h"
#include "../mem/pool.h"
#include "ponyassert.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#define CD_MAX_CHECK_BLOCKED 1000

//
// About the Pony cycle detector
//
// The cycle detector exists to reap actors and cycles of actors. Not every
// Pony program needs the cycle detector. If an application contains no cycles
// amongst it's actors or is able via its shutdown mechanism to break
// those cycles then the cycle detector isn't need and is extra overhead. Such
// programs use the "ponynoblock" runtime option to turn off the cycle
// detector.
//
// Some applications do need a cycle detector in order to garbage collect
// actors. The job of the cycle detector is find groups of actors (cycles) where
// no more work is possible because all of the actors in the cycle are blocked.
// When the cycle detector finds a blocked cycle, it deletes the members of
// cycle.
//
// The cycle detector is a special actor that is implemented here in cycle.c in
// c rather than being implemented in Pony. Like other actors, it is run by
// the various schedulers. Unlike other actors, it does not participate in the
// scheduler's backpressure system as the cycle detector is marked as a "system
// actor". All system actors are not part of the backpressure system.
//
// The cycle detector has evolved over the years and is much more complicated
// now than it was in early versions of Pony. "In the beginning", every actor
// would send a message upon its creation to the cycle detector. This allowed
// the detector to track all the actors ahd use a protocol that still mostly
// exists to query actor's about there view of the world and put together a
// "omniscient" view of actor relations and find cycles of blocked actors.
//
// Early cycle detector designs had a few problems.
//
// 1. Every actor was communicating with the cycle detector which made a the
//    cycle detector a bottleneck in many applications.
// 2. The cycle detector was run like other actors which meant that it often
//    couldn't keep up with particularly "CD busy applications". When the cycle
//    detector can't keep up, memory pressure grows from it's queue.
// 3. The cycle detector was scheduled like every other actor and would steal
//    cycles from "real work actors". In some applications a non-trivial amount
//    of CPU time was spent on cycle detection.
// 4. It was easy to overload the cycle detector by creating large numbers of
//    actors in a short period of time.
//
// Despite these problems, the early cycle detector designs were relatively
// easy to understand and didn't contain possible race conditions.
//
// Over time, as we have evolved the cycle detector to be less likely to suffer
// from memory pressure or to be overly grabby with CPU time, it has gotten more
// and more complicated. It would be rather easy now when modifying the cycle
// detector to introduce dangerous race conditions that could result in crashing
// Pony programs.
//
// Changes that have been made to the cycle detector over time:
//
// The cycle detector is no longer run like a regular actor. Instead, it will
// only ever be run by scheduler 0. And it will only every be run every X amount
// of units (which is configurable using the `cdinternal` option). This
// scheduling change greatly dropped the performance impact of the cycle
// detector on "average" Pony programs.
//
// Early versions of the cycle detector looked at every actor that it knew about
// looking for cycles each time it ran. This could result in large amounts of
// CPU usage and severely impact performance. Performance was most impacted as
// the number of actors grew and as the number of CPUs shrank. Single threaded
// Pony programs with a lot of actors could spend a very large amount of time
// looking for cycles.
//
// Every actor informed the cycle detector of its existence via message passing
// when the actor was created. This could cause a lot of additional message
// activity.
//
// Every actor informed the cycle detector of its existence even if it was
// never blocked. This resulted in a lot of extra work as the purpose of the
// cycle detector is to find cycles of blocked actors. Despite this, all actors
// were informing the cycle detector of their existence.
//
// The relationship between individual actors and the cycle detector is now as
// follows.
//
// Actors only inform the cycle detector of their existence if they are blocked.
//
// We've introduced a concept of "orphaned" actor that at the time of their
// creation have a reference count of 0. Orphaned actors whose rc never gets
// above 0 can delete themselves locally like they would if `ponynoblock` was
// in use.
//
// Any actor that has had a reference count above 0 but currently has a
// reference count of 0 and is blocked can inform the cycle detector that it is
// blocked (and is isolated) so that the cycle detector can speed up reaping it.
//
// The three above changes can, for many applications, have a huge impact on
// performance and memory usage. However, these gains come at a cost.
//
// Early versions of the cycle detector were trivially race condition free. Now,
// it is non-trivial to keep cycle detector/actor interactions race free.
//
// Where there was once only one way for the cycle detector to become aware of
// an actor, there are now two. An actor might inform the cycle detector of
// itself when it blocks. A different actor that knows about an actor, might
// also have shared its view of the world with the cycle detector such that
// the cycle detector knows about the actor without it ever having contacted the
// cycle detector. This can result in the cycle detector having multiple
// protcol messages in flight that could be leading the deletion of an actor.
// Care must be taken in the cycle detector to not contact actors that are
// "pending deletion" as it could lead to a double-free.
//
// There are critical sections of "deletion" code that are non-atomic in both
// the actor and the cycle detector where for any actor, only the actor or
// cycle detector can be executing. If both sides were to be executing critical
// sections at the same time, a double free could result. We have an atomic
// in place on a per actor basis that is used to prevent both sides from
// executing their critical sections at the same time. If either can not
// acquire the critical section atomic, it means the other side is in their
// section and an invariant needed for safe deletion is about to be broken.
//
// If all of this sounds complicated, it is. Tread carefully. The three people
// who know the cycle detector best all approved performance improvements
// around the time of Pony 0.38.0 that introduced race conditions.
//
// We feel the additional complexity is worth it because the performance gains
// have been so large. However, we are looking at doing away with the cycle
// detector entirely and coming up with a decentralized approach to detecting
// actor cycles and reaping them. Until that time, we have the current cycle
// detector that has evolved to this state over time.

typedef struct block_msg_t
{
  pony_msg_t msg;
  pony_actor_t* actor;
  size_t rc;
  deltamap_t* delta;
} block_msg_t;

typedef struct view_t view_t;
typedef struct perceived_t perceived_t;

typedef struct viewref_t
{
  view_t* view;
  size_t rc;
} viewref_t;

static size_t viewref_hash(viewref_t* vref)
{
  return ponyint_hash_ptr(vref->view);
}

static bool viewref_cmp(viewref_t* a, viewref_t* b)
{
  return a->view == b->view;
}

static void viewref_free(viewref_t* vref)
{
  POOL_FREE(viewref_t, vref);
}

DECLARE_STACK(ponyint_viewrefstack, viewrefstack_t, viewref_t);
DEFINE_STACK(ponyint_viewrefstack, viewrefstack_t, viewref_t);

DECLARE_HASHMAP(ponyint_viewrefmap, viewrefmap_t, viewref_t);
DEFINE_HASHMAP(ponyint_viewrefmap, viewrefmap_t, viewref_t, viewref_hash,
  viewref_cmp, viewref_free);

#ifdef USE_RUNTIMESTATS
static size_t viewrefmap_total_mem_size(viewrefmap_t* map)
{
  return ponyint_viewrefmap_mem_size(map)
    + (ponyint_viewrefmap_size(map) * sizeof(viewref_t));
}

static size_t viewrefmap_total_alloc_size(viewrefmap_t* map)
{
  return ponyint_viewrefmap_alloc_size(map)
    + (ponyint_viewrefmap_size(map) * sizeof(viewref_t));
}
#endif

typedef enum
{
  COLOR_BLACK,
  COLOR_GREY,
  COLOR_WHITE
} ponyint_color_t;

struct view_t
{
  pony_actor_t* actor;
  size_t rc;
  uint32_t view_rc;
  bool blocked;
  bool deferred;
  uint8_t color;
  viewrefmap_t map;
  perceived_t* perceived;
};

static size_t view_hash(view_t* view)
{
  return ponyint_hash_ptr(view->actor);
}

static bool view_cmp(view_t* a, view_t* b)
{
  return a->actor == b->actor;
}

#ifdef USE_RUNTIMESTATS
static void track_mem_view_free(view_t* view);
#endif

static void view_free(view_t* view)
{
  view->view_rc--;

  if(view->view_rc == 0)
  {
#ifdef USE_RUNTIMESTATS
    track_mem_view_free(view);
#endif

    ponyint_viewrefmap_destroy(&view->map);
    POOL_FREE(view_t, view);
  }
}

// no element free for a viewmap. views are kept in many maps.
DECLARE_HASHMAP(ponyint_viewmap, viewmap_t, view_t);
DEFINE_HASHMAP(ponyint_viewmap, viewmap_t, view_t, view_hash, view_cmp, NULL);

struct perceived_t
{
  size_t token;
  size_t ack;
  viewmap_t map;
};

static size_t perceived_hash(perceived_t* per)
{
  return ponyint_hash_size(per->token);
}

static bool perceived_cmp(perceived_t* a, perceived_t* b)
{
  return a->token == b->token;
}

#ifdef USE_RUNTIMESTATS
static void track_mem_perceived_free(perceived_t* per);
#endif

static void perceived_free(perceived_t* per)
{
#ifdef USE_RUNTIMESTATS
  track_mem_perceived_free(per);
#endif
  ponyint_viewmap_destroy(&per->map);
  POOL_FREE(perceived_t, per);
}

DECLARE_HASHMAP(ponyint_perceivedmap, perceivedmap_t, perceived_t);
DEFINE_HASHMAP(ponyint_perceivedmap, perceivedmap_t, perceived_t,
  perceived_hash, perceived_cmp, perceived_free);

DECLARE_STACK(ponyint_pendingdestroystack, pendingdestroystack_t, pony_actor_t);
DEFINE_STACK(ponyint_pendingdestroystack, pendingdestroystack_t, pony_actor_t);

typedef struct detector_t
{
  pony_actor_pad_t pad;

  size_t next_token;
  size_t detect_interval;
  size_t last_checked;

  viewmap_t views;
  viewmap_t deferred;
  perceivedmap_t perceived;

  size_t attempted;
  size_t detected;
  size_t collected;

  size_t created;
  size_t destroyed;

#ifdef USE_RUNTIMESTATS
  size_t mem_used;
  size_t mem_allocated;
#endif
} detector_t;

static pony_actor_t* cycle_detector;

#ifdef USE_RUNTIMESTATS
static void track_mem_view_free(view_t* view)
{
  detector_t* d = (detector_t*)cycle_detector;
  d->mem_used -= sizeof(view_t);
  d->mem_allocated -= POOL_ALLOC_SIZE(view_t);
  d->mem_used -= viewrefmap_total_mem_size(&view->map);
  d->mem_allocated -= viewrefmap_total_alloc_size(&view->map);
}

static void track_mem_perceived_free(perceived_t* per)
{
  detector_t* d = (detector_t*)cycle_detector;
  d->mem_used -= sizeof(perceived_t);
  d->mem_allocated -= POOL_ALLOC_SIZE(perceived_t);
  d->mem_used -= ponyint_viewmap_mem_size(&per->map);
  d->mem_allocated -= ponyint_viewmap_alloc_size(&per->map);
}
#endif

static view_t* get_view(detector_t* d, pony_actor_t* actor, bool create)
{
  pony_assert(actor != NULL);

  view_t key;
  key.actor = actor;
  size_t index = HASHMAP_UNKNOWN;

  view_t* view = ponyint_viewmap_get(&d->views, &key, &index);

  if((view == NULL) && create)
  {
    view = (view_t*)POOL_ALLOC(view_t);
    memset(view, 0, sizeof(view_t));
    view->actor = actor;
    view->view_rc = 1;

#ifdef USE_RUNTIMESTATS
    d->mem_used += sizeof(view_t);
    d->mem_allocated += POOL_ALLOC_SIZE(view_t);
    size_t mem_size_before = ponyint_viewmap_mem_size(&d->views);
    size_t alloc_size_before = ponyint_viewmap_alloc_size(&d->views);
#endif
    // didn't find it in the map but index is where we can put the
    // new one without another search
    ponyint_viewmap_putindex(&d->views, view, index);
    d->created++;
#ifdef USE_RUNTIMESTATS
    size_t mem_size_after = ponyint_viewmap_mem_size(&d->views);
    size_t alloc_size_after = ponyint_viewmap_alloc_size(&d->views);
    d->mem_used += (mem_size_after - mem_size_before);
    d->mem_allocated += (alloc_size_after - alloc_size_before);
#endif
  }

  return view;
}

static void apply_delta(detector_t* d, view_t* view, deltamap_t* map)
{
  if(map == NULL)
    return;

  // update refs with delta
  size_t i = HASHMAP_BEGIN;
  delta_t* delta;

  while((delta = ponyint_deltamap_next(map, &i)) != NULL)
  {
    // If rc is 0, we skip creating a view for the actor.
    pony_actor_t* actor = ponyint_delta_actor(delta);
    size_t rc = ponyint_delta_rc(delta);

    // If the referenced actor has never blocked, we will insert a view_t
    // that has blocked set to false.
    view_t* find = get_view(d, actor, rc > 0);

    if(find == NULL)
      continue;

    viewref_t key;
    key.view = find;

    if(rc > 0)
    {
      size_t index = HASHMAP_UNKNOWN;
      viewref_t* ref = ponyint_viewrefmap_get(&view->map, &key, &index);

      if(ref == NULL)
      {
        ref = (viewref_t*)POOL_ALLOC(viewref_t);
        ref->view = find;

#ifdef USE_RUNTIMESTATS
        d->mem_used += sizeof(viewref_t);
        d->mem_allocated += POOL_ALLOC_SIZE(viewref_t);
        size_t mem_size_before = ponyint_viewrefmap_mem_size(&view->map);
        size_t alloc_size_before = ponyint_viewrefmap_alloc_size(&view->map);
#endif
        // didn't find it in the map but index is where we can put the
        // new one without another search
        ponyint_viewrefmap_putindex(&view->map, ref, index);
        find->view_rc++;

#ifdef USE_RUNTIMESTATS
        size_t mem_size_after = ponyint_viewrefmap_mem_size(&view->map);
        size_t alloc_size_after = ponyint_viewrefmap_alloc_size(&view->map);
        d->mem_used += (mem_size_after - mem_size_before);
        d->mem_allocated += (alloc_size_after - alloc_size_before);
#endif
      }

      ref->rc = rc;
    } else {
      viewref_t* ref = ponyint_viewrefmap_remove(&view->map, &key);

      if(ref != NULL)
      {
#ifdef USE_RUNTIMESTATS
        d->mem_used -= sizeof(viewref_t);
        d->mem_allocated -= POOL_ALLOC_SIZE(viewref_t);
#endif
        viewref_free(ref);
        view_free(find);
      }
    }
  }

#ifdef USE_RUNTIMESTATS
  d->mem_used -= ponyint_deltamap_total_mem_size(map);
  d->mem_allocated -= ponyint_deltamap_total_alloc_size(map);
#endif
  ponyint_deltamap_free(map);
}

static bool mark_grey(detector_t* d, view_t* view, size_t rc)
{
  if(!view->blocked)
    return false;

  if(view->deferred)
  {
    ponyint_viewmap_remove(&d->deferred, view);
    view->deferred = false;
  }

  // if rc > view->rc, this will show as a huge number, which is fine
  // it just means our view is out of date for this actor
  view->rc -= rc;

  if(view->color == COLOR_GREY)
    return false;

  pony_assert(view->color == COLOR_BLACK);
  view->color = COLOR_GREY;
  return true;
}

static void scan_grey(detector_t* d, view_t* view, size_t rc)
{
  viewref_t* ref;
  viewref_t head = {view, rc};
  viewrefstack_t* stack = ponyint_viewrefstack_push(NULL, &head);

  while(stack != NULL)
  {
    stack = ponyint_viewrefstack_pop(stack, &ref);

    if(mark_grey(d, ref->view, ref->rc))
    {
      size_t i = HASHMAP_BEGIN;
      viewref_t* child;

      while((child = ponyint_viewrefmap_next(&ref->view->map, &i)) != NULL)
        stack = ponyint_viewrefstack_push(stack, child);
    }
  }
}

static bool mark_black(view_t* view, size_t rc, int* count)
{
  if(!view->blocked)
  {
    pony_assert(view->color == COLOR_BLACK);
    return false;
  }

  view->rc += rc;

  if(view->color == COLOR_BLACK)
    return false;

  // keep a count of white nodes colored black
  if(view->color == COLOR_WHITE)
    *count = *count + 1;

  view->color = COLOR_BLACK;
  return true;
}

static int scan_black(view_t* view, size_t rc)
{
  // return the count of white nodes colored black
  int count = 0;
  viewref_t* ref;
  viewref_t head = {view, rc};
  viewrefstack_t* stack = ponyint_viewrefstack_push(NULL, &head);

  while(stack != NULL)
  {
    stack = ponyint_viewrefstack_pop(stack, &ref);

    if(mark_black(ref->view, ref->rc, &count))
    {
      size_t i = HASHMAP_BEGIN;
      viewref_t* child;

      while((child = ponyint_viewrefmap_next(&ref->view->map, &i)) != NULL)
        stack = ponyint_viewrefstack_push(stack, child);
    }
  }

  return count;
}

static bool mark_white(view_t* view, int* count)
{
  if(view->color != COLOR_GREY)
    return false;

  pony_assert(view->blocked);

  if(view->rc > 0)
  {
    // keep a count of grey nodes colored white minus white nodes colored black
    *count = *count - scan_black(view, 0);
    return false;
  }

  pony_assert(view->perceived == NULL);

  view->color = COLOR_WHITE;
  *count = *count + 1;
  return true;
}

static int scan_white(view_t* view)
{
  // return count of white nodes
  int count = 0;
  viewref_t* ref;
  viewref_t head = {view, 0};
  viewrefstack_t* stack = ponyint_viewrefstack_push(NULL, &head);

  while(stack != NULL)
  {
    stack = ponyint_viewrefstack_pop(stack, &ref);

    if(mark_white(ref->view, &count))
    {
      size_t i = HASHMAP_BEGIN;
      viewref_t* child;

      while((child = ponyint_viewrefmap_next(&ref->view->map, &i)) != NULL)
        stack = ponyint_viewrefstack_push(stack, child);
    }
  }

  return count;
}

static bool collect_view(perceived_t* per, view_t* view, size_t rc, int* count)
{
  if(view->color == COLOR_WHITE)
  {
    pony_assert(view->deferred == false);
    pony_assert(view->perceived == NULL);

    view->perceived = per;

#ifdef USE_RUNTIMESTATS
    detector_t* d = (detector_t*)cycle_detector;
    size_t mem_size_before = ponyint_viewmap_mem_size(&per->map);
    size_t alloc_size_before = ponyint_viewmap_alloc_size(&per->map);
#endif

    ponyint_viewmap_put(&per->map, view);

#ifdef USE_RUNTIMESTATS
    size_t mem_size_after = ponyint_viewmap_mem_size(&per->map);
    size_t alloc_size_after = ponyint_viewmap_alloc_size(&per->map);
    d->mem_used += (mem_size_after - mem_size_before);
    d->mem_allocated += (alloc_size_after - alloc_size_before);
#endif
  }

  return mark_black(view, rc, count);
}

static int collect_white(perceived_t* per, view_t* view, size_t rc)
{
  // return a count of white nodes colored black
  int count = 0;
  viewref_t* ref;
  viewref_t head = {view, rc};
  viewrefstack_t* stack = ponyint_viewrefstack_push(NULL, &head);

  while(stack != NULL)
  {
    stack = ponyint_viewrefstack_pop(stack, &ref);

    if(collect_view(per, ref->view, ref->rc, &count))
    {
      size_t i = HASHMAP_BEGIN;
      viewref_t* child;

      while((child = ponyint_viewrefmap_next(&ref->view->map, &i)) != NULL)
        stack = ponyint_viewrefstack_push(stack, child);
    }
  }

  return count;
}

static void send_conf(pony_ctx_t* ctx, perceived_t* per)
{
  size_t i = HASHMAP_BEGIN;
  view_t* view;

  while((view = ponyint_viewmap_next(&per->map, &i)) != NULL)
  {
    // The actor itself is also allowed to initiate a deletion of itself if
    // it is blocked and has a reference count of 0. Because deletion is
    // not an atomic operation, and the message queue remaining empty is an
    // invariant that can't be violated if the cycle detector is sending
    // a message to an actor, that actor isn't eligible to initiate getting
    // reaped in a short-circuit fashion. Only the actor or the cycle
    // detector can be in the "cycle detector critical" section for the actor.
    if (ponyint_acquire_cycle_detector_critical(view->actor))
    {
      // To avoid a race condition, we have to make sure that the actor isn't
      // pending destroy before sending a message. Failure to do so could
      // eventually result in a double free.
      if(!ponyint_actor_pendingdestroy(view->actor))
        pony_sendi(ctx, view->actor, ACTORMSG_CONF, per->token);

      ponyint_release_cycle_detector_critical(view->actor);
    }
  }
}

static bool detect(pony_ctx_t* ctx, detector_t* d, view_t* view)
{
  pony_assert(view->perceived == NULL);

  scan_grey(d, view, 0);
  int count = scan_white(view);
  pony_assert(count >= 0);

  if(count == 0)
    return false;

  d->detected++;

  perceived_t* per = (perceived_t*)POOL_ALLOC(perceived_t);
  per->token = d->next_token++;
  per->ack = 0;
  ponyint_viewmap_init(&per->map, count);
#ifdef USE_RUNTIMESTATS
  d->mem_used += sizeof(perceived_t);
  d->mem_allocated += POOL_ALLOC_SIZE(perceived_t);
  d->mem_used += ponyint_viewmap_mem_size(&per->map);
  d->mem_allocated += ponyint_viewmap_alloc_size(&per->map);

  size_t mem_size_before = ponyint_perceivedmap_mem_size(&d->perceived);
  size_t alloc_size_before = ponyint_perceivedmap_alloc_size(&d->perceived);
#endif

  ponyint_perceivedmap_put(&d->perceived, per);

#ifdef USE_RUNTIMESTATS
  size_t mem_size_after = ponyint_perceivedmap_mem_size(&d->perceived);
  size_t alloc_size_after = ponyint_perceivedmap_alloc_size(&d->perceived);
  d->mem_used += (mem_size_after - mem_size_before);
  d->mem_allocated += (alloc_size_after - alloc_size_before);
#endif

  int count2 = collect_white(per, view, 0);

  (void)count2;
  pony_assert(count2 == count);
  pony_assert(ponyint_viewmap_size(&per->map) == (size_t)count);

  send_conf(ctx, per);
  return true;
}

static void deferred(pony_ctx_t* ctx, detector_t* d)
{
  d->attempted++;

  size_t i = HASHMAP_BEGIN;
  view_t* view;

  while((view = ponyint_viewmap_next(&d->deferred, &i)) != NULL)
  {
    pony_assert(view->deferred == true);
    ponyint_viewmap_removeindex(&d->deferred, i);

    // always scan again from same index because robin hood hashmap
    // will shift delete items
    i--;

    view->deferred = false;

    detect(ctx, d, view);
  }
}

static void expire(detector_t* d, view_t* view)
{
  perceived_t* per = view->perceived;

  if(per == NULL)
    return;

  size_t i = HASHMAP_BEGIN;
  view_t* pview;

  while((pview = ponyint_viewmap_next(&per->map, &i)) != NULL)
    pview->perceived = NULL;

  ponyint_perceivedmap_remove(&d->perceived, per);
  perceived_free(per);
  view->perceived = NULL;
}

static void collect(pony_ctx_t* ctx, detector_t* d, perceived_t* per)
{
  ponyint_perceivedmap_remove(&d->perceived, per);

  size_t i = HASHMAP_BEGIN;
  view_t* view;

  // mark actors in the cycle as pending destruction
  while((view = ponyint_viewmap_next(&per->map, &i)) != NULL)
  {
    // these actors should not already be marked as pendingdestroy
    // or else we could end up double freeing them
    pony_assert(!ponyint_actor_pendingdestroy(view->actor));

    pony_assert(view->perceived == per);

    // remove from the deferred set
    if(view->deferred)
      ponyint_viewmap_remove(&d->deferred, view);

    // invoke the actor's finalizer
    ponyint_actor_setpendingdestroy(view->actor);
    ponyint_actor_final(ctx, view->actor);
  }

  // actors being collected that have references to actors that are not in
  // the cycle now send ponyint_gc_release messages to those actors
  i = HASHMAP_BEGIN;

  while((view = ponyint_viewmap_next(&per->map, &i)) != NULL)
    ponyint_actor_sendrelease(ctx, view->actor);

  // destroy the actor and free the view on the actor
  i = HASHMAP_BEGIN;

  while((view = ponyint_viewmap_next(&per->map, &i)) != NULL)
  {
    ponyint_actor_destroy(view->actor);
    ponyint_viewmap_remove(&d->views, view);
    view_free(view);
  }

  d->destroyed += ponyint_viewmap_size(&per->map);

  // free the perceived cycle
  perceived_free(per);
  d->collected++;
}

static void check_blocked(pony_ctx_t* ctx, detector_t* d)
{
  size_t i = d->last_checked;
  size_t total = ponyint_viewmap_size(&d->views);
  size_t n = 0;
  view_t* view;

  while((view = ponyint_viewmap_next(&d->views, &i)) != NULL)
  {
    // send message asking if actor is blocked
    // if it is not already blocked
    if(!view->blocked)
    {
      // The actor itself is also allowed to initiate a deletion of itself if
      // it is blocked and has a reference count of 0. Because deletion is
      // not an atomic operation, and the message queue remaining empty is an
      // invariant that can't be violated if the cycle detector is sending
      // a message to an actor, that actor isn't eligible to initiate getting
      // reaped in a short-circuit fashion. Only the actor or the cycle
      // detector can be in the "cycle detector critical" section for the actor.
      if (ponyint_acquire_cycle_detector_critical(view->actor))
      {
        // To avoid a race condition, we have to make sure that the actor isn't
        // pending destroy before sending a message. Failure to do so could
        // eventually result in a double free.
        if(!ponyint_actor_pendingdestroy(view->actor))
          pony_send(ctx, view->actor, ACTORMSG_ISBLOCKED);

        ponyint_release_cycle_detector_critical(view->actor);
      }
    }

    // Stop if we've hit the max limit for # of actors to check
    // (either the CD_MAX_CHECK_BLOCKED constant, or 10% of the total number
    // of actors, whichever limit is larger)
    n++;
    if(n > (total/10 > CD_MAX_CHECK_BLOCKED ? total/10 : CD_MAX_CHECK_BLOCKED))
      break;
  }

  // if we've reached the end of the map, reset and start from
  // the beginning next time else continue from where we left
  // off
  if(view == NULL)
    d->last_checked = HASHMAP_BEGIN;
  else
    d->last_checked = i;

  // process all deferred view stuff
  deferred(ctx, d);
}

static void actor_destroyed(detector_t* d, pony_actor_t* actor)
{
  // this would only called by a manual destroy of an actor
  // if that was possible. It is currently unused.
  // used to clean up the dangling reference to this actor
  // in the cycle detector to avoid a crash

  // get view for actor
  view_t* view = get_view(d, actor, false);

  if(view)
  {
    // remove and free view
    ponyint_viewmap_remove(&d->views, view);
    view_free(view);
  }
}

static void block(detector_t* d, pony_ctx_t* ctx, pony_actor_t* actor,
  size_t rc, deltamap_t* map)
{
  if (rc == 0)
  {
    // if rc == 0 then this is a zombie actor with no references left to it
    // - the actor blocked because it has no messages in its queue
    // - there's no references to this actor because rc == 0
    // therefore the actor is a zombie and can be reaped.

    view_t* view = get_view(d, actor, false);

    if (view != NULL)
    {
      // remove from the deferred set
      if(view->deferred)
        ponyint_viewmap_remove(&d->deferred, view);

      // if we're in a perceived cycle, that cycle is invalid
      expire(d, view);

      // free the view on the actor
      ponyint_viewmap_remove(&d->views, view);
      view_free(view);
    }

    if (map != NULL)
    {
      // free deltamap
      #ifdef USE_RUNTIMESTATS
        d->mem_used -= ponyint_deltamap_total_mem_size(map);
        d->mem_allocated -= ponyint_deltamap_total_alloc_size(map);
      #endif

      ponyint_deltamap_free(map);
    }

    // the actor should already be marked as pending destroy
    pony_assert(ponyint_actor_pendingdestroy(actor));

    // invoke the actor's finalizer and destroy it
    ponyint_actor_final(ctx, actor);
    ponyint_actor_sendrelease(ctx, actor);
    ponyint_actor_destroy(actor);

    d->destroyed++;

    return;
  }


  view_t* view = get_view(d, actor, true);

  // update reference count
  view->rc = rc;

  // apply delta immediately
  if(map != NULL)
    apply_delta(d, view, map);

  // record that we're blocked
  view->blocked = true;

  // if we're in a perceived cycle, that cycle is invalid
  expire(d, view);

  // add to the deferred set
  if(!view->deferred)
  {
#ifdef USE_RUNTIMESTATS
    size_t mem_size_before = ponyint_viewmap_mem_size(&d->deferred);
    size_t alloc_size_before = ponyint_viewmap_alloc_size(&d->deferred);
#endif

    ponyint_viewmap_put(&d->deferred, view);
    view->deferred = true;

#ifdef USE_RUNTIMESTATS
    size_t mem_size_after = ponyint_viewmap_mem_size(&d->deferred);
    size_t alloc_size_after = ponyint_viewmap_alloc_size(&d->deferred);
    d->mem_used += (mem_size_after - mem_size_before);
    d->mem_allocated += (alloc_size_after - alloc_size_before);
#endif
  }
}

static void unblock(detector_t* d, pony_actor_t* actor)
{
  view_t key;
  key.actor = actor;
  size_t index = HASHMAP_UNKNOWN;

  // we must be in the views, because we must have blocked in order to unblock
  view_t* view = ponyint_viewmap_get(&d->views, &key, &index);
  pony_assert(view != NULL);

  // record that we're unblocked
  view->blocked = false;

  // remove from the deferred set
  if(view->deferred)
  {
    ponyint_viewmap_remove(&d->deferred, view);
    view->deferred = false;
  }

  // if we're in a perceived cycle, that cycle is invalid
  expire(d, view);
}

static void ack(pony_ctx_t* ctx, detector_t* d, size_t token)
{
  perceived_t key;
  key.token = token;
  size_t index = HASHMAP_UNKNOWN;

  perceived_t* per = ponyint_perceivedmap_get(&d->perceived, &key, &index);

  if(per == NULL)
    return;

  per->ack++;

  if(per->ack == ponyint_viewmap_size(&per->map))
  {
    collect(ctx, d, per);
    return;
  }
}

// this is called as part of cycle detector termination
// which happens after all scheduler threads have been
// stopped already
static void final(pony_ctx_t* ctx, pony_actor_t* self)
{
  // Find block messages and invoke finalisers for those actors
  pendingdestroystack_t* stack = NULL;
  pony_msg_t* msg;

  do
  {
    while((msg = ponyint_actor_messageq_pop(&self->q
#ifdef USE_DYNAMIC_TRACE
      , ctx->scheduler, self
#endif
      )) != NULL)
    {
      if(msg->id == ACTORMSG_BLOCK)
      {
        block_msg_t* m = (block_msg_t*)msg;

        if(m->delta != NULL)
          ponyint_deltamap_free(m->delta);

        if(!ponyint_actor_pendingdestroy(m->actor))
        {
          ponyint_actor_setpendingdestroy(m->actor);
          ponyint_actor_final(ctx, m->actor);
          stack = ponyint_pendingdestroystack_push(stack, m->actor);
        }
      }
    }
  } while(!ponyint_messageq_markempty(&self->q));

  detector_t* d = (detector_t*)self;
  size_t i = HASHMAP_BEGIN;
  view_t* view;

  // Invoke the actor's finalizer for all actors as long as
  // they haven't already been destroyed.
  while((view = ponyint_viewmap_next(&d->views, &i)) != NULL)
  {
    if(!ponyint_actor_pendingdestroy(view->actor))
    {
      ponyint_actor_setpendingdestroy(view->actor);
      ponyint_actor_final(ctx, view->actor);
      stack = ponyint_pendingdestroystack_push(stack, view->actor);
    }
  }

  pony_actor_t* actor;

  while(stack != NULL)
  {
    stack = ponyint_pendingdestroystack_pop(stack, &actor);
    ponyint_actor_destroy(actor);
  }

  i = HASHMAP_BEGIN;
  while((view = ponyint_viewmap_next(&d->deferred, &i)) != NULL)
  {
    ponyint_viewmap_removeindex(&d->deferred, i);
    // always scan again from same index because robin hood hashmap
    // will shift delete items
    i--;
  }

#ifdef USE_RUNTIMESTATS
  d->mem_used -= ponyint_viewmap_mem_size(&d->deferred);
  d->mem_allocated -= ponyint_viewmap_alloc_size(&d->deferred);
  d->mem_used -= ponyint_viewmap_mem_size(&d->views);
  d->mem_allocated -= ponyint_viewmap_alloc_size(&d->views);
  d->mem_used -= ponyint_perceivedmap_mem_size(&d->perceived);
  d->mem_allocated -= ponyint_perceivedmap_alloc_size(&d->perceived);
  // pony_assert(d->mem_used == 0);
  // pony_assert(d->mem_allocated == 0);
#endif
  ponyint_viewmap_destroy(&d->deferred);
  ponyint_viewmap_destroy(&d->views);
  ponyint_perceivedmap_destroy(&d->perceived);
}

#ifndef PONY_NDEBUG

static void dump_view(view_t* view)
{
  printf("%p: " __zu "/" __zu " (%s)%s%s\n",
    view->actor, view->rc, view->actor->gc.rc,
    view->blocked ? "blocked" : "unblocked",
    view->rc == view->actor->gc.rc ? "" : " ERROR",
    view->actor->gc.delta == NULL ? "" : " DELTA");

  size_t i = HASHMAP_BEGIN;
  viewref_t* p;
  actorref_t* aref;
  size_t index = HASHMAP_UNKNOWN;

  while((p = ponyint_viewrefmap_next(&view->map, &i)) != NULL)
  {
    aref = ponyint_actormap_getactor(&view->actor->gc.foreign, p->view->actor, &index);

    if(aref != NULL)
    {
      printf("\t%p: " __zu "/" __zu " %s\n",
        p->view->actor, p->rc, aref->rc,
        p->rc == aref->rc ? "" : "ERROR");
    } else {
      printf("\t%p: " __zu " ERROR\n", p->view->actor, p->rc);
    }
  }

  if(ponyint_actormap_size(&view->actor->gc.foreign) != \
    ponyint_viewrefmap_size(&view->map))
  {
    printf("\t--- ERROR\n");

    i = HASHMAP_BEGIN;

    while((aref = ponyint_actormap_next(&view->actor->gc.foreign, &i)) != NULL)
    {
      printf("\t%p: " __zu "\n", aref->actor, aref->rc);
    }
  }
}

static void dump_views()
{
  detector_t* d = (detector_t*)cycle_detector;
  size_t i = HASHMAP_BEGIN;
  view_t* view;

  while((view = ponyint_viewmap_next(&d->views, &i)) != NULL)
  {
    dump_view(view);
  }
}

static void check_view(detector_t* d, view_t* view)
{
  if(view->perceived != NULL)
  {
    printf("%p: in a cycle\n", view->actor);
    return;
  }

  scan_grey(d, view, 0);
  int count = scan_white(view);
  pony_assert(count >= 0);

  printf("%p: %s\n", view->actor, count > 0 ? "COLLECTABLE" : "uncollectable");
}

static void check_views()
{
  detector_t* d = (detector_t*)cycle_detector;
  size_t i = HASHMAP_BEGIN;
  view_t* view;

  while((view = ponyint_viewmap_next(&d->views, &i)) != NULL)
  {
    check_view(d, view);
  }
}

#endif

static void cycle_dispatch(pony_ctx_t* ctx, pony_actor_t* self,
  pony_msg_t* msg)
{
  detector_t* d = (detector_t*)self;

  switch(msg->id)
  {
    case ACTORMSG_CHECKBLOCKED:
    {
      // check for blocked actors/cycles
      check_blocked(ctx, d);
      break;
    }

    case ACTORMSG_DESTROYED:
    {
      pony_msgp_t* m = (pony_msgp_t*)msg;
      actor_destroyed(d, (pony_actor_t*)m->p);
      break;
    }

    case ACTORMSG_BLOCK:
    {
#ifdef USE_RUNTIMESTATS_MESSAGES
      ctx->schedulerstats.mem_used_inflight_messages -= sizeof(block_msg_t);
      ctx->schedulerstats.mem_allocated_inflight_messages -= POOL_ALLOC_SIZE(block_msg_t);
#endif

      block_msg_t* m = (block_msg_t*)msg;
      block(d, ctx, m->actor, m->rc, m->delta);
      break;
    }

    case ACTORMSG_UNBLOCK:
    {
      pony_msgp_t* m = (pony_msgp_t*)msg;
      unblock(d, (pony_actor_t*)m->p);
      break;
    }

    case ACTORMSG_ACK:
    {
      pony_msgi_t* m = (pony_msgi_t*)msg;
      ack(ctx, d, m->i);
      break;
    }

#ifndef PONY_NDEBUG
    default:
    {
      // Never happens, used to keep debug functions.
      dump_views();
      check_views();
    }
#endif
  }
}

static pony_type_t cycle_type =
{
  0,
  sizeof(detector_t),
  0,
  0,
  0,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  cycle_dispatch,
  NULL,
  0,
  0,
  NULL,
  NULL,
  NULL
};

void ponyint_cycle_create(pony_ctx_t* ctx, uint32_t detect_interval)
{
  // max is 1 second (1000 ms)
  if(detect_interval > 1000)
    detect_interval = 1000;

  // min is 10 ms
  if(detect_interval < 10)
    detect_interval = 10;

  cycle_detector = NULL;
  cycle_detector = pony_create(ctx, &cycle_type, false);
  ponyint_actor_setsystem(cycle_detector);

  detector_t* d = (detector_t*)cycle_detector;

  // convert to cycles for use with ponyint_cpu_tick()
  // 1 second = 2000000000 cycles (approx.)
  // based on same scale as ponyint_cpu_core_pause() uses
  d->detect_interval = detect_interval * 2000000;

  // initialize last_checked
  d->last_checked = HASHMAP_BEGIN;
}

bool ponyint_cycle_check_blocked(uint64_t tsc, uint64_t tsc2)
{
  detector_t* d = (detector_t*)cycle_detector;

  // if enough time has passed, trigger cycle detector
  uint64_t diff = ponyint_cpu_tick_diff(tsc, tsc2);
  if(diff > d->detect_interval)
  {
    ponyint_send_inject(cycle_detector, ACTORMSG_CHECKBLOCKED);
    return true;
  }

  return false;
}

void ponyint_cycle_actor_destroyed(pony_actor_t* actor)
{
  // this will only be false during the creation of the cycle detector
  // and after the runtime has been shut down or if the cycle detector
  // is being destroyed
  if(cycle_detector && !ponyint_is_cycle(actor)) {
    ponyint_sendp_inject(cycle_detector, ACTORMSG_DESTROYED, actor);
  }
}

void ponyint_cycle_block(pony_actor_t* actor, gc_t* gc)
{
  pony_assert(&actor->gc == gc);

  block_msg_t* m = (block_msg_t*)pony_alloc_msg(
    POOL_INDEX(sizeof(block_msg_t)), ACTORMSG_BLOCK);

  m->actor = actor;
  m->rc = ponyint_gc_rc(gc);
  m->delta = ponyint_gc_delta(gc);
  pony_assert(gc->delta == NULL);

  ponyint_sendv_inject(cycle_detector, &m->msg);
}

void ponyint_cycle_unblock(pony_actor_t* actor)
{
  ponyint_sendp_inject(cycle_detector, ACTORMSG_UNBLOCK, actor);
}

void ponyint_cycle_ack(size_t token)
{
  ponyint_sendi_inject(cycle_detector, ACTORMSG_ACK, token);
}

void ponyint_cycle_terminate(pony_ctx_t* ctx)
{
  ponyint_become(ctx, cycle_detector);
  final(ctx, cycle_detector);
  ponyint_destroy(ctx, cycle_detector);
  cycle_detector = NULL;
}

bool ponyint_is_cycle(pony_actor_t* actor)
{
  return actor == cycle_detector;
}

#ifdef USE_RUNTIMESTATS
size_t ponyint_cycle_mem_size()
{
  detector_t* d = (detector_t*)cycle_detector;
  return d->mem_used;
}

size_t ponyint_cycle_alloc_size()
{
  detector_t* d = (detector_t*)cycle_detector;
  return d->mem_allocated;
}
#endif
