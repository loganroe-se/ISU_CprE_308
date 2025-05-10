#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <string.h>
#include <sys/time.h>
#include "Bank.c"

#define MAX_ACCOUNTS 1000

// Structure for a transaction pair
struct trans { 
    int acc_id; // account ID
    int amount; // amount to be added, could be positive or negative
};
// Structure for a request
struct request {
    struct request * next; // pointer to the next request in the list
    int request_id; // request ID assigned by the main thread
    int check_acc_id; // account ID for a CHECK request
    struct trans * transactions; // array of transaction data
    int num_trans; // number of accounts in this transaction
    struct timeval starttime, endtime; // starttime and endtime for TIME
};
// Structure for the queue of jobs
struct queue {
    struct request * head, * tail; // head and tail of the list
    int num_jobs; // number of jobs currently in queue
};

// Declare global variables
pthread_mutex_t queueMutex; // Holds the mutex for the queue
pthread_cond_t startWorker; // Holds the conditional to signal workers to run
pthread_cond_t allJobsFinished; // Holds the conditional to signal when all jobs are done
struct queue jobQueue; // The queue for all of the jobs
FILE *file; // The file to write outputs to
pthread_mutex_t accountMutexes[MAX_ACCOUNTS]; // Holds mutexes for all of the accounts
int numAvailableWorkers, numWorkers, endFlag = 0; // Holds the number of currently available workers, total number of workers, and an end flag

// Declare functions
void *workers(void *);
void *runWorkers(void *);
void balCheck(struct request* nextRequest);
void transactionReq(struct request* nextRequest);
void quickSort(int arr[], int low, int high);
int partition(int arr[], int low, int high);
void swap(int* p1, int* p2);

/* The main function for the banking system
 * Inputs:
 *    Arg 1 -- # of worker threads
 *    Arg 2 -- # of accounts
 *    Arg 3 -- Output file name
*/
int main(int argc, char *argv[]) {
    // Initialize queue mutex
    pthread_mutex_init(&queueMutex, NULL);

    // Initialize conditional that signals workers to run
    pthread_cond_init(&startWorker, NULL);

    // The current request ID
    int currReqID = 1;

    // Retrieve the passed in values
    // Arg 1 -- # of worker threads
    // Arg 2 -- # of accounts
    // Arg 3 -- output file
    numWorkers = atoi(argv[1]);
    int numAccounts = atoi(argv[2]);
    char outputFile[500];
    strncpy(outputFile, argv[3], sizeof(outputFile) - 1);

    // Open the file with write priveleges
    file = fopen(outputFile, "w+");

    // Holds the pthread workers
    pthread_t pthreadWorkers[numWorkers];

    // Create all pthreads
    for (int i = 0; i < numWorkers; i++) {
        pthread_create(&pthreadWorkers[i], NULL, workers, NULL);
    }

    // Holds a pthread to signal the workers to run
    pthread_t pthreadRunWorkers;
    pthread_create(&pthreadRunWorkers, NULL, runWorkers, NULL);
    // Signal to send when there are no jobs
    pthread_cond_init(&allJobsFinished, NULL);

    // Initialize all mutexes
    for (int i = 0; i < numAccounts; i++) {
        pthread_mutex_init(&accountMutexes[i], NULL);
    }

    // Initialize the bank accounts - Error out if the init fails
    if (!initialize_accounts(numAccounts)) {
        printf("Failed to initialize accounts, exiting.\n");
        exit(1);
    }

    while(1) {
        // Print the > sign - means input line
        printf("> ");

        // Create a variable to handle input
        char tempInput[500];
        
        // Read the user input -- error if fgets fails
        if (fgets(tempInput, 500, stdin) == NULL) { exit(1); }
        // Remove the extra newline characters
        tempInput[strcspn(tempInput, "\n")] = 0;
        // Remove whitespace at start
        int i, j, regChar = 0;
        char input[500];
        for (i = 0, j = 0; i < strlen(tempInput); i++, j++) {
            // Copy over all characters that are not whitespace until first non-whitespace char, then copy all
            if (regChar || tempInput[i] != ' ') {
                regChar = 1; // Set the flag to 1 to notify to copy the rest, including whitespaces
                input[j] = tempInput[i];
            } else {
                j--;
            }
        }
        input[j] = 0;

        // If the user input is empty, continue the loop
        if (!strcmp(input, "")) {
            continue;
        }

        // Depending on the type of the request, perform the related action
        if (!strncmp(input, "CHECK", 5) || !strncmp(input, "TRANS", 5)) {
            // Get the total list
            char* requestArgs[30];
            // Keep track of the number of arguments -- Start at 0 to ignore first value
            int numArgs = 0;

            // Get the first value in inputCopy
            char* ptr = strtok(input, " ");
            // Loop through inputCopy until it is NULL
            while (ptr != NULL) {
                requestArgs[numArgs++] = ptr;
                ptr = strtok(NULL, " ");
            }

            // Create a new request
            struct request* newRequest;
            // Allocate memory
            newRequest = (struct request*) malloc(sizeof(struct request));

            // Fill generic data
            gettimeofday(&newRequest->starttime, NULL);
            newRequest->request_id = currReqID++;

            // Print out the ID
            printf("< ID %d\n", newRequest->request_id);

            // Determine which request occurred
            if (!strncmp(input, "CHECK", 5)) {
                // Fill the request with data
                newRequest->check_acc_id = atoi(requestArgs[1]);
            } else {
                // Create the transaction array
                struct trans* transactions;
                // Keep track of where we are in the requestArgs array
                int currLoc = 1;
                // Divide the number or aguments by two to ignore amount values
                numArgs /= 2;
                // Allocate memory
                transactions = (struct trans*) malloc(numArgs * sizeof(struct trans));

                // Loop through all of the transactions to occur
                for (int i = 0; i < numArgs; i++) {
                    struct trans transaction = {atoi(requestArgs[currLoc++]), atoi(requestArgs[currLoc++])};
                    transactions[i] = transaction;
                }

                // Fill the request with data
                newRequest->num_trans = numArgs;
                newRequest->transactions = transactions;
            }

            // Lock the queue
            pthread_mutex_lock(&queueMutex);
            
            // Add the request to the queue
            if (jobQueue.head == NULL) {
                jobQueue.head = newRequest;
                jobQueue.tail = newRequest;
            } else {
                jobQueue.tail->next = newRequest;
                jobQueue.tail = newRequest;
            }

            // Increment the total number of jobs
            jobQueue.num_jobs++;

            // Unlock the queue
            pthread_mutex_unlock(&queueMutex);
        } else if (!strncmp(input, "END", 3)) {
            endFlag = 1;
            break;
        } else {
            // If execution arrives here, an invalid request was entered
            printf("An invalid request was entered. The following are allowed: CHECK, TRANS, END.\n");
            continue;
        }
    }

    // Wait for a signal saying all jobs have finished
    pthread_mutex_lock(&queueMutex);
    pthread_cond_wait(&allJobsFinished, &queueMutex);
    pthread_mutex_unlock(&queueMutex);

    // Free the bank accounts & close the file
    free_accounts();
    fclose(file);
    exit(0);
}

