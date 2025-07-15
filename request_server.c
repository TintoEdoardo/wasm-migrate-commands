//------------------------------//
//        REQUEST SERVER        //
//------------------------------//

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <wasi.h>
#include <wasm.h>
#include <wasmtime.h>
#include <wasmtime/func.h>
#include <wasmtime/trap.h>
#include <unistd.h>
#include "shared_object.c"

#define handle_error(msg) \
           do { perror(msg); exit(EXIT_FAILURE); } while (0)

//  This structure contains a semaphore used to synchronize
//  the access to should_migrate.
struct State {
    // Should migrate can be either:
    // 0      -> false
    // not 0  -> true
    int *should_migrate;
    sem_t *semaphore;
    const char *path_to_main_memory;
    const char *path_to_checkpoint_memory;
};

///  Support function for logging error.
static void exit_with_error(const char *message, wasmtime_error_t *error,
                            wasm_trap_t *trap) {
    fprintf(stderr, "error: %s\n", message);
    wasm_byte_vec_t error_message;
    if (error != NULL) {
        wasmtime_error_message(error, &error_message);
        wasmtime_error_delete(error);
    } else {
        wasm_trap_message(trap, &error_message);
        wasm_trap_delete(trap);
    }
    fprintf(stderr, "%.*s\n", (int)error_message.size, error_message.data);
    wasm_byte_vec_delete(&error_message);
    exit(1);
};

///  Should_migrate callback, this function is
///  imported in the Wasm module.
static wasm_trap_t *should_migrate(void *env, wasmtime_caller_t *caller,
                                   const wasmtime_val_t *args, size_t nargs,
                                   wasmtime_val_t *results, size_t nresults) {

    printf("Evaluate should_migrate. \n");
    // Return value.
    wasmtime_val_t result[1];

    // Extract the caller state.
    wasmtime_context_t *context = wasmtime_caller_context(caller);
    struct State *state = wasmtime_context_get_data(context);

    // Enqueue on the semaphore.
    printf("Competing for lock...\n");
    if (sem_wait(state->semaphore) == -1)
        handle_error("sem_wait(state->semaphore) == -1\n");
    printf("Within the state lock... \n");

    // When acquired, check should_migrate.
    results[0].kind   = WASMTIME_I32;
    results[0].of.i32 = *state->should_migrate;
    nresults         = sizeof result;

    // Relinquish the semaphore.
    sem_post(state->semaphore);

    printf("End of should_migrate. \n");
    return NULL;
}

///  Restore_memory callback, used by the Wasm module to
///  make a copy of the linear memory and checkpoint memory.
static wasm_trap_t *restore_memory(void *env, wasmtime_caller_t *caller,
                                      const wasmtime_val_t *args, size_t nargs,
                                      wasmtime_val_t *results, size_t nresults) {

    printf("Restoring memory...\n");

    // Extract the caller state.
    wasmtime_context_t *context = wasmtime_caller_context(caller);
    struct State *state = wasmtime_context_get_data(context);

    const char *path_to_main_memory       = state->path_to_main_memory;
    const char *path_to_checkpoint_memory = state->path_to_checkpoint_memory;

    wasmtime_memory_t memory, checkpoint_memory;
    wasmtime_extern_t memory_item, checkpoint_memory_item;

    printf("Retrieve memory export...\n");
    // Get the main linear memory from the caller.
    wasmtime_caller_export_get(caller, "memory", strlen("memory"), &memory_item);
    memory = memory_item.of.memory;
    uint8_t *memory_ref = wasmtime_memory_data(context, &memory);

    // Get the checkpoint memory from the caller.
    wasmtime_caller_export_get(caller, "checkpoint_memory", strlen("checkpoint_memory"), &checkpoint_memory_item);
    checkpoint_memory = checkpoint_memory_item.of.memory;
    uint8_t *checkpoint_memory_ref = wasmtime_memory_data(context, &checkpoint_memory);

    //----------------------------------//
    //      Memory Initialization       //
    //----------------------------------//
    // This step is performed at runtime.
    printf("Read memory file: %s\n", path_to_main_memory);
    FILE *main_memory_fd = fopen(path_to_main_memory, "r");
    if (main_memory_fd != NULL) {
        fread(memory_ref, 1, 64 * 1024, main_memory_fd);
    } // Ignore otherwise.
    fclose(main_memory_fd);

    printf("Read memory file: %s\n", path_to_checkpoint_memory);
    FILE *checkpoint_memory_fd = fopen(path_to_checkpoint_memory, "r");
    if (checkpoint_memory_fd != NULL) {
        // Note that we are reading at most 4KB from the checkpoint_memory.
        fread(checkpoint_memory_ref, 1, 4 * 1024, checkpoint_memory_fd);
    } // Ignore otherwise.
    fclose(checkpoint_memory_fd);

    printf("Memory restored. \n");
    return NULL;
}

