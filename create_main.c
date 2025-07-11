//------------------------------//
//        CREATE COMMAND        //
//------------------------------//

#include "request_server.c"

int main(int argc, char* argv[]) {

    if(argc < 4) {
        printf("Insufficient number of arguments (4 expected). ");
        exit(1);
    }
    const char *path_to_file              = argv[1];
    const char *path_to_ipc_file          = argv[2];
    const char *path_to_main_memory       = argv[3];
    const char *path_to_checkpoint_memory = argv[4];

    pid_t pid = fork();

    if(pid == 0) {
        request_server_workload(path_to_file, path_to_ipc_file, path_to_main_memory, path_to_checkpoint_memory);
    }
    else if(pid > 0) {
        printf("Child PID = %i\n", pid);
        // Do nothing.
    }
    else {
        printf("Fork failed. ");
        exit(1);
    }

    return 0;

}
