#include <stdio.h> // for printf, fprintf, stderr
#include <stdlib.h>
#include <threads.h>
#include <sys/stat.h>
#include <dirent.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <linux/limits.h>
#include <stdatomic.h>


#define FAIL 1
#define SUCCESS 0

typedef struct node node;
typedef struct queue queue;
int enqueue(queue* _queue, char* _data);
char* dequeue(queue* _queue);
int handle_dir(char* dir_path_entry);
void handle_file(char* file_path_entry, char* filename);
int search_dir(char* dirname);
void* thread_run();
int main(int argc, char **argv);


//============================== Variable declarations
queue* dir_queue;
atomic_int threads_waiting_counter;
atomic_int running_threads_counter; // using this var only for debugging - counts how many are still holding a dir from the queue
atomic_int threads_alive_counter; 
atomic_int search_files_counter;
atomic_int threads_error = 0;
char* TERM;

mtx_t qlock; // for accessing num_threads_waiting and dir_queue
mtx_t start_lock;
mtx_t print_lock;
cnd_t notEmpty;


//==================================== Queue Implementation - using linked list
typedef struct node {
    char* data;
    struct node* next;
} node;

typedef struct queue {
    struct node* front;
    struct node* rear;
} queue;

int enqueue(queue* _queue, char* _data) {
    // initialize new node
    node* new_node = (struct node*) malloc (sizeof(struct node));
    
    
    if (new_node == NULL) { // allocation failure
    	fprintf(stderr, "ERROR: memory allocation failed\n");
    	return FAIL;	
    }
    
    new_node->data = (char*) malloc (PATH_MAX * sizeof(char));
    
    if (new_node->data == NULL) { // allocation failure
	    fprintf(stderr, "ERROR: memory allocation failed\n");
    	return FAIL;	
    }
    
    strcpy(new_node->data, _data);
	
	new_node->next = _queue->front;
	_queue->front = new_node;
	
	if (_queue->front->next == NULL) { // check if queue is empty - if yes add node to rear as well
		_queue->rear = new_node;
	}

    return SUCCESS;
}

char* dequeue(queue* _queue) {
	char* data_to_return = (char*) malloc (PATH_MAX * sizeof(char));
	char* data_from_node;

    node* node_to_pop = _queue->front;
    data_from_node = node_to_pop->data;
  	
    _queue->front = node_to_pop->next;
	
    if (_queue->front == NULL){ // after pop queue is now empty - need to update rear
        _queue->rear = NULL;
    }
    free(node_to_pop); // no need for this node anymore
    strcpy(data_to_return, data_from_node);

    return data_to_return;
}


//==================================================== Thread functions
int handle_dir(char* dir_path_entry) {
    // check if dp_entry can be searched
    DIR* dir_entry_pointer = opendir(dir_path_entry);
    if (dir_entry_pointer != NULL) { // enqueue
        mtx_lock(&qlock);
        if (enqueue(dir_queue, dir_path_entry) != SUCCESS) {
        	threads_error = 1;
        	return FAIL;
        }
        cnd_signal(&notEmpty); // always needs to signal or only when queue was empty before adding?
        mtx_unlock(&qlock);
    }
    else { // returned NULL - dir_path_entry can't be searched
	    mtx_lock(&print_lock);
        printf("Directory %s: Permission denied.\n", dir_path_entry);
        mtx_unlock(&print_lock);
    }
    closedir(dir_entry_pointer);
    return SUCCESS;
}

void handle_file(char* file_path_entry, char*filename) {
    //char* found_term = ;
    if (strstr(filename, TERM)) {
        search_files_counter ++;
        mtx_lock(&print_lock);
        printf("%s\n", file_path_entry);
        mtx_unlock(&print_lock);
    }
}


// Assumption dirname holds the full path to this dir (from root)
int search_dir(char* dirname) {
    // char path_entry[PATH_MAX];  // defined in <limits.h>
    DIR* dir_pointer;
    struct dirent *entry;
    struct stat stats;
    char path_entry[PATH_MAX];

    dir_pointer = opendir(dirname);
    if (dir_pointer == NULL) {
        fprintf(stderr, "ERROR: unable to open dir, errno: %s\n", strerror(errno));
        return FAIL;
    }
    while ( (entry=readdir(dir_pointer)) ) {
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
            if (handle_dir(path_entry) != SUCCESS) { // check if dir is searchable and if yes add to queue
            	return FAIL;
            }
        }
        else {
            handle_file(path_entry, entry->d_name); // handle folder - check term
        }
    }

    if (closedir(dir_pointer) != 0) {
        fprintf(stderr, "ERROR: failed to close dir, errno: %s\n", strerror(errno));
        return FAIL;
    }
    return SUCCESS;
}


void* thread_run(){
	int ret;
	char* dirname;
    // FLOW:

    // 1: signal from main that all threads are created and ready to start searching == start_lock is available
    mtx_lock(&start_lock);
    mtx_unlock(&start_lock); // allow other threads to retrieve this lock so they can start searching too

    while(1) { // 2: keep trying to dequeue and search until nothing left
        mtx_lock(&qlock);  // Assumption: dir_queue (==dequeue action) and num_threads_waiting are atomic with qlock
        
        while (dir_queue->front == NULL ) { // dir queue is empty 
            // check if all other threads are waiting - dont want to be in cond_wait if there is no more potential work to do
            // Assumption: num_threads_waiting == threads_alive_counter-1 iff there are no threads currently working
            if (threads_waiting_counter == threads_alive_counter-1) { 
                threads_alive_counter--;
                
                cnd_signal(&notEmpty); // wake up another thread so it can also exit - all threads will eventually be woken up and get to this condition to exit
                mtx_unlock(&qlock); //unlock before exiting
                thrd_exit(SUCCESS); // all threads will get to this condition and exit
            }
            threads_waiting_counter++; // before waiting - add myself to waiting list
            cnd_wait(&notEmpty,&qlock); // need to be woken up when notEmpty.
            threads_waiting_counter--; // got woken up so removing itself from waiting list
        }
        dirname = dequeue(dir_queue);  // woken up and there is data in dir_queue - time to dequeue
        running_threads_counter++;        
        mtx_unlock(&qlock);
        
        // 3: Search dir...
        ret = search_dir(dirname);
        running_threads_counter--;
        
        if (ret != SUCCESS) {

            threads_alive_counter--;
            running_threads_counter--;
            threads_error = 1;

            mtx_lock(&qlock);
            
            if (dir_queue->front != NULL) { // if queue is not empty - wake up another thread to start searching
                cnd_signal(&notEmpty);
            }
            else if (threads_waiting_counter==threads_alive_counter-1) { // if no more potential work - wake up another thread to exit
                cnd_signal(&notEmpty); 
            }
            // else - if queue is empty but not all threads are waiting - it means that there are other threads working who can wake up other threads - TODO: make sure this is a correct assumption

            mtx_unlock(&qlock);
            thrd_exit(FAIL);
        }
    }

}


//==================================================== Main Functions
void initialize_locks_and_cvs() {
    mtx_init(&qlock, mtx_plain);
    mtx_init(&start_lock, mtx_plain);
    mtx_init(&print_lock, mtx_plain);
    cnd_init(&notEmpty);
}

void destroy_locks_and_cvs() {
    mtx_destroy(&qlock);
    mtx_destroy(&start_lock);
    mtx_destroy(&print_lock);
    cnd_destroy(&notEmpty);
}


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
    int ret;

    if (argc != 4) {
	    fprintf(stderr, "ERROR: number of args=%d, incorrect number of arguments, errno: %s\n", argc, strerror(EINVAL));
        exit(FAIL);
    }

    char* root_dir = argv[1]; // TODO: can I assume that this is a dir and not a file?
    TERM = argv[2]; // global variable
    int num_threads = atoi(argv[3]); // global variable
    threads_alive_counter = num_threads;
    thrd_t thread[num_threads];

    if (strcmp(root_dir, ".") == 0 || strcmp(root_dir, "..") == 0 || opendir(root_dir) == NULL){
        fprintf(stderr, "ERROR: The root directory cannot be searched, errno: %s\n", strerror(EINVAL));
        exit(FAIL);
    }

    //--- Starting Flow ------------------------------
    // 1: initializations
    initialize_locks_and_cvs();

    dir_queue = (struct queue*) malloc (sizeof(struct queue));

    // 2: add root_dir to queue
    ret = enqueue(dir_queue, root_dir);
    if (ret != SUCCESS) {
    	exit(FAIL);
    }

    // 3: Launch threads ------------------------------
    mtx_lock(&start_lock);
    for (long t = 0; t < num_threads; t++) {
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
    mtx_lock(&print_lock);
	printf("Done searching, found %d files\n", search_files_counter);
	mtx_unlock(&print_lock);
	
	destroy_locks_and_cvs();
	//free_queue();
	
    if (threads_error != 0) { // there was an error in at least one of the threads
        return FAIL;
    }
    return SUCCESS;
}
//=================== END OF FILE ====================