///  The workload of a request server, it initialize the computation
///  then sleep waiting for the activation command. While running
///  a Wasm function, if a migration request is issued, it suspend the
///  execution and produce a checkpoint.
int request_server_workload(
        const char *path_to_file,
        const char *path_to_ipc_file,
        const char *path_to_main_memory,
        const char *path_to_checkpoint_memory) {

    //----------------------------------//
    //    Inter-Process Communication   //
    //----------------------------------//
    printf("Configuring IPC...\n");

    // Prepare a process-level semaphore to signal when to start
    // running a computation.
    int fd = open(path_to_ipc_file, O_RDWR);
    if (fd == -1)
        handle_error("Failed to open file (fd == -1)\n");
    int size_of_shared_obj = sizeof(struct Shared_Object);

    // Prepare to host two semaphores.
    ftruncate(fd, size_of_shared_obj);
    // Here semaphores is an array of two semaphores.
    struct Shared_Object *shared_obj = mmap(NULL, size_of_shared_obj, O_RDWR, MAP_FILE | MAP_SHARED, fd, 0);
    if (shared_obj == MAP_FAILED)
        handle_error("Failed to call mmap (shared_obj == MAP_FAILED)\n");
    close(fd);

    // The real semaphores are defined next.
    sem_t *proceed_to_run = &shared_obj->proceed_to_run;
    if (sem_init(proceed_to_run, 1, 0) == -1)
        handle_error("sem_init(proceed_to_run, 1, 0) == -1\n");
    sem_t *should_migrate_sem = &shared_obj->should_migrate;
    if (sem_init(should_migrate_sem, 1, 1) == -1)
        handle_error("sem_init(should_migrate_sem, 1, 1) == -1\n");
    shared_obj->should_migrate_flag = 0;

    //----------------------------------//
    //          Initialization          //
    //----------------------------------//

    // The state of the Wasm computation.
    struct State state;
    state.should_migrate            = &shared_obj->should_migrate_flag;
    state.semaphore                 = should_migrate_sem;
    state.path_to_main_memory       = path_to_main_memory;
    state.path_to_checkpoint_memory = path_to_checkpoint_memory;

    //  Initialize the engine.
    printf("Initializing...\n");
    wasm_config_t *config = wasm_config_new();
    assert(config != NULL);
    wasmtime_config_wasm_multi_memory_set(config, true);
    wasm_engine_t *engine = wasm_engine_new_with_config(config);
    assert(engine != NULL);

    // Initialize the store.
    wasmtime_store_t *store = wasmtime_store_new(engine, &state, NULL);
    assert(store != NULL);

    // Initialize the context.
    wasmtime_context_t *context = wasmtime_store_context(store);

    // Create a linker with WASI functions defined.
    wasmtime_linker_t *linker = wasmtime_linker_new(engine);
    wasmtime_error_t *error   = wasmtime_linker_define_wasi(linker);
    if (error != NULL)
        exit_with_error("Failed to link Wasi. \n", error, NULL);

    // Read Wasm bytecode from file.
    printf("Loading binary...\n");
    FILE *file = fopen(path_to_file, "rb");
    if (!file) {
        printf("> Error opening module. \n");
        return 1;
    }
    fseek(file, 0L, SEEK_END);
    size_t file_size = ftell(file);
    fseek(file, 0L, SEEK_SET);
    wasm_byte_vec_t binary;
    wasm_byte_vec_new_uninitialized(&binary, file_size);
    if (fread(binary.data, file_size, 1, file) != 1) {
        printf("> Error reading module!\n");
        return 1;
    }
    fclose(file);

    // Compile the module.
    printf("Compiling module...\n");
    wasmtime_module_t *module = NULL;
    error   = wasmtime_module_new(engine, (uint8_t *)binary.data, binary.size, &module);
    wasm_byte_vec_delete(&binary);
    if (error != NULL)
        exit_with_error("Failed to compile module. \n", error, NULL);

    // Instantiate wasi
    wasi_config_t *wasi_config = wasi_config_new();
    assert(wasi_config);
    wasmtime_config_wasm_multi_memory_set(config, true);

    wasi_config_inherit_argv(wasi_config);
    wasi_config_inherit_env(wasi_config);
    wasi_config_inherit_stdin(wasi_config);
    wasi_config_inherit_stdout(wasi_config);
    wasi_config_inherit_stderr(wasi_config);
    error = wasmtime_context_set_wasi(context, wasi_config);
    if (error != NULL)
        exit_with_error("Failed to instantiate Wasi. ", error, NULL);

    // Add the expected callbacks for checkpoint and restore.
    printf("Creating callbacks...\n");
    wasm_functype_t *should_migrate_ty = wasm_functype_new_0_1(wasm_valtype_new_i32());
    wasmtime_func_t should_migrate_t;
    wasmtime_func_new(context, should_migrate_ty, should_migrate, NULL, NULL, &should_migrate_t);
    wasmtime_linker_define_func(linker, "host", strlen("host"), "should_migrate", strlen("should_migrate"), should_migrate_ty,
                                should_migrate, NULL, NULL);

    wasm_functype_t *restore_memory_ty = wasm_functype_new_0_0();
    wasmtime_func_t restore_memory_t;
    wasmtime_func_new(context, restore_memory_ty, restore_memory, NULL, NULL, &restore_memory_t);
    wasmtime_linker_define_func(linker, "host", strlen("host"), "restore_memory", strlen("restore_memory"), restore_memory_ty,
                                restore_memory, NULL, NULL);

    // Instantiate the module.
    printf("Instantiating module...\n");
    // error = wasmtime_linker_module(linker, context, "", 0, module);
    wasmtime_instance_t instance;
    wasm_trap_t *trap;
    error = wasmtime_linker_instantiate(linker, context, module, &instance, &trap);
    if (error != NULL)
        exit_with_error("Failed to instantiate module. ", error, NULL);

    // Register our new `linking2` instance with the linker
    error = wasmtime_linker_define_instance(linker, context, "",
                                            strlen(""), &instance);
    if (error != NULL)
        exit_with_error("Failed to link instance. ", error, NULL);

    // Lookup our `run` export function.
    printf("Extracting export...\n");
    wasmtime_func_t func;

    error = wasmtime_linker_get_default(linker, context, "", 0, &func);
    printf("After extraction...\n");
    if (error != NULL)
       exit_with_error("failed to locate default export for module", error, NULL);

    //----------------------------------//
    //       Suspend until START        //
    //----------------------------------//

    printf("Wait for activation...\n");
    sem_wait(proceed_to_run);
    // Once the semaphore is relinquished, proceed to:

    //----------------------------------//
    //         Run the function         //
    //----------------------------------//

    // Call the function.
    printf("Calling export...\n");
    error = wasmtime_func_call(context, &func, NULL, 0, NULL, 0, &trap);
    if (error != NULL)
        exit_with_error("error calling default export", error, trap);

    if (trap != NULL) {
        wasm_message_t message;
        wasmtime_trap_code_t code;
        wasm_trap_message(trap, &message);
        bool is_wasm_trap = wasmtime_trap_code(trap, &code);
        wasm_trap_delete(trap);
        if (is_wasm_trap && code == WASMTIME_TRAP_CODE_UNREACHABLE_CODE_REACHED) {

            //----------------------------------//
            //        Checkpoint section        //
            //----------------------------------//

            wasmtime_memory_t memory, checkpoint_memory;
            wasmtime_extern_t memory_item, checkpoint_memory_item;

            // Get the main linear memory from the caller.
            wasmtime_instance_export_get(context, &instance, "memory", strlen("memory"), &memory_item);
            memory = memory_item.of.memory;
            uint8_t *memory_ref = wasmtime_memory_data(context, &memory);

            // Get the checkpoint memory from the caller.
            wasmtime_instance_export_get(context, &instance, "checkpoint_memory", strlen("checkpoint_memory"), &checkpoint_memory_item);
            checkpoint_memory = checkpoint_memory_item.of.memory;
            uint8_t *checkpoint_memory_ref = wasmtime_memory_data(context, &checkpoint_memory);

            FILE *main_memory_fd = fopen(path_to_main_memory, "w+");
            if (main_memory_fd != NULL) {
                fwrite(memory_ref, 1, 64 * 1024, main_memory_fd);
            }
            else {
                printf("main_memory_fd == NULL\n");
                exit(1);
            }
            fclose(main_memory_fd);

            FILE *checkpoint_memory_fd = fopen(path_to_checkpoint_memory, "w+");
            if (checkpoint_memory_fd != NULL) {
                fwrite(checkpoint_memory_ref, 1, 4 * 1024, checkpoint_memory_fd);
            }
            else {
                printf("checkpoint_memory_fd == NULL\n");
                exit(1);
            }
            fclose(checkpoint_memory_fd);

            printf("Checkpoint completed. \n");
        }
        else {
            fprintf(stdout, "%.*s\n", (int)message.size, message.data);
        }
        wasm_byte_vec_delete(&message);
    }

    // Finalize.
    printf("All finished!\n");
    wasmtime_module_delete(module);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);

    // Then return.
    return 0;

}