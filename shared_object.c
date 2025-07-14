//------------------------------//
//        SHARED OBJECT         //
//------------------------------//

// Used for IPC (inter-process communication).

#include <semaphore.h>

struct Shared_Object {
    sem_t proceed_to_run;
    sem_t should_migrate;
    int   should_migrate_flag;
};
