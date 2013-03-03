#include <assert.h>
#include <stdio.h>

#include "task_pool.h"

#define CHECK(x) do { if (!(x)) { assert(0); } } while (0)

error_t
libtask_task_pool_initialize(libtask_task_pool_t *pool)
{
  pool->ntasks = 0;
  libtask_refcount_initialize(&pool->refcount);
  libtask_list_initialize(&pool->waiting_list);
  CHECK(pthread_spin_init(&pool->spinlock, PTHREAD_PROCESS_PRIVATE) == 0);
  return 0;
}

// Callback function used to reset tasks in a pool.
static void libtask_task_pool_reset_task(int index, libtask_list_t *link)
{
  libtask_task_t *task = libtask_list_entry(link, libtask_task_t,
					    waiting_link);
  task->task_pool = NULL;
  libtask_list_initialize(&task->waiting_link);
}

error_t
libtask_task_pool_finalize(libtask_task_pool_t *pool)
{
  CHECK(libtask_refcount_count(&pool->refcount) <= 1);

  libtask_list_apply(&pool->waiting_list, libtask_task_pool_reset_task);
  CHECK(pthread_spin_destroy(&pool->spinlock) == 0);
  return 0;
}

error_t
libtask_task_pool_create(libtask_task_pool_t **new_poolp)
{
  libtask_task_pool_t *pool =
    (libtask_task_pool_t *) calloc(sizeof(libtask_task_pool_t), 1);
  if (!pool) {
    return ENOMEM;
  }

  error_t error = libtask_task_pool_initialize(pool);
  if (error) {
    free(pool);
    return error;
  }

  libtask_refcount_create(&pool->refcount);
  *new_poolp = pool;
  return 0;
}

int32_t
libtask_get_task_pool_size(libtask_task_pool_t *pool)
{
  CHECK(pthread_spin_lock(&pool->spinlock) == 0);
  int32_t size = pool->ntasks;
  CHECK(pthread_spin_unlock(&pool->spinlock) == 0);
  return size;
}

error_t
libtask_task_pool_push_back(libtask_task_pool_t *pool, libtask_task_t *task)
{
  if (task->task_pool != pool) {
    return EINVAL;
  }

  CHECK(pthread_spin_lock(&pool->spinlock) == 0);
  libtask_list_erase(&task->waiting_link);
  libtask_list_push_back(&pool->waiting_list, &task->waiting_link);
  CHECK(pthread_spin_unlock(&pool->spinlock) == 0);
  return 0;
}

error_t
libtask_task_pool_pop_front(libtask_task_pool_t *pool, libtask_task_t **taskp)
{
  CHECK(pthread_spin_lock(&pool->spinlock) == 0);
  libtask_list_t *link = libtask_list_pop_front(&pool->waiting_list);
  CHECK(pthread_spin_unlock(&pool->spinlock) == 0);

  if (!link) {
    return ENOENT;
  }

  *taskp = libtask_list_entry(link, libtask_task_t, waiting_link);
  return 0;
}

error_t
libtask_task_pool_insert(libtask_task_pool_t *pool, libtask_task_t *task)
{
  CHECK(task->task_pool == NULL);
  CHECK(pthread_spin_lock(&pool->spinlock) == 0);

  pool->ntasks++;
  task->task_pool = libtask_task_pool_ref(pool);
  libtask_list_push_back(&pool->waiting_list, &task->waiting_link);

  CHECK(pthread_spin_unlock(&pool->spinlock) == 0);
  return 0;
}

error_t
libtask_task_pool_erase(libtask_task_pool_t *pool, libtask_task_t *task)
{
  if (task->task_pool != pool) {
    return EINVAL;
  }

  CHECK(pthread_spin_lock(&pool->spinlock) == 0);

  pool->ntasks--;
  CHECK(pool->ntasks >= 0);

  task->task_pool = NULL;
  if (!libtask_list_empty(&task->waiting_link)) {
    libtask_list_erase(&task->waiting_link);
  }
  if (pool->ntasks == 0) {
    CHECK(libtask_list_empty(&task->waiting_link));
  }

  CHECK(pthread_spin_unlock(&pool->spinlock) == 0);
  libtask_task_pool_unref(pool);
  return 0;
}

error_t
libtask_task_pool_switch(libtask_task_pool_t *pool,
			 libtask_task_pool_t **oldp)
{
  libtask_task_t *task = libtask_get_task_current();
  if (task == NULL || task->task_pool == pool) {
    return EINVAL;
  }

  if (oldp) {
    *oldp = task->task_pool ? libtask_task_pool_ref(task->task_pool) : NULL;
  }

  if (task->task_pool) {
    CHECK(libtask_task_pool_erase(task->task_pool, task) == 0);
  }

  CHECK(libtask_task_pool_insert(pool, task) == 0);
  return libtask_task_yield();
}
