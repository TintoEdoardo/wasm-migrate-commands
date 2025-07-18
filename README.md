# Building the library

### Requirements

1. Cmake (at least v3.24). 
2. The repository has one submodule (Wasmtime). Hence, it needs to be cloned adding the ```--recursive``` flag. 

### Steps

1. Enter the repository root directory ```wasm-migrate-commands```. 
2. From there run: \
 ```cmake -S /abs/path/to/wasm-migrate-commands -B /abs/path/to/wasm-migrate-commands/build```
3. Then build with: \
 ```cmake --build /abs/path/to/wasm-migrate-commands/build```

---

# Running the commands 

### Create Command

```
./create_command comp.wasm ipc_file.txt main_memory.b checkpoint_memory.b
```

###### Arguments

1. ```comp.wasm```: a Webassembly module that might (or might not) have injected the checkpoint and restore (C/R) procedures in [TintoEdoardo/wasm-tools](https://github.com/TintoEdoardo/wasm-tools/tree/develop). 
2. ```ipc_file.txt```: used for IPC communication. 
3. ```main_memory.b```: if the file exists, it is used to populate the linear memory of the function with C/R. Otherwise, this step is ignored.
4. ```checkpoint_memory.b```: if the file exists it is used to populate the checkpoint memory of the function with (C/R). 

### Start Command

```
./start_command ipc_file.txt
```

###### Arguments

1. ```ipc_file.txt```: used for IPC communication.

###### Notes

Fails if there is no process created with ```create_command``` waiting for activation.

### Migrate Command

```
./migrate_command ipc_file.txt
```

###### Arguments 

1. ```ipc_file.txt```: used for IPC communication.

###### Notes

Fails if there is no process created with ```create_command``` running or at least waiting for activation.
