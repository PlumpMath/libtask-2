#include <stdio.h>
#include <assert.h>

#include "libtask/task_pool.h"

#define CHECK(x) do { if (!(x)) { assert(0); } } while (0)

// A thread local variable that keeps track of currently executing task.
static libtask_task_t __thread *libtask_current_task = NULL;

// Global list of all threads.
static pthread_spinlock_t all_task_lock;
static pthread_once_t all_task_list_once = PTHREAD_ONCE_INIT;
static libtask_list_t all_task_list;

// Forward declarations.
static void *libtask_task_main(libtask_task_t *task);

static void
initialize_all_task_list()
{
  pthread_spin_init(&all_task_lock, PTHREAD_PROCESS_PRIVATE);
  libtask_list_initialize(&all_task_list);
}

void
libtask_print_all_tasks(void)
{
  CHECK(pthread_spin_lock(&all_task_lock) == 0);

  // TODO: Print each task and the function call-trace in that.
  // libtask_list_apply(&all_task_list, NULL);

  CHECK(pthread_spin_unlock(&all_task_lock) == 0);
}

libtask_task_t *
libtask_get_task_current(void)
{
  return libtask_current_task;
}

error_t
libtask_task_initialize(libtask_task_t *task,
			int (*function)(void *),
			void *argument,
			int32_t stack_size)
{
  CHECK(pthread_once(&all_task_list_once, initialize_all_task_list) == 0);

  char *stack = (char *)malloc(stack_size);
  if (!stack) {
    return ENOMEM;
  }

  task->stack = stack;
  task->nbytes = stack_size;
  CHECK(pthread_mutex_init(&task->mutex, NULL) == 0);

  task->complete = false;
  task->task_pool = NULL;
  libtask_list_initialize(&task->waiting_link);

  // Initialize the task with user function.
  task->result = 0;
  task->argument = argument;
  task->function = function;

  CHECK(getcontext(&task->uct_self) == 0);
  task->uct_self.uc_stack.ss_sp = task->stack;
  task->uct_self.uc_stack.ss_size = task->nbytes;
  task->uct_self.uc_link = NULL;

  makecontext(&task->uct_self, (void(*)())libtask_task_main, 1, task);

  libtask_list_initialize(&task->all_task_link);
  CHECK(pthread_spin_lock(&all_task_lock) == 0);
  libtask_list_push_back(&all_task_list, &task->all_task_link);
  CHECK(pthread_spin_unlock(&all_task_lock) == 0);
  libtask_refcount_initialize(&task->refcount);
  return 0;
}

error_t
libtask_task_finalize(libtask_task_t *task)
{
  CHECK(libtask_refcount_count(&task->refcount) <= 1);
  CHECK(task->task_pool == NULL);
  CHECK(libtask_list_empty(&task->waiting_link));

  if (libtask_current_task == task) {
    return EINVAL;
  }

  // Remove the task from all task list.
  CHECK(pthread_spin_lock(&all_task_lock) == 0);
  libtask_list_erase(&task->all_task_link);
  CHECK(pthread_spin_unlock(&all_task_lock) == 0);

  CHECK(pthread_mutex_destroy(&task->mutex) == 0);
  free(task->stack);
  task->stack = NULL;
  return 0;
}

error_t
libtask_task_create(libtask_task_t **taskp,
		    int (*function)(void *),
		    void *argument,
		    int32_t stack_size)
{
  libtask_task_t *task = (libtask_task_t *)malloc(sizeof(libtask_task_t));
  if (!task) {
    return ENOMEM;
  }

  error_t error = libtask_task_initialize(task, function, argument,
					  stack_size);
  if (error != 0) {
    free(task);
    return error;
  }

  libtask_refcount_create(&task->refcount);
  *taskp = task;
  return 0;
}

error_t
libtask_task_suspend()
{
  libtask_task_t *task = libtask_current_task;
  if (!task) {
    return EINVAL;
  }
  return swapcontext(&task->uct_self, &task->uct_thread) == -1 ? errno : 0;
}

static void *
libtask_task_main(libtask_task_t *task)
{
  assert(libtask_current_task == task);
  task->complete = false;
  task->result = task->function(task->argument);
  task->complete = true;
  assert(libtask_current_task == task);

  if (task->task_pool) {
    libtask_task_pool_erase(task->task_pool, task);
  }

  assert(task->task_pool == NULL);
  assert(libtask_list_empty(&task->waiting_link));

  CHECK(libtask_task_suspend() == 0);

  // No task should ever reach here!
  CHECK(0);
  return NULL;
}

error_t
libtask_task_execute(libtask_task_t *task)
{
  if (task->complete) {
    return EINVAL;
  }

  // Take a reference to the task so that it cannot be destroyed when
  // it is running.
  libtask_task_ref(task);

  // Lock the task's stack.
  CHECK(pthread_mutex_lock(&task->mutex) == 0);

  libtask_current_task = task;
  CHECK(swapcontext(&task->uct_thread, &task->uct_self) == 0);
  libtask_current_task = NULL;

  CHECK(pthread_mutex_unlock(&task->mutex) == 0);
  libtask_task_unref(task);
  return 0;
}

error_t
libtask_task_schedule(libtask_task_t *task)
{
  if (task->complete) {
    return EINVAL;
  }
  if (task->task_pool) {
    return libtask_task_pool_push_back(task->task_pool, task);
  }
  return 0;
}

error_t
libtask_task_yield()
{
  libtask_task_t *task = libtask_current_task;
  if (task == NULL) {
    return pthread_yield();
  }

  if (task->complete) {
    return EINVAL;
  }

  if (task->task_pool == NULL) {
    libtask_task_suspend();
    return 0;
  }

  CHECK(libtask_task_schedule(task) == 0);
  libtask_task_suspend();
  return 0;
}