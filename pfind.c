#include <stdio.h> // for printf, fprintf, stderr
#include <stdlib.h>
#include <thread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>


#define FAIL 1
#define SUCCESS 0

// TODO: need to validate all mutex functions (init, lock, unlock)
// TODO: add path array to dir_queue with max_length - to be able to add dirname, but hold absolute path

//============================== Initializations
queue* dir_queue;
int num_threads_waiting;
char* TERM;
int NUM_THREADS;
// int thread_at_work;

// locks and cvs - need to initialize

mtx_t qlock; // for accessing num_threads_waiting and dir_queue
cnd_t notEmpty;

// mtx_t working_thread_lock; // for modifying thread_at_work

//==================================== Queue Implementation - using linked list
typedef struct node {
    char* data;
    struct node* next;
} node;

typedef struct queue {
    struct node* front;
    struct node* rear;
} queue;

void enqueue(queue* _queue, char* _data) {
    // initialize new node
    node* new_node = (struct node*) malloc (sizeof(struct node));
    new_node->data = _data;

    // check if queue is empty - if yes add node to front and rear of queue
    if (_queue->front == NULL) {
        _queue->front = new_node;
        _queue->rear = new_node;
    }
    else { // else add to front only and update next
        node* current_front = _queue->front;
        _queue->front = new_node;
        new_node->next = current_front;
    }
    return;
}

node* dequeue(queue* _queue) {
    // assert that dequeue was called when no empty?
    if (_queue->front == NULL) {
        return NULL; // raise exception?
    }
    // check if queue is holding 1 item (meaning front and rear are both pointing to this item)
    node* node_to_pop = _queue->front;
    node* next_node = node_to_pop->next;

    if (next_node == NULL){
        _queue->front = NULL;
        _queue->rear = NULL;
    }
    else { // no need to update rear
        _queue->front = next_node;
    }
    char* data_to_return = node_to_pop->data;
    free(node_to_pop); // no need for this node anymore

    return data_to_return;
}


//==================================================== Thread functions
void handle_dir(dir_entry) {
    // check if 
    DIR* dir_entry_pointer;
    if ( (dir_entry_pointer=opendir(dir_entry)) ) { // enqueue
        mtx_lock(&qlock);
        enqueue(dir_queue, dir_entry);
        cnd_signal(&notEmpty); // always needs to signal or only when queue was empty before adding?
        mtx_unlock(&qlock);
    }
    else { // returned NULL - dir_entry can't be searched
        printf("Directory %s: Permission denied.\n", dir_entry); // TODO: dir_entry needs to be full path!
    }
    return;
}

void handle_folder(folder_entry) {
    char* ret = strstr(folder_entry, TERM);
    if (ret != NULL) {
        printf("%s\n", folder_entry); // TODO: folder_entry needs to be full path
    }
}


int search_dir(dirname) {
    DIR* dir_pointer;
    struct dirent *entry;
    struct stat stats;

    dir_pointer = opendir(dirname);
    if (dir_pointer == NULL) {
        fprintf(stderr, "ERROR: unable to open dir, errno: %s\n", strerror(errno));
        return FAIL;
    }
    while ( (entry=readdir(dir_pointer)) ) {
        if (entry->d_name == "." || entry->d_name == "..") {
            continue;
        }
        if (lstat(entry, &stats) != 0) { // TODO: not sure if to use stat or lstat?
            fprintf(stderr, "ERROR: unable to get entry stats, errno: %s\n", strerror(errno));
        }
        if (S_ISDIR(stats.st_mode)) {
            handle_dir(entry); // check if dir is searchable and if yes add to queue
        }
        else {
            // handle folder - check term
            handle_folder(entry);
        }
    }

    if (closedir(dir_pointer) != 0) {
        fprintf(stderr, "ERROR: failed to close dir, errno: %s\n", strerror(errno));
        return FAIL;
    }
    return SUCCESS;
}


