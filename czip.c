#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

const int INT_OFFSET = 4;

#ifndef DEBUG_INFO
const unsigned int __DEBUG_INFO = 0;
#define DEBUG_INFO
#endif

#define eprintf(priority, ...)                                                 \
  if (priority <= __DEBUG_INFO)                                                \
  fprintf(stderr, __VA_ARGS__)

const int CHUNK_SIZE = 9000; // NOTE: be carful with this buffer size
const int THREAD_BUFF_LENGTH = CHUNK_SIZE * 10;

const char *NTHREADS =
    "NTHREADS"; // The name of the variable for setting threads.

typedef struct {
  volatile int write_num;
  pthread_mutex_t
      guard; // A semiphore would have been better, but not supported on mac.
} WriteHead;

WriteHead WRITE_HEAD;

/*
 * Setup write-head.
 */
void write_head_init() {
  WRITE_HEAD.write_num = 1;
  pthread_mutex_init(&WRITE_HEAD.guard, NULL);
}

/*
 * Locks the write-head, returning a pointer to the write position.
 * This pointer should be updated on write.
 */
void write_head_lock() {
  pthread_mutex_lock(&WRITE_HEAD.guard);
  eprintf(3, "WRITE_HEAD locked\n");
}

void write_head_unlock() {
  pthread_mutex_unlock(&WRITE_HEAD.guard);
  eprintf(3, "WRITE_HEAD unlocked\n");
}

int write_head_num() { return WRITE_HEAD.write_num; }

/*
 * Writes the buff to head.
 * Assumes that the write-head is already locked.
 */
void write_head_write_buff(unsigned char *buff, int length) {
  if (length > 0)
    fwrite(buff, length, 1, stdout);
  WRITE_HEAD.write_num++;
  eprintf(3, "Written %d chars.\n", length);
}

typedef struct {
  char last_char;
  int count;
  int buff_index;
  unsigned char buff[THREAD_BUFF_LENGTH];
} CompressInfo;

void compress_info_init(CompressInfo *info) {
  info->last_char = EOF;
  info->count = 0;
  info->buff_index = 0;
}

typedef struct {
  int tasknum;          // The sequential ordering of writes, starting from 1.
  char *read_begin;     // The begininning of the sequence to encode.
  char *read_end;       // The end of the sequence to decode.
  CompressInfo info;    // Information about the decoding taking place.
  int exit_immediately; // Whither to exit after finishing one cycle.
  int write_end; // Whither to write the end, or wait for new input to encode.
  int return_tasknum; // The tasknum to next write to.
  pthread_t thread;
} TaskDescriptor;

void task_descriptor_init(TaskDescriptor *task) {
  task->tasknum = 0;
  task->read_end = NULL;
  task->read_begin = NULL;
  task->exit_immediately = 0;
  task->write_end = 0;
  task->return_tasknum = -1;
  compress_info_init(&task->info);
}
void eprint_write_buff(unsigned char *buf, int length) {
  eprintf(3, "buf: '");
  for (int len = 0; len < length; len += sizeof(int) + 1) {
    int n = (*(buf + len) << 0) + (*(buf + len + 1) << 8) +
            (*(buf + len + 2) << 16) + (*(buf + len + 3) << 24);
    eprintf(3, "%d%c", n, *(buf + len + sizeof(int)));
  }
  eprintf(3, "'\n");
}

void write_internal_buff(unsigned char *buff, int *index, char c, int count) {
  if (count > 0) {
    if (*index > 1 && buff[*index - 1] == c) {
      count = count + (buff[*index - 2] << 24 & 0xFF) +
              (buff[*index - 3] << 16 & 0xFF) + (buff[*index - 4] << 8 & 0xFF) +
              (buff[*index - 5] << 0 & 0xFF);
      *index -= 5;
    }
    unsigned char *ind = buff + *index;
    *index += sizeof(int) + 1;
    assert(*index >= 0 && *index < THREAD_BUFF_LENGTH);
    ind[3] = (count >> 24) & 0xFF;
    ind[2] = (count >> 16) & 0xFF;
    ind[1] = (count >> 8) & 0xFF;
    ind[0] = count & 0xFF;
    ind[4] = c;
    eprintf(6, "Writing internal '%d%c'\n", count, c == '\n' ? '@' : c);
  }
}

