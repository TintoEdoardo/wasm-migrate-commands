//------------------------------//
//        REQUEST SERVER        //
//------------------------------//

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <wasm.h>
#include <wasmtime.h>
#include <wasmtime/func.h>
#include <unistd.h>

#define handle_error(msg) \
           do { perror(msg); exit(EXIT_FAILURE); } while (0)

//  This structure contains a semaphore used to synchronize
//  the access to should_migrate.
struct State {
    // Should migrate can be either:
    // 0      -> false
    // not 0  -> true
    int should_migrate;
    sem_t *semaphore;
};

///  Support function for logging error.
static void exit_with_error(const char *message, wasmtime_error_t *error,
                            wasm_trap_t *trap);

///  Should_migrate callback, this function is
///  imported in the Wasm module.
static wasm_trap_t *should_migrate(void *env, wasmtime_caller_t *caller,
                                   const wasmtime_val_t *args, size_t nargs,
                                   wasmtime_val_t *results, size_t nresults) {
    // Return value.
    wasmtime_val_t result[1];

    // Extract the caller state.
    wasmtime_context_t *context = wasmtime_caller_context(caller);
    struct State *state = wasmtime_context_get_data(context);

    // Enqueue on the semaphore.
    sem_wait(state->semaphore);

    // When acquired, check should_migrate.
    result[0].kind   = WASMTIME_I32;
    result[0].of.i32 = state->should_migrate;
    nresults         = sizeof result;

    // Relinquish the semaphore.
    sem_post(state->semaphore);

    // And save the results.
    results = result;

    return NULL;
}

///  Restore_memory callback, used by the Wasm module to
///  make a copy of the linear memory and checkpoint memory.
static wasm_trap_t *restore_memory(void *env, wasmtime_caller_t *caller,
                                      const wasmtime_val_t *args, size_t nargs,
                                      wasmtime_val_t *results, size_t nresults) {

}

///  The workload of a request server, it initialize the computation
///  then sleep waiting for the activation command. While running
///  a Wasm function, if a migration request is issued, it suspend the
///  execution and produce a checkpoint.
int request_server_workload(const char *path_to_file, const char *path_to_ipc_file) {

    //----------------------------------//
    //    Inter-Process Communication   //
    //----------------------------------//
    printf("Configuring IPC...\n");

    // Prepare a process-level semaphore to signal when to start
    // running a computation.
    int fd = open(path_to_ipc_file, O_RDONLY | O_WRONLY);
    if (fd == -1)
        handle_error("Failed to open file (fd == -1)");
    int size_of_sem = sizeof(sem_t);
    ftruncate(fd, size_of_sem);
    sem_t *proceed_to_run =
            mmap(NULL, size_of_sem, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, fd, 0);
    if (proceed_to_run == MAP_FAILED)
        handle_error("Failed to call mmap (proceed_to_run == MAP_FAILED)");
    sem_init(proceed_to_run, 1, 0);
    close(fd);

    // Then do the same thing fot signalling a pending migration.
    fd = open(path_to_ipc_file, O_RDONLY | O_WRONLY);
    if (fd == -1)
        handle_error("Failed to open file (fd == -1)");
    ftruncate(fd, size_of_sem * 2);
    sem_t *should_migrate_sem =
            mmap(NULL, size_of_sem, PROT_READ | PROT_WRITE, MAP_FILE | MAP_SHARED, fd, size_of_sem);
    if (should_migrate_sem == MAP_FAILED)
        handle_error("Failed to call mmap (should_migrate_sem == MAP_FAILED)");
    sem_init(should_migrate_sem, 1, 0);
    close(fd);

    //----------------------------------//
    //          Initialization          //
    //----------------------------------//

    // The state of the Wasm computation.
    struct State state;
    state.should_migrate = 0;
    state.semaphore      = should_migrate_sem;

    //  Initialize the engine.
    printf("Initializing...\n");
    wasm_engine_t *engine = wasm_engine_new();
    assert(engine != NULL);

    // Initialize the store.
    wasmtime_store_t *store = wasmtime_store_new(engine, &state, NULL);
    assert(store != NULL);

    // Initialize the context.
    wasmtime_context_t *context = wasmtime_store_context(store);

    // Read Wasm bytecode from file.
    FILE *file = fopen(path_to_file, "r");
    assert(file != NULL);
    fseek(file, 0L, SEEK_END);
    size_t file_size = ftell(file);
    fseek(file, 0L, SEEK_SET);
    wasm_byte_vec_t wat;
    wasm_byte_vec_new_uninitialized(&wat, file_size);
    if (fread(wat.data, file_size, 1, file) != 1) {
        printf("> Error loading module!\n");
        return 1;
    }
    fclose(file);

    // Parse the wat into the binary wasm format.
    wasm_byte_vec_t wasm;
    wasmtime_error_t *error = wasmtime_wat2wasm(wat.data, wat.size, &wasm);
    if (error != NULL)
        exit_with_error("failed to parse wat", error, NULL);
    wasm_byte_vec_delete(&wat);

    // Now that we've got our binary webassembly we can compile our module.
    printf("Compiling module...\n");
    wasmtime_module_t *module = NULL;
    error = wasmtime_module_new(engine, (uint8_t *)wasm.data, wasm.size, &module);
    wasm_byte_vec_delete(&wasm);
    if (error != NULL)
        exit_with_error("failed to compile module", error, NULL);

    // Add the expected callbacks for checkpoint and restore.
    printf("Creating callback...\n");
    wasm_functype_t *should_migrate_ty = wasm_functype_new_0_0();
    wasmtime_func_t should_migrate_t;
    wasmtime_func_new(context, should_migrate_ty, should_migrate, NULL, NULL, &should_migrate_t);

    // With our callback function we can now instantiate the compiled module,
    // giving us an instance we can then execute exports from. Note that
    // instantiation can trap due to execution of the `start` function, so we need
    // to handle that here too.
    printf("Instantiating module...\n");
    wasm_trap_t *trap = NULL;
    wasmtime_instance_t instance;
    wasmtime_extern_t import;
    import.kind = WASMTIME_EXTERN_FUNC;
    import.of.func = should_migrate_t;
    error = wasmtime_instance_new(context, module, &import, 1, &instance, &trap);
    if (error != NULL || trap != NULL)
        exit_with_error("failed to instantiate", error, trap);

    // Lookup our `run` export function
    printf("Extracting export...\n");
    wasmtime_extern_t run;
    bool ok = wasmtime_instance_export_get(context, &instance, "run", 3, &run);
    assert(ok);
    assert(run.kind == WASMTIME_EXTERN_FUNC);

}

