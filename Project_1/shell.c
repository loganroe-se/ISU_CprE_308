#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <pthread.h>
#include <libgen.h>

#define NUM_BACKGROUND 10

// Declare global variables
pthread_mutex_t  mut;
pthread_cond_t child_cv;
int numBgProcesses = 0;

// Defines a background process
typedef struct bgProcess {
    char* cmd; // The name of the cmd that was run
    int PID; // The PID of the background process
} bgProcess;

// Declare functions
int builtInCmds(char input[500], bgProcess bgProc[]);
void programCmd(char input[500], bgProcess bgProc[]);
void checkBgProcesses(bgProcess bgProc[]);
void printStatus(int pid, int status, char* cmd);
void removeBgProcess(bgProcess bgProc[], int idx);

/* The main function for the UNIX-like shell
* Switches:
*   -p <prompt> : This becomes the user prompt - Default is "308sh> "
*/
int main(int argc, char *argv[]) {
    // String to hold the user prompt
    char userPrompt[50] = "308sh> ";

    // Loop through all arguments passed in
    int i;
    for (i = 1; i < argc; i++) {
        // Check for the -p switch
        if (!strcmp(argv[i], "-p")) {
            // Read the input to userPrompt
            strncpy(userPrompt, argv[++i], sizeof(userPrompt) - 1);
        }
    }

    // Define the struct to hold background processes
    bgProcess bgProc[NUM_BACKGROUND];

    // Initialize mutex & conditionals
    pthread_mutex_init(&mut, NULL);
    pthread_cond_init(&child_cv, NULL);

    // Run an infinite loop waiting for user input
    while(1) {
        // Call a helper to check for background processes
        checkBgProcesses(bgProc);

        // Print the command line
        printf("%s", userPrompt);

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

        // Check for builtInCmds
        // Return -- 0 if none ran; 1 if continue
        if (builtInCmds(input, bgProc)) {
            continue;
        }

        // If execution makes it this far, it must be a program command
        programCmd(input, bgProc);
    }
}

/* Runs the all built in commands
* Inputs:
*   input -- Char array that is the user's inputted value
*   bgProc -- The struct that holds all background process information
*
* Outputs:
*   int -- 0 if no commands ran; 1 if one command did run
*/
int builtInCmds(char input[500], bgProcess bgProc[]) {
    // If the user input is exit, break out of the loop
    if (!strcmp(input, "exit")) {
        exit(0);
    }

    // If the user input is pid, print the process ID
    if (!strcmp(input, "pid")) {
        printf("Process ID: %d\n", getpid());
        return 1;
    }

    // If the user input is ppid, print the parent's process ID
    if (!strcmp(input, "ppid")) {
        printf("Parent Process ID: %d\n", getppid());
        return 1;
    }
    
    // If the user input is cd & has no further arguments, go to home directory
    // Else, if the user input is cd & has further arguments, go to that location, if valid
    if (!strncmp(input, "cd", 2) && strlen(input) == 2) {
        chdir(getenv("HOME"));
        return 1;
    } else if (!strncmp(input, "cd", 2)) {
        char cdArgs[500] = "";
        // Copy the argument
        strncpy(cdArgs, input + 3, strlen(input) - 3);
        // CD to the specified directory -- if fails, tell the user
        if (chdir(cdArgs) != 0) {
            printf("cd: %s: No such file or directory\n", cdArgs);
        }
        return 1;
    }

    // If the user input is pwd, print the current working directory
    if (!strcmp(input, "pwd")) {
        char cwd[500];
        printf("%s\n", getcwd(cwd, sizeof(cwd)));
        return 1;
    }

    // If the user input is jobs, print the current jobs that are running
    if (!strcmp(input, "jobs")) {
        // Double check the jobs didn't finish already
        checkBgProcesses(bgProc);
        int i;
        // Loop through all of the current background processes and print them
        for (i = 0; i < numBgProcesses; i++) {
            printf("[%d] %d: %s\n", i + 1, bgProc[i].PID, bgProc[i].cmd);
        }
        return 1;
    }

    return 0;
}