void sync_write_from_internal_buff(TaskDescriptor *desc) {
  int do_loop = 1;
  while (do_loop) {
    write_head_lock(); // TODO: streamline concurency
    assert(desc->tasknum >= write_head_num());
    eprintf(3, "Attempting do-loop of task %d on write_head %d\n",
            desc->tasknum, write_head_num());
    if (desc->tasknum == write_head_num()) {
      if (!desc->write_end) {
        write_head_write_buff(desc->info.buff, desc->info.buff_index);
        desc->info.buff_index = 0;
      } else {
        desc->return_tasknum = desc->tasknum;
      }
      do_loop = 0;
      desc->tasknum = 0;
    } else
      eprintf(5, "no action loop: tasknum %d on headnum %d for thread %lu\n",
              desc->tasknum, write_head_num(), (long)desc->thread);
    write_head_unlock();
    eprintf(3, "spin baby spin\n");
  }
}
// reads the range described by desc into buff, returning the length read of the
// buff read.
void read_to_internal_buff(TaskDescriptor *desc) {
  char c = EOF;
  while (desc->read_end > desc->read_begin) {
    c = *((desc->read_begin)++);
    if (c == desc->info.last_char) {
      (desc->info.count)++;
    } else {
      write_internal_buff(desc->info.buff, &desc->info.buff_index,
                          desc->info.last_char, desc->info.count);
      desc->info.last_char = c;
      desc->info.count = 1;
    }
  }
  write_internal_buff(desc->info.buff, &desc->info.buff_index,
                      desc->info.last_char, desc->info.count);
  desc->info.last_char = EOF;
  desc->info.count = 0;
}

void write_section(TaskDescriptor *desc) {
  int had_exit_immediatly = 0;
  do {
    assert(had_exit_immediatly == 0);
    assert(desc->exit_immediately == 0 || desc->exit_immediately == 1);
    if (desc->tasknum) { // TODO: put in non spin-lock here
      eprintf(2, "executing task_num=%d with write_end=%d on thread %lu\n",
              desc->tasknum, desc->write_end, (long)desc->thread);
      read_to_internal_buff(desc);

      eprintf(2, "finished read task_num=%d with write_end=%d on thread %lu\n",
              desc->tasknum, desc->write_end, (long)desc->thread);
      sync_write_from_internal_buff(desc);
      eprintf(2,
              "finished write section loop:\n\ttask_num=%d with write_end=%d "
              "on thread %lu\n",
              desc->tasknum, desc->write_end, (long)desc->thread);
      eprintf(2, "note: desc->exit_immediately=%d\n", desc->exit_immediately);
    }
    eprintf(5, "spin the write section\n");
    had_exit_immediatly |= desc->exit_immediately;
  } while (!desc->exit_immediately);
  assert(desc->exit_immediately);
}

// mmap a file into memeory,
// setting file to the file,
// setting length to the file length
// returning 0 if successful
int mmap_file(char *file_name, char **file, size_t *length) {
  struct stat sb;
  int fd = open(file_name, O_RDONLY);
  if (fd == -1)
    return 1;
  int fstat_code = fstat(fd, &sb);
  if (fstat_code == -1)
    return 2;
  *file = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (*file == MAP_FAILED)
    return 3;
  *length = sb.st_size;
  return 0;
}

#include <assert.h>

// Function to set the next task
int set_next_task(int *task_num, TaskDescriptor *task, char **head, char *end,
                  int use_pending_info, CompressInfo *pending_info) {
  
  // Check if head has reached or exceeded the end
  if (*head >= end) {
    return 0;  // No more tasks
  }

  // Ensure the exit_immediately flag is either 0 or 1
  assert(task->exit_immediately == 0 || task->exit_immediately == 1);

  // Set the read_begin position for the task
  task->read_begin = *head;
  
  // Calculate the seek position with a CHUNK_SIZE offset
  char *seek = *head + CHUNK_SIZE;

  // Don't seek past the 'end' boundary
  if (end < seek) {
    seek = end;
  }

  // Avoid breaking a section into different tasks by checking consecutive characters
  while (seek + 1 < end && *seek == *(seek + 1)) {
    seek++;
  }

  // Move seek one position further if it is less than 'end'
  if (seek < end) {
    seek++;
  }

  // Update the head and read_end positions for the task
  *head = (task->read_end = seek);

  // Determine if the task has reached the end
  int add_task_num = !task->write_end; // from the previous run
  task->write_end = task->read_end == end;
  int out = task->read_end == end;

  // Increment the task number
  int tasknum = (*task_num += add_task_num);

  // If pending info is provided, update the task's info and print a message
  if (use_pending_info) {
    task->info = *pending_info;
    eprintf(3, "Info switch made to buff with length %d\n",
            pending_info->buff_index);
  }

  // For Debugging
  eprintf(3, "Task assigned to thread %d=%lu with", tasknum, (long)task->thread);
  eprintf(3, " head set to offset %ld\n", (long)(end - *head));

  // Update the task number and return whether the task has reached the end
  task->tasknum = tasknum;
  return out;
}

#include <pthread.h>

