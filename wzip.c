#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char* argv[]) {
    // command line is empty
    if (argc == 1) {
        printf("wzip: file1 [file2 ...]\n");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        // open the file
        FILE *fp = fopen(argv[i], "r");
        if (fp == NULL) {
            // failed to open a file
            printf("wunzip: cannot open file\n");
            return 1;
        }

        int cur_len = 0;
        char cur_char = '\0';
        char prev_char = '\0';
        while ((cur_char = fgetc(fp)) != EOF) {
            if (prev_char == '\0') {
                prev_char = cur_char;
                cur_len++;
            } else if (cur_char != prev_char) {
                // end of a sequence
                fwrite(&cur_len, sizeof(int), 1, stdout);
                fwrite(&prev_char, sizeof(char), 1, stdout);
                cur_len = 1;
            } else {
                cur_len++;
            }
            prev_char = cur_char;
        }
        
        // this is the last sequence
        if (cur_len > 0) {
            fwrite(&cur_len, sizeof(int), 1, stdout);
            fwrite(&prev_char, 1, 1, stdout);
        }
        fclose(fp);
    }

    return 0;
}