static wasm_trap_t *hello_callback(void *env, wasmtime_caller_t *caller,
                                   const wasmtime_val_t *args, size_t nargs,
                                   wasmtime_val_t *results, size_t nresults) {
    printf("Calling back...\n");
    printf("> Hello World!\n");
    return NULL;
}

int thing() {
    int ret = 0;
    // Set up our compilation context. Note that we could also work with a
    // `wasm_config_t` here to configure what feature are enabled and various
    // compilation settings.
    printf("Initializing...\n");
    wasm_engine_t *engine = wasm_engine_new();
    assert(engine != NULL);

    // With an engine we can create a *store* which is a long-lived group of wasm
    // modules. Note that we allocate some custom data here to live in the store,
    // but here we skip that and specify NULL.
    wasmtime_store_t *store = wasmtime_store_new(engine, NULL, NULL);
    assert(store != NULL);
    wasmtime_context_t *context = wasmtime_store_context(store);

    // Read our input file, which in this case is a wasm text file.
    FILE *file = fopen("examples/hello.wat", "r");
    assert(file != NULL);
    fseek(file, 0L, SEEK_END);
    size_t file_size = ftell(file);
    fseek(file, 0L, SEEK_SET);
    wasm_byte_vec_t wat;
    wasm_byte_vec_new_uninitialized(&wat, file_size);
    if (fread(wat.data, file_size, 1, file) != 1) {
        printf("> Error loading module!\n");
        return 1;
    }
    fclose(file);

    // Parse the wat into the binary wasm format
    wasm_byte_vec_t wasm;
    wasmtime_error_t *error = wasmtime_wat2wasm(wat.data, wat.size, &wasm);
    if (error != NULL)
        exit_with_error("failed to parse wat", error, NULL);
    wasm_byte_vec_delete(&wat);

    // Now that we've got our binary webassembly we can compile our module.
    printf("Compiling module...\n");
    wasmtime_module_t *module = NULL;
    error = wasmtime_module_new(engine, (uint8_t *)wasm.data, wasm.size, &module);
    wasm_byte_vec_delete(&wasm);
    if (error != NULL)
        exit_with_error("failed to compile module", error, NULL);

    // Next up we need to create the function that the wasm module imports. Here
    // we'll be hooking up a thunk function to the `hello_callback` native
    // function above. Note that we can assign custom data, but we just use NULL
    // for now).
    printf("Creating callback...\n");
    wasm_functype_t *hello_ty = wasm_functype_new_0_0();
    wasmtime_func_t hello;
    wasmtime_func_new(context, hello_ty, hello_callback, NULL, NULL, &hello);

    // With our callback function we can now instantiate the compiled module,
    // giving us an instance we can then execute exports from. Note that
    // instantiation can trap due to execution of the `start` function, so we need
    // to handle that here too.
    printf("Instantiating module...\n");
    wasm_trap_t *trap = NULL;
    wasmtime_instance_t instance;
    wasmtime_extern_t import;
    import.kind = WASMTIME_EXTERN_FUNC;
    import.of.func = hello;
    error = wasmtime_instance_new(context, module, &import, 1, &instance, &trap);
    if (error != NULL || trap != NULL)
        exit_with_error("failed to instantiate", error, trap);

    // Lookup our `run` export function
    printf("Extracting export...\n");
    wasmtime_extern_t run;
    bool ok = wasmtime_instance_export_get(context, &instance, "run", 3, &run);
    assert(ok);
    assert(run.kind == WASMTIME_EXTERN_FUNC);

    // And call it!
    printf("Calling export...\n");
    error = wasmtime_func_call(context, &run.of.func, NULL, 0, NULL, 0, &trap);
    if (error != NULL || trap != NULL)
        exit_with_error("failed to call function", error, trap);

    // Clean up after ourselves at this point
    printf("All finished!\n");
    ret = 0;

    wasmtime_module_delete(module);
    wasmtime_store_delete(store);
    wasm_engine_delete(engine);
    return ret;
}

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
}