void thread_run(){
    // FLOW:

    // 1: signal from main that all threads are created and ready to start searching == start_lock is available
    rc = mtx_lock(&start_lock);
    rc = mtx_unlock(&start_lock); // allow other threads to retrieve this lock so they can start searching too

    while(1) { // keep trying to dequeue and search until nothing left
        
        // 2: dequeue
        mtx_lock(&qlock);  // Assumption: dir_queue (==dequeue action) and num_threads_waiting are atomic with qlock
        while (dir_queue->front == NULL ) { // dir queue is empty - need to be woken up when notEmpty.
            // check if no threads are waiting - dont want to be in cond_wait if there is no more potential work to do
            if (num_threads_waiting == NUM_THREADS && thread_at_work == 0) { // no threads are waiting or working and nothing to search for - exit cleanly. //TODO: do i need to have a special lock for num_threads_waiting?
                mtx_unlock(&qlock); //unlock before exiting
                thrd_exit(NULL); // all threads will get to this condition and exit
                // exit(SUCCESS);
            }
            num_threads_waiting++; // before waiting - add myself to waiting list
            cnd_wait(&notEmpty,&qlock);
            num_threads_waiting--; // got woken up so removing itself from waiting list
        }
        char* dirname = dequeue(dir_queue);  // woken up and there is data in dir_queue - time to dequeue
        // mtx_lock(&working_thread_lock);
        // thread_at_work++ ;
        // mtx_unlock(&working_thread_lock);
        mtx_unlock(&qlock);
        
        // 3: Search dir...
        int ret = search_dir(dirname);
        
        // mtx_lock(&working_thread_lock);
        // thread_at_work--;
        // mtx_unlock(&working_thread_lock);
        
        if (ret != SUCCESS) {
            thrd_exit(NULL);
        }
         // only when thread finishes processing the info from queue it removes itself from waiting list
                                // this is so that the queue and waiting list will be empty iff there are no threads working.
    }

}


//==================================================== Main Functions

// def launch_threads(int num_threads) {

//     // --- Create threads -----------------------------
//     for (long t = 0; t < num_threads; t++) {
//         printf("Main: creating thread %ld\n", t);
//         if (thrd_create(&thread[t], thread_run, NULL) != thrd_success) {
//             fprintf(stderr, "ERROR in thrd_create(), errno: %s\n", strerror(errno));
//             exit(FAIL);
//         }
//     }

//     // --- Wait for threads to finish ------------------
//     for (long t = 0; t < num_threads; ++t) {
//         if (thrd_join(thread[t], &status) != thrd_success) {  // &status is not optional - can also be NULL
//             fprintf(stderr, "ERROR in thrd_join(), errno: %s\n", strerror(errno));
//             exit(FAIL);
//         }
//         printf("Main: completed join with thread %ld having a status of %ld\n", t, (long)status);
//     }
// }

/*
• argv[1]: search root directory (search for files within this directory and its subdirectories).
• argv[2]: search term (search for file names that include the search term).
• argv[3]: number of searching threads to be used for the search (assume a valid integer greater
than 0)
*/

// TODO: make sure main finished in a case that all threads exited due to an error

int main(int argc, char **argv) {
    int rc;
    int status;

    if (argc != 4) {
	    fprintf(stderr, "ERROR: number of args=%d, incorrect number of arguments, errno: %s\n", argc, strerror(EINVAL));
        exit(FAIL);
    }

    char* root_dir = argv[1]; // TODO: can I assume that this is a dir and not a file? Can I assume that this is not "." or ".."?
    TERM = argv[2]; // global variable
    NUM_THREADS = atoi(argv[3]); // global variable
    thrd_t thread[num_threads];

    if (strcmp(root_dir, ".") == 0 || strcmp(root_dir, "..") == 0 || opendir(root_dir) == NULL){
        fprintf(stderr, "ERROR: The root directory cannot be searched, errno: %s\n", strerror(EINVAL));
        exit(FAIL);
    }

    //--- Starting Flow ------------------------------
    // 1: initialize queue
    dir_queue = (struct queue*) malloc (sizeof(struct queue));

    // 2: add root_dir to queue
    enqueue(dir_queue, root_dir);

    // 3: Launch threads ------------------------------
    mtx_lock(&start_lock);
    for (long t = 0; t < num_threads; t++) {
        printf("Main: creating thread %ld\n", t);
        if (thrd_create(&thread[t], thread_run, NULL) != thrd_success) {
            fprintf(stderr, "ERROR in thrd_create(), errno: %s\n", strerror(errno));
            exit(FAIL);
        }
    }
    mtx_unlock(&start_lock); // the threads will manage to lock start_lock once all the threads are created - this is the signal from main for them to start searching

    // --- Wait for threads to finish ------------------
    for (long t = 0; t < num_threads; ++t) {
        if (thrd_join(thread[t], &status) != thrd_success) {  // &status is not optional - can also be NULL
            fprintf(stderr, "ERROR in thrd_join(), errno: %s\n", strerror(errno));
            exit(FAIL);
        }
        printf("Main: completed join with thread %ld having a status of %ld\n", t, (long)status);
    }


    // 4: Signal to send to threads to start ?
    cnd_wait(&all_threads_ready_signal, &threads_ready_lock); // wait for a thread to signal that all threads are ready
    cnd_broadcast(&start_signal);
    
    // 5: exit program according to specific conditions... 



    // --- Epilogue ------------------------------------
    printf("Main: program completed. Exiting. Counter = %d\n", counter);
    thrd_exit(SUCCESS);
}
//=================== END OF FILE ====================