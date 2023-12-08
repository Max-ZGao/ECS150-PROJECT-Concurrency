#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#include <time.h>

typedef struct {
    int count;
    char buff;
} __attribute__((packed)) CharCount;

typedef struct {
    int start;
    int term;
} ThreadArgs;

typedef struct {
    CharCount *data;
    int DataSize;
} ThreadResult;

char *FileContents;

//Open and map the input file
char *openAndMapFile(const char *FileName, int *fileSize) {
    int fd = open(FileName, O_RDONLY);
    if (fd == -1) {
        perror("Error opening file");
        return NULL;
    }

    struct stat fileStat;
    if (fstat(fd, &fileStat) == -1) {
        perror("Error getting file size");
        close(fd);
        return NULL;
    }

    *fileSize = fileStat.st_size;

    char *fileContents = (char *)mmap(0, *fileSize, PROT_READ, MAP_PRIVATE, fd, 0);

    if (fileContents == MAP_FAILED) {
        perror("Error mapping file");
        close(fd);
        return NULL;
    }

    close(fd);
    return fileContents;
}

ThreadResult *compressThreadPortion(ThreadArgs *args) {
    ThreadResult *result = malloc(sizeof(ThreadResult));
    int DataSize = 0;
    int counter = 0;
    int start = args->start;
    int end = args->term;
    char curr;

    result->data = malloc(5 * (end - start + 1));

    while (FileContents[start] == '\0')
        start++;

    curr = FileContents[start];

    while (start <= end) {
        if (FileContents[start] == '\0') {
            start++;
        } else if (FileContents[start] == curr) {
            start++;
            counter++;
        } else {
            CharCount temp = {counter, curr};
            result->data[DataSize++] = temp;

            counter = 0;
            curr = FileContents[start];
        }
    }

    CharCount temp = {counter, curr};
    result->data[DataSize++] = temp;
    result->DataSize = DataSize;

    return result;
}

void *compressionThread(void *arg) {
    ThreadArgs *args = (ThreadArgs *)arg;
    return (void *)compressThreadPortion(args);
}

void aggregateResults(ThreadResult *result, int *CurrentCount, int *CurrentMax, CharCount **StoredCounts) {
    int start = 0;
    if (*StoredCounts != NULL && result->data[0].buff == (*StoredCounts)[(*CurrentCount) - 1].buff) {
        (*StoredCounts)[(*CurrentCount) - 1].count += result->data[0].count;
        start = 1;
    }

    while (start < result->DataSize) {
        (*StoredCounts)[(*CurrentCount)++] = result->data[start++];
        if (*CurrentCount == *CurrentMax) {
            *StoredCounts = realloc(*StoredCounts, *CurrentMax * 2 * 5);
            *CurrentMax *= 2;
        }
    }

    free(result->data);
    free(result);
}

void Compression(const char *FileName, int NThreads, int *CurrentCount, int *CurrentMax, CharCount **StoredCounts) {
    int fileSize;
    FileContents = openAndMapFile(FileName, &fileSize);

    if (FileContents == NULL) {
        fprintf(stderr, "Compression failed for file: %s\n", FileName);
        return;
    }

    int portion = fileSize / NThreads;
    pthread_t *threads = malloc(NThreads * sizeof(pthread_t));
    ThreadArgs **args = malloc(NThreads * sizeof(ThreadArgs *));
    ThreadResult **results = malloc(NThreads * sizeof(ThreadResult *));

    for (int j = 0; j < NThreads; j++) {
        args[j] = malloc(sizeof(ThreadArgs));
        args[j]->start = j * portion;
        args[j]->term = (j == NThreads - 1) ? (fileSize - 1) : ((j + 1) * portion - 1);
        pthread_create(&threads[j], NULL, compressionThread, args[j]);
    }

    for (int j = 0; j < NThreads; j++) {
        pthread_join(threads[j], (void **)&results[j]);
        aggregateResults(results[j], CurrentCount, CurrentMax, StoredCounts);
        free(args[j]);
    }

    free(threads);
    free(args);
    free(results);


    // Clean up and unmap the file
    munmap(FileContents, fileSize);
}

int main(int argc, char *argv[]) {
//    clock_t start, end;  // Variables for measuring CPU time
//    double cpu_time_used;  // Variable for storing the elapsed CPU time
    if (argc == 1) {
        printf("pzip: file1 [file2 ...]\n"); // "pzip: file1 [file2 ...]\n"
        return 1;
    }
    // Record the start time
    //start = clock();
    int NThreads = get_nprocs(); //
    int CurrentCount = 0;
    int CurrentMax = 16;
    CharCount *StoredCounts = malloc(CurrentMax * 5);

    for (int i = 1; i < argc; i++) {
        Compression(argv[i], NThreads, &CurrentCount, &CurrentMax, &StoredCounts);
    }
    fwrite(StoredCounts, 5, CurrentCount, stdout);
    free(StoredCounts);
    // Record the end time
    end = clock();

    // Calculate the elapsed CPU time in seconds
//    cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;

    // Print the elapsed CPU time
//    printf("Elapsed CPU time: %f seconds\n", cpu_time_used);
    return 0;
}