// Function to process files using multiple threads
int process_files(char **fnames, int flength, TaskDescriptor *tasks,
                  int ntasks) {

  int last_task = 0; // Index of the last task processed
  CompressInfo pending_info;

  // Validate the exit_immediately flag for each task
  for (int i = 0; i < ntasks; i++)
    assert(tasks[i].exit_immediately == 0 || tasks[i].exit_immediately == 1);

  // PROCESS FILES
  int task_num = 0;
  // Iterate through input files
  for (int file_i = 1; file_i < flength; file_i++) {
    char *file, *findex, *end;
    size_t length;

    // Memory map the file
    if (mmap_file(fnames[file_i], &file, &length))
      return (1);

    findex = file;
    end = file + length;
    int exit = 0;

    // Process file until the end
    while (end > findex && !exit) {
      // Iterate through tasks to assign work
      for (int task_i = 0; task_i < ntasks; task_i++) {
        assert(tasks[task_i].exit_immediately == 0 ||
               tasks[task_i].exit_immediately == 1);

        // Assign the next task
        if (tasks[task_i].tasknum == 0) {
          exit = set_next_task(&task_num, tasks + task_i, &findex, end,
                               last_task == -1, &pending_info);
          if (exit) {
            last_task = task_i;
          }
        }
      }
      write_section(tasks + 0); // Process the assigned task section
    }

    // Unmap the file after processing
    munmap(file, length);

    // Wait for the last task to complete
    while (tasks[last_task].tasknum) {
      write_section(tasks + 0); // NOTE: Spin lock
    }
    // Save the info of the last completed task
    pending_info = tasks[last_task].info;
    last_task = -1;
  }

  // Finish when done
  for (int i = 0; i < ntasks; i++) {
    tasks[i].tasknum = tasks[i].write_end ? tasks[i].return_tasknum : 0;
    tasks[i].write_end = 0;
    tasks[i].exit_immediately = 1;
  }

  // Join threads
  eprintf(2, "Joining threads\n");
  for (int i = 1; i < ntasks; i++) {
    write_section(tasks + 0); // Process the assigned task section


    // Display information about other threads
    for (int k = 0; k < ntasks; k++) {
      if (k != i)
        eprintf(2,
                "\n"
                "\texamining thread: %lu\n"
                "\t\ttasknum=%d, write_end=%d\n"
                "\t\theadnum=%d, exit_immediately=%d\n"
                "\t\tinfo.count=%d, into.buff_index=%d\n",
                (long)tasks[k].thread, tasks[k].tasknum, tasks[k].write_end,
                write_head_num(), tasks[k].exit_immediately,
                tasks[k].info.count, tasks[k].info.buff_index);
    }

    // Join the current thread
    pthread_join(tasks[i].thread, NULL);
    eprintf(2, "thread %lu joined\n", (long)tasks[i].thread);
  }

  // Process the assigned task section
  write_section(tasks + 0);
  return 0;
}


TaskDescriptor *setup_tasks(int ntasks) {

  // INITIALIZE TASKS
  TaskDescriptor *tasks =
      (TaskDescriptor *)malloc(sizeof(TaskDescriptor) * ntasks + 1);
  assert(tasks != NULL);
  for (int i = 0; i < ntasks; i++) {
    task_descriptor_init(tasks + i);
    assert(tasks[i].exit_immediately == 0);
  }
  tasks[0].exit_immediately = 1;

  // INITIALIZE THREADS
  pthread_attr_t pattr;
  pthread_attr_init(&pattr);
  tasks[0].thread = 0;
  for (int i = 1; i < ntasks; i++) {
    if (pthread_create(&(tasks + i)->thread, &pattr, (void *)write_section,
                       (tasks) + i))
      exit(1);
    assert(tasks[i].exit_immediately == 0 || tasks[i].exit_immediately == 1);
  }
  return tasks;
}

int main(int argc, char **argv) {
    clock_t start, end;  // Variables for measuring CPU time
    double cpu_time_used;  // Variable for storing the elapsed CPU time
  if (argc == 1) {
    printf("pzip: file1 [file2 ...]\n");
    return 1;
  } else {
    // Note: the main thread handles task[0]
    // SETUP HEAD
      // Record the start time
      start = clock();
    write_head_init();

    // GET THREAD COUNT
    char *given_thread_count = getenv(NTHREADS);
    int concurrent_task_num = given_thread_count ? atoi(given_thread_count) : 1;
    if (concurrent_task_num < 1) {
      printf("%s must be a positive integer\n", NTHREADS);
      exit(1);
    }

    TaskDescriptor *tasks = setup_tasks(concurrent_task_num);
    for (int i = 0; i < concurrent_task_num; i++)
      assert(tasks[i].exit_immediately == 0 || tasks[i].exit_immediately == 1);
    // Joins threads, to tasks can now be freed.
    int out = process_files(argv, argc, tasks, concurrent_task_num);

    // cleanup
    free(tasks);
      // Record the end time
      end = clock();

      // Calculate the elapsed CPU time in seconds
      cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;

      // Print the elapsed CPU time
      printf("Elapsed CPU time: %f seconds\n", cpu_time_used);
    return out;
  }
}