/* Holds the thread that tells the worker threads when to run
 * Inputs:
 *    arg -- No inputs are required
*/
void *runWorkers(void *arg) {
    // Infinite loop
    while (1) {
        // Lock the queue mutex
        pthread_mutex_lock(&queueMutex);
        // If any jobs are available, send a signal
        if (jobQueue.num_jobs > 0) {
            pthread_cond_signal(&startWorker);
        } else if (endFlag && numAvailableWorkers == numWorkers) {
            pthread_cond_signal(&allJobsFinished);
        }
        // Unlock the mutex
        pthread_mutex_unlock(&queueMutex);
    }
}

/* Holds all of the worker threads
 *  - Keeps track of available workers and pulls jobs off the queue
 *  - Calls the appropriate helper method to perform the request 
 *
 * Inputs:
 *    arg -- No inputs are required
*/
void *workers(void *arg) {
    // Infinite loop
    while (1) {
        // Lock the queue mutex
        pthread_mutex_lock(&queueMutex);
        // Increment the number of available workers
        numAvailableWorkers++;
        // Wait for the associated conditional variable to be signaled
        while (jobQueue.num_jobs == 0) {
            pthread_cond_wait(&startWorker, &queueMutex);
        }
        // Decrement the number of available workers
        numAvailableWorkers--;
        // Grab the next job off the queue
        struct request* nextRequest = (struct request*) malloc(sizeof(struct request));
        nextRequest = jobQueue.head;
        jobQueue.head = jobQueue.head->next;
        jobQueue.num_jobs--;
        // Unlock the queue mutex
        pthread_mutex_unlock(&queueMutex);
        // Call helper functions
        if (nextRequest->check_acc_id != 0) {
            // It must be a balance check - call the helper
            balCheck(nextRequest);
        } else {
            // It must be a transaction - call the helper
            transactionReq(nextRequest);
        }
    }
}

/* Performs a balance check on the given request
 * Inputs:
 *    nextRequest -- The request struct that holds the balance check
*/
void balCheck(struct request* nextRequest) {
    // Lock the account's mutex
    pthread_mutex_lock(&accountMutexes[nextRequest->check_acc_id - 1]);
    // Get the balance of the account
    int bal = read_account(nextRequest->check_acc_id);
    // Unlock the account's mutex
    pthread_mutex_unlock(&accountMutexes[nextRequest->check_acc_id - 1]);

    // Get the end time
    gettimeofday(&nextRequest->endtime, NULL);
    // Lock the file, print, unlock the file
    flockfile(file);
    fprintf(file, "%d BAL %d TIME %ld.%06ld %ld.%06ld\n", nextRequest->request_id, bal, 
        nextRequest->starttime.tv_sec, nextRequest->starttime.tv_usec, nextRequest->endtime.tv_sec, nextRequest->endtime.tv_usec);
    funlockfile(file);
}

