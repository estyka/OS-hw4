#include <stdio.h> // for printf, fprintf, stderr
#include <stdlib.h>
#include <pthread.h>

#define FAIL 1
#define SUCCESS 0

//============================== Initializations
queue* dir_queue;
queue* thread_queue; // queue of all threads waiting - initialize with all threads...


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

    // check if queue is empty
    if (_queue->front == NULL) {
        _queue->front = new_node;
        _queue->rear = new_node;
    }
    else {
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

//===================================== Atomic Functions
pthread_mutex_t qlock;
pthread_cond_t notEmpty;

/* … initialization code … */

void atomic_enqueue(item x) {
    pthread_mutex_lock(&qlock);
    enqueue(x);
    pthread_cond_signal(&notEmpty);
    pthread_mutex_unlock(&qlock);
}

char* atomic_dequeue() { // not sure I'll use this function... put it seperately when needed...
    pthread_mutex_lock(&qlock);
    while (dir_queue->front == NULL) { // while <queue is empty>
        // before calling pthread_cond_wait check if all other queues are waiting - if yes return NULL and finish
        pthread_cond_wait(&notEmpty,&qlock);
        char* dirname = dequeue();
        pthread_mutex_unlock(&qlock);
        return dirname;
    }
}

//==================================================== Thread functions
void thread_run(){
    // FLOW:
    // 1: some sort of signal from main is meant to make this function start (not sure of signal from other thread as well?)
    // 2: dequeue
    pthread_mutex_lock(&qlock);
    while (dir_queue->front == NULL ) { // dir queue is empty - need to be woken up when notEmpty.
        // check if no threads are waiting - dont want to be in cond_wait if there is no more potential work to do
        if (thread_queue->front == NULL) { // no threads are waiting - exit cleanly. //TODO: do i need to have a special lock for this queue? even if i'm only checking it and not doing enqueue/dequeue?
            pthread_mutex_unlock(&qlock); //unlock before exiting
            exit(SUCCESS);
        }
        // not enough to call - pthread_cond_wait(&notEmpty,&qlock)
        // need to add itself to waiting queue, and then somehow wake up in correct order.
        thread_queue
        pthread_cond_wait(&notEmpty,&qlock);
    }
    char* dirname = dequeue();  // woken up and there is data in dir_queue - time to dequeue
    pthread_mutex_unlock(&qlock);
    
    // 3: Search dir...
}


//==================================================== Main Functions

def launch_threads(int num_threads) {

    // --- Create threads -----------------------------
    for (long t = 0; t < num_threads; t++) {
        printf("Main: creating thread %ld\n", t);
        if (pthread_create(&thread[t], NULL, thread_run, NULL) != 0) {
            fprintf(stderr, "ERROR in thrd_create(), errno: %s\n", strerror(errno));
            exit(FAIL);
        }
    }

    // --- Wait for threads to finish ------------------
    for (long t = 0; t < num_threads; ++t) {
        if (pthread_join(thread[t], &status) != 0) {  // &status is not optional - can also be NULL
            fprintf(stderr, "ERROR in thrd_join(), errno: %s\n", strerror(errno));
            exit(FAIL);
        }
        printf("Main: completed join with thread %ld having a status of %ld\n", t, (long)status);
    }
}

/*
• argv[1]: search root directory (search for files within this directory and its subdirectories).
• argv[2]: search term (search for file names that include the search term).
• argv[3]: number of searching threads to be used for the search (assume a valid integer greater
than 0)
*/
int main(int argc, char **argv) {
    int rc;
    int status;

    if (argc != 4) {
	    fprintf(stderr, "ERROR: number of args=%d, incorrect number of arguments, errno: %s\n", argc, strerror(EINVAL));
        exit(FAIL);
    }

    char* root_dir = argv[1]; // TODO: can I assume that this is a dir and not a file? Can I assume that this is not "." or ".."?
    char* term = argv[2];
    int num_threads = atoi(argv[3]);
    pthread_t thread[num_threads];

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


    // 4: Signal to send to threads to start ?
    
    // 5: exit program according to specific conditions... 



    // --- Epilogue ------------------------------------
    printf("Main: program completed. Exiting. Counter = %d\n", counter);
    thrd_exit(SUCCESS);
}
//=================== END OF FILE ====================