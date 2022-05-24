#include <stdio.h> // for printf, fprintf, stderr
#include <stdlib.h>
#include <threads.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <linux/limits.h>


#define FAIL 1
#define SUCCESS 0

// TODO: need to validate all mutex functions (init, lock, unlock)
// TODO: add path array to dir_queue with max_length - to be able to add dirname, but hold absolute path
typedef struct node node;
typedef struct queue queue;
void enqueue(queue* _queue, char* _data);
char* dequeue(queue* _queue);
void handle_dir(char* dir_path_entry);
void handle_file(char* file_path_entry, char* filename);
int search_dir(char* dirname);
void* thread_run();
int main(int argc, char **argv);



//============================== Initializations
queue* dir_queue;
int num_threads_waiting;
int threads_error = 0;
char* TERM;
int threads_alive_counter; // I think qlock is good enough to make this variable atomic - only place it is written to is with qlock locked.
int seach_files_counter;

// locks and cvs - need to initialize

mtx_t qlock; // for accessing num_threads_waiting and dir_queue
mtx_t start_lock;
cnd_t notEmpty;

mtx_t threads_counter_lock; // for accessing threads_error, threads_alive_counter
mtx_t search_files_counter_lock;

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
	//printf("in enqueue, data=%s\n", _data);
    // initialize new node
    node* new_node = (struct node*) malloc (sizeof(struct node));
   	new_node->data = (char*) malloc (sizeof(char)*PATH_MAX);
    strcpy(new_node->data, _data);

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

char* dequeue(queue* _queue) {
	char* data_to_return = (char*) malloc (sizeof(char)*PATH_MAX);
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
    //printf("node_to_pop->data=%s\n", node_to_pop->data);
    strcpy(data_to_return, node_to_pop->data);
    free(node_to_pop); // no need for this node anymore

    return data_to_return;
}

void test_function(queue* dir_queue) {
	node* curr_node = dir_queue->front;
	char* data;
	int i=0;
	while (curr_node != NULL) {
		data = curr_node->data;
		printf("** node %d data = %s\n", i, data);
		i++;
		curr_node = curr_node->next;
	}
}

//==================================================== Thread functions
void handle_dir(char* dir_path_entry) {
	// printf("*** in handle_dir, dir_path_entry=%s\n", dir_path_entry);
    // check if dp_entry can be searched
    DIR* dir_entry_pointer = opendir(dir_path_entry);
    //printf("[handle_dir]: dir_path_entry=%s\n" , dir_path_entry);
    if (dir_entry_pointer != NULL) { // enqueue
        mtx_lock(&qlock);
        //printf("about to enqueue:%s \n", dir_path_entry);
        enqueue(dir_queue, dir_path_entry);
        cnd_signal(&notEmpty); // always needs to signal or only when queue was empty before adding?
        mtx_unlock(&qlock);
    }
    else { // returned NULL - dir_path_entry can't be searched
        printf("Directory %s: Permission denied.\n", dir_path_entry);
        //fprintf(stderr, "ERROR: unable to open dir, errno: %s\n", strerror(errno));
    }
    closedir(dir_entry_pointer);
    return;
}

void handle_file(char* file_path_entry, char*filename) {
    char* ret = strstr(filename, TERM);
    if (ret != NULL) {
        mtx_lock(&search_files_counter_lock);
        seach_files_counter ++;
        mtx_unlock(&search_files_counter_lock);
        //printf("***found file:\n");
        printf("%s\n", file_path_entry);
    }
}


// Assumption dirname holds the full path to this dir (from root)
int search_dir(char* dirname) {
    // char path_entry[PATH_MAX];  // defined in <limits.h>
    DIR* dir_pointer;
    struct dirent *entry;
    struct stat stats;
    char* path_entry = (char*) malloc (sizeof(char)*PATH_MAX);

    dir_pointer = opendir(dirname);
    //printf("dirname=%s\n", dirname);
    if (dir_pointer == NULL) {
        fprintf(stderr, "ERROR: unable to open dir, errno: %s\n", strerror(errno));
        return FAIL;
    }
    while ( (entry=readdir(dir_pointer)) ) {
    	//printf("iteration=%d\n", ++i);
        if (strcmp(entry->d_name, ".")==0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        strcpy(path_entry, dirname); // to iterate parent dir to entry
        strcat(path_entry, "/");
        strcat(path_entry, entry->d_name);

        if (lstat(path_entry, &stats) != 0) { // TODO: not sure if to use stat or lstat?
            fprintf(stderr, "ERROR: unable to get entry stats, errno: %s\n", strerror(errno));
        }
        if (S_ISDIR(stats.st_mode)) {
        	//printf("isdir: path_entry=%s\n", path_entry);
            handle_dir(path_entry); // check if dir is searchable and if yes add to queue
        }
        else {
	        //printf("isfile: path_entry=%s\n", path_entry);
            handle_file(path_entry, entry->d_name); // handle folder - check term
        }
    }

    if (closedir(dir_pointer) != 0) {
        fprintf(stderr, "ERROR: failed to close dir, errno: %s\n", strerror(errno));
        return FAIL;
    }
    //printf("*** finished search_dir\n");
    return SUCCESS;
}


void* thread_run(){
    // FLOW:

    // 1: signal from main that all threads are created and ready to start searching == start_lock is available
    mtx_lock(&start_lock);
    mtx_unlock(&start_lock); // allow other threads to retrieve this lock so they can start searching too

    while(1) { // keep trying to dequeue and search until nothing left
        // 2: dequeue
        mtx_lock(&qlock);  // Assumption: dir_queue (==dequeue action) and num_threads_waiting are atomic with qlock
        //printf("***num_threads_waiting=%d, threads_alive_counter=%d\n", num_threads_waiting, threads_alive_counter);
        while (dir_queue->front == NULL ) { // dir queue is empty 
            // check if all other threads are waiting - dont want to be in cond_wait if there is no more potential work to do
            // Assumption: num_threads_waiting == threads_alive_counter-1 iff there are no threads currently working
            if (num_threads_waiting == threads_alive_counter-1) { // TODO: not sure if num_threads_waiting could also be == THREADS_ALIVE_COUNTER
            	//printf("*** inside condition: num_threads_waiting == threads_alive_counter-1\n");
                mtx_lock(&threads_counter_lock);
                threads_alive_counter--;
                mtx_unlock(&threads_counter_lock);

                cnd_signal(&notEmpty); // wake up another thread so it can also exit - all threads will eventually be woken up and get to this condition to exit // TODO: not sure if this should be cnd_broadcast?
                mtx_unlock(&qlock); //unlock before exiting

                thrd_exit(SUCCESS); // all threads will get to this condition and exit
            }
            num_threads_waiting++; // before waiting - add myself to waiting list
            cnd_wait(&notEmpty,&qlock); // need to be woken up when notEmpty.
            num_threads_waiting--; // got woken up so removing itself from waiting list
        }
        char* dirname = dequeue(dir_queue);  // woken up and there is data in dir_queue - time to dequeue
        //printf("*** in thread_run, dequeued: %s\n", dirname);
        mtx_unlock(&qlock);
        
        // 3: Search dir...
        int ret = search_dir(dirname);
        if (ret != SUCCESS) {

            mtx_lock(&threads_counter_lock);
            threads_alive_counter--;
            threads_error = 1;

            // check if need to wake up another thread // if queue is empty but not all threads are waiting - it means that there are other threads working who can wake up other threads 
            mtx_lock(&qlock);
            if (dir_queue->front != NULL) { // if queue is not empty - wake up another thread to start searching
                cnd_signal(&notEmpty);
            }
            else if (num_threads_waiting==threads_alive_counter-1) { // if no more potential work - wake up another thread to exit
                cnd_signal(&notEmpty); 
            }

            mtx_unlock(&threads_counter_lock);
            mtx_unlock(&qlock);

            thrd_exit(FAIL);
        }
        //thrd_exit(SUCCESS);
    }

}


//==================================================== Main Functions
/*
• argv[1]: search root directory (search for files within this directory and its subdirectories).
• argv[2]: search term (search for file names that include the search term).
• argv[3]: number of searching threads to be used for the search (assume a valid integer greater
than 0)
*/

// TODO: make sure main finished in a case that all threads exited due to an error

int main(int argc, char **argv) {
    // int rc;
    int status;

    if (argc != 4) {
	    fprintf(stderr, "ERROR: number of args=%d, incorrect number of arguments, errno: %s\n", argc, strerror(EINVAL));
        exit(FAIL);
    }

    char* root_dir = argv[1]; // TODO: can I assume that this is a dir and not a file? Can I assume that this is not "." or ".."?
    TERM = argv[2]; // global variable
    int num_threads = atoi(argv[3]); // global variable
    threads_alive_counter = num_threads;
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
        //printf("Main: creating thread %ld\n", t);
        if (thrd_create(&thread[t], (thrd_start_t) thread_run, NULL) != thrd_success) {
            fprintf(stderr, "ERROR in thrd_create(), errno: %s\n", strerror(errno));
            exit(FAIL);
        }
    }
    // 4: Signal threads to start searching once all threads were sucessfully created - by allowing them to access start_lock once all the threads are created
    mtx_unlock(&start_lock);

    // --- Wait for threads to finish ------------------
    for (long t = 0; t < num_threads; ++t) {
        if (thrd_join(thread[t], &status) != thrd_success) {  // &status is not optional - can also be NULL
            fprintf(stderr, "ERROR in thrd_join(), errno: %s\n", strerror(errno));
            exit(FAIL);
        }
        //printf("Main: completed join with thread %ld having a status of %ld\n", t, (long)status); // TODO: remove this printf...
    }

    // --- Epilogue ------------------------------------
	printf("Done searching, found %d files\n", seach_files_counter);
	
    if (threads_error != 0) { // there was an error in at least one of the threads
    	//printf("***[main]: threads_error == 0 - therefore returning non zero value\n");
    	
        return FAIL;
    }
    return SUCCESS;
}
//=================== END OF FILE ====================
