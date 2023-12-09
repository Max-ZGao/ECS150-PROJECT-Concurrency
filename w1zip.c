#include <stdio.h>
#include <stdlib.h>
#include <time.h>  // Include the time.h library for benchmarking

int main(int argc, char *argv[]) {
    // Check for command-line arguments
    if (argc == 1) {
        printf("wzip: file1 [file2 ...]\n");
        return 1;
    }

    // Variables for benchmarking
    clock_t start, end;
    double cpu_time_used;

    // Record the start time
    start = clock();

    // Main compression logic
    int count = 0, curr = 0, prev = -1;
    for (int i = 1; i < argc; i++) {
        FILE *file = fopen(argv[i], "r");
        if (file == NULL) {
            perror("wzip");
            return 1;
        }

        while ((curr = fgetc(file)) != EOF) {
            if (prev == -1) {
                prev = curr;
                count++;
            } else if (curr != prev) {
                fwrite(&count, sizeof(int), 1, stdout);
                fwrite(&prev, 1, 1, stdout);
                count = 1;
            } else {
                count++;
            }
            prev = curr;
        }
        fclose(file);
    }

    // If there are remaining characters, write the count and character to stdout
    if (count > 0) {
        fwrite(&count, sizeof(int), 1, stdout);
        fwrite(&prev, 1, 1, stdout);
    }

    // Record the end time
    end = clock();

    // Calculate the elapsed CPU time in seconds
    cpu_time_used = ((double)(end - start)) / CLOCKS_PER_SEC;

    // Print the elapsed CPU time
    printf("Elapsed CPU time: %f seconds\n", cpu_time_used);

    return 0;
}
