//------------------------------//
//         START COMMAND        //
//------------------------------//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <fcntl.h>
#include "shared_object.c"

#define handle_error(msg) \
           do { perror(msg); exit(EXIT_FAILURE); } while (0)

int main(int argc, char* argv[]) {

    if (argc < 1) {
        printf("Insufficient number of arguments (1 expected). ");
        exit(1);
    }
    const char *path_to_ipc_file = argv[1];

    //----------------------------------//
    //    Inter-Process Communication   //
    //----------------------------------//
    printf("Configuring IPC...\n");

    // Map a process-level semaphore to signal when to start
    // running a computation.
    int fd = open(path_to_ipc_file, O_RDWR);
    if (fd == -1)
        handle_error("Failed to open file (fd == -1)\n");
    int size_of_shared_obj = sizeof(struct Shared_Object);
    struct Shared_Object *shared_obj = mmap(NULL, size_of_shared_obj, O_RDWR, MAP_FILE | MAP_SHARED, fd, 0);
    if (shared_obj == MAP_FAILED)
        handle_error("Failed to call mmap (shared_obj == MAP_FAILED)\n");
    close(fd);
    // And no initialization here, it already happened in the request_server.

    // Extract the semaphore.
    sem_t *proceed_to_run = &shared_obj->proceed_to_run;

    printf("Open the semaphore...\n");
    sem_post(proceed_to_run);

    printf("End of command. \n");
    return 0;

}