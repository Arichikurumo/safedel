#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#define BLOCK_SIZE 4096 // 4 KB
#define NUM_PASSES 3    // Number of overwrite passes

void overwrite_drive(const char *device) {
    int fd = open(device, O_WRONLY);
    if (fd < 0) {
        perror("Error opening device");
        exit(EXIT_FAILURE);
    }

    unsigned char *buffer = malloc(BLOCK_SIZE);
    if (!buffer) {
        perror("Memory allocation failed");
        close(fd);
        exit(EXIT_FAILURE);
    }

    off_t total_bytes_written = 0;
    ssize_t bytes_written;
    srand(time(NULL));

    for (int pass = 1; pass <= NUM_PASSES; pass++) {
        printf("Pass %d/%d: Overwriting with %s data...\n", pass, NUM_PASSES, (pass % 2 == 1) ? "random" : "zeros");

        lseek(fd, 0, SEEK_SET); // Rewind the file descriptor to the beginning
        memset(buffer, 0, BLOCK_SIZE); // Clear buffer for zero writes

        if (pass % 2 == 1) {
            for (size_t i = 0; i < BLOCK_SIZE; i++) {
                buffer[i] = rand() % 256; // Fill with random data
            }
        }

        while ((bytes_written = write(fd, buffer, BLOCK_SIZE)) > 0) {
            total_bytes_written += bytes_written;
        }

        if (bytes_written < 0) {
            perror("Write error");
            free(buffer);
            close(fd);
            exit(EXIT_FAILURE);
        }
    }

    printf("Total bytes written: %ld\n", total_bytes_written);

    free(buffer);
    close(fd);
}

void confirm_action(const char *device) {
    printf("WARNING: This will erase all data on the device '%s' irreversibly.\n", device);
    printf("Type 'YES' to proceed: ");
    char response[10];
    fgets(response, sizeof(response), stdin);

    if (strcmp(response, "YES\n") != 0) {
        printf("Operation aborted.\n");
        exit(EXIT_SUCCESS);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <device>\n", argv[0]);
        fprintf(stderr, "Example: %s /dev/sdX\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *device = argv[1];
    confirm_action(device);
    overwrite_drive(device);

    printf("Data wipe completed successfully.\n");
    return 0;
}