/* Runs the all program commands
* Inputs:
*   input -- Char array that is the user's inputted value
*   bgProc -- The struct that holds all background process information
*/
void programCmd(char input[500], bgProcess bgProc[]) {
    // Get the command name
    char* cmd;
    char inputCopy[500];
    strcpy(inputCopy, input);
    cmd = strtok(inputCopy, " ");

    // Get the total list
    char* cmdArgs[20];

    // Reset variables
    strcpy(inputCopy, input);

    // Get the first value in inputCopy
    char* ptr = strtok(inputCopy, " ");
    // Loop through inputCopy until it is NULL
    int i = 0;
    while (ptr != NULL) {
        cmdArgs[i++] = ptr;
        ptr = strtok(NULL, " ");
    }

    // Check if the final value is an ampersand (background process)
    int bgProcess = 0;
    if (!strcmp(cmdArgs[i - 1], "&")) {
        cmdArgs[i - 1] = NULL;
        bgProcess = 1;
    } else {
        // Add NULL to the end of the cmdArgs array
        cmdArgs[i++] = NULL;
    }

    // Create a pipe to pass data between the child and parent
    int p[2];
    pipe(p);
    // Fork so that another process will run the command
    int pid = fork();
    // If pid is less than 0, it means the fork failed
    // Else if pid is equal to 0, it is the child process
    // Else, it is the parent process
    if (pid < 0) {
        printf("Fork failed.\n");
        exit(1);
    } else if (pid == 0) {
        // Lock the mutex
        pthread_mutex_lock(&mut);
        // Wait for the signal to execute the command
        pthread_cond_wait(&child_cv, &mut);
        // Close the read end of the pipe
        close(p[0]);
        // Execute the command
        int status = execvp(cmd, cmdArgs);
        // Write the status to the pipe
        write(p[1], &status, sizeof(status));
        // If the status is a -1, print that the command didn't exist
        // Otherwise, if bgFlag is 1, print that the background process was killed
        if (status == -1) {
            printf("Cannot exec %s: No such file or directory\n", cmd);
        }
        // Unlock the mutex
        pthread_mutex_unlock(&mut);
        // Kill the child process
        kill(getpid(), SIGTERM);
    } else {
        // Remove the */bin/*
        cmd = basename(cmd);
        // This is where the parent process will go
        // Print the child's process ID & the command to run
        printf("[%d] %s\n", pid, cmd);
        // Signal the child_cv conditional
        pthread_cond_broadcast(&child_cv);
        // If it is not a background process, wait on the child, if it is, do not wait on the child
        int status;
        if (!bgProcess) {
            // Wait on the child process to get killed
            waitpid(pid, &status, 0);
            // Get the child's execution status via the pipe
            int execStatus = 0;
            close(p[1]);
            read(p[0], &execStatus, sizeof(execStatus));
            // Call a helper to print the status
            if (execStatus == -1) {
                printStatus(pid, execStatus, cmd);
            } else {
                printStatus(pid, status, cmd);
            }
        } else {
            // Do not wait for the child process but still track the status
            waitpid(-1, &status, WNOHANG);
            // Assign struct values
            bgProc[numBgProcesses].PID = pid;
            bgProc[numBgProcesses].cmd = malloc(sizeof(cmd));
            strncpy(bgProc[numBgProcesses++].cmd, cmd, sizeof(cmd));
        }

        // Check if the command was a kill
        if (!strcmp(cmd, "kill")) {
            // Check if the kill was killing a background process
            int pidKilled = atoi(cmdArgs[1]);
            int j, flag = 0;
            for (j = 0; j < numBgProcesses; j++) {
                // If this is the right process, break out of the for loop
                if (pidKilled == bgProc[j].PID) {
                    flag = 1;
                    break;
                }
            }
            // Break out if it was not a background process
            if (flag) {
                // Ensure the kill worked by attempting to send the process a signal
                int killRet = kill(bgProc[j].PID, 0); // Signal 0 means it won't terminate
                // Kill succeeded so print that the command was killed and update the struct
                if (killRet == 0) {
                    // Print that the process was killed
                    printf("[%d] %s Killed (%d)\n", bgProc[j].PID, bgProc[j].cmd, SIGTERM);
                    // Call a helper to remove the background process
                    removeBgProcess(bgProc, j);
                }
            }
        }
    }
    
    // Ensure the cmdArgs array is empty to not affect future executions
    while (i >= 0) {
        cmdArgs[i--] = '\0';
    }
}

/* Checks if any background processes have finished
* Inputs:
*   bgProc -- The struct that holds all background process information
*/
void checkBgProcesses(bgProcess bgProc[]) {
    // Check if there were any background processes & if they finished
    int status;
    int backgroundPID = waitpid(-1, &status, WNOHANG);
    while (backgroundPID > 0) {
        // Find the process that this ID is associated with
        int i;
        for (i = 0; i < numBgProcesses; i++) {
            // If this is the right process, break out of the for loop
            if (backgroundPID == bgProc[i].PID) {
                // Print the background processes status
                printStatus(backgroundPID, status, bgProc[i].cmd);
                backgroundPID = 0;
                // Clean up the process in the struct
                removeBgProcess(bgProc, i);
                break;
            }
        }
        // Check for a new backgroundPID
        backgroundPID = waitpid(-1, &status, WNOHANG);
    }
}

/* Prints the status message of a command
* Inputs:
*   pid -- The PID of the process that has exited
*   status -- The status of the process that has exited
*   cmd -- The command ran for the process that has exited
*/
void printStatus(int pid, int status, char* cmd) {
    // Print the child's process ID & the command that was attempted to be run & exit status
    if (WIFEXITED(status)) {
        printf("[%d] %s Exit %d\n", pid, cmd, WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("[%d] %s Exit %d\n", pid, cmd, WTERMSIG(status));
    } else if (status == -1) {
        printf("[%d] %s Exit %d\n", pid, cmd, 255);
    }
}

/* Removes a background process from the struct
* Inputs:
*   bgProc -- The struct that holds all background process information
*   idx -- The index of the background process to remove
*/
void removeBgProcess(bgProcess bgProc[], int idx) {
    // Move all following processes forward one
    int i, j;
    for (i = idx; i < (numBgProcesses - 1); i++) {
        // Move values forward one
        j = i + 1;
        strncpy(bgProc[i].cmd, bgProc[j].cmd, sizeof(bgProc[j].cmd));
        bgProc[i].PID = bgProc[j].PID;
    }
    // Reset the values at the ending index & decrement total processes
    bgProc[--numBgProcesses].cmd = '\0';
    bgProc[numBgProcesses].PID = 0;
}