/* Performs a transaction request
 * Inputs:
 *    nextRequest -- The request struct that holds the transaction
*/
void transactionReq(struct request* nextRequest) {
    // Get a list of all accounts affected
    int accountList[nextRequest->num_trans];
    for (int i = 0; i < nextRequest->num_trans; i++) {
        accountList[i] = nextRequest->transactions[i].acc_id;
    }

    // Sort the account IDs
    quickSort(accountList, 0, nextRequest->num_trans - 1);

    // Lock all associated accounts in ascending order
    for (int i = 0; i < nextRequest->num_trans; i++) {
        // Lock the account's mutex
        pthread_mutex_lock(&accountMutexes[accountList[i] - 1]);
    }

    // Flag for if an invalid balance was found
    int invalidBalance = 0;
    // Holds the account that had an issue
    int invalidAccID;

    // Loop through the transactions
    for (int i = 0; i < nextRequest->num_trans; i++) {
        if ((read_account(nextRequest->transactions[i].acc_id) + nextRequest->transactions[i].amount) < 0) {
            // Set the invalid account ID
            invalidAccID = nextRequest->transactions[i].acc_id;
            // Set the flag to 1
            invalidBalance = 1;
            // Break out of the loop
            break;
        }
    }

    // If no invalid balanace was found, perform the transactions
    if (!invalidBalance) {
        for (int i = 0; i < nextRequest->num_trans; i++) {
            // Write the new value to the account
            write_account(nextRequest->transactions[i].acc_id, read_account(nextRequest->transactions[i].acc_id) + nextRequest->transactions[i].amount);
        }
    }

    // Unlock all associated accounts in ascending order
    for (int i = 0; i < nextRequest->num_trans; i++) {
        // Unlock the account's mutex
        pthread_mutex_unlock(&accountMutexes[accountList[i] - 1]);
    }

    // Get the end time
    gettimeofday(&nextRequest->endtime, NULL);
    // Lock the file
    flockfile(file);
    // Depending ont he state of the invalidBalance flag, print the output
    if (!invalidBalance) {
        // Print the successful output
        fprintf(file, "%d OK TIME %ld.%06ld %ld.%06ld\n", nextRequest->request_id, 
            nextRequest->starttime.tv_sec, nextRequest->starttime.tv_usec, nextRequest->endtime.tv_sec, nextRequest->endtime.tv_usec);
    } else {
        // Print out the unsuccessful output
        fprintf(file, "%d ISF %d TIME %ld.%06ld %ld.%06ld\n", nextRequest->request_id, invalidAccID, 
            nextRequest->starttime.tv_sec, nextRequest->starttime.tv_usec, nextRequest->endtime.tv_sec, nextRequest->endtime.tv_usec);
    }
    // Unlock the file
    funlockfile(file);
}

/* Quicksort -- Sorts the array in ascending order
 * Inputs:
 *    arr -- The array to sort
 *    min -- The minimum index of arr
 *    max -- The maximum index of arr
*/
void quickSort(int arr[], int min, int max)
{
    // When min is less than max
    if (min < max) {
        // Get the partition index
        int returnIdx = partition(arr, min, max);

        // Recursion Calls
        // Smaller element than pivot goes left and max element goes right
        quickSort(arr, min, returnIdx - 1);
        quickSort(arr, returnIdx + 1, max);
    }
}

/* Partitions the array based on min and max
 * Inputs:
 *    arr -- The array to partition
 *    min -- The minimum index of arr
 *    max -- The maxmimum index of arr
 * 
 * Outputs:
 *    int -- Returns the partition index
*/
int partition(int arr[], int min, int max)
{
    // Choose the pivot as the max index
    int pivot = arr[max];

    // Index of smaller element and Indicate
    // the right position of pivot found so far
    int i = (min - 1);

    for (int j = min; j <= max; j++) {
        // If current element is smaller than the pivot
        if (arr[j] < pivot) {
            // Increment index of smaller element
            i++;
            // Swap the values
            swap(&arr[i], &arr[j]);
        }
    }
    // Swap the values and return
    swap(&arr[i + 1], &arr[max]);
    return (i + 1);
}

/* Swaps the values at p1 and p2
 * Inputs:
 *    p1 -- Value 1 to swap
 *    p2 -- Value 2 to swap
*/
void swap(int* p1, int* p2)
{
    int temp;
    temp = *p1;
    *p1 = *p2;
    *p2 = temp;
}