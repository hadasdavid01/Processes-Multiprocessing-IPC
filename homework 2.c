/*
/*
 * ex2.c
 *
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <curl/curl.h>
#include <string.h>
#include <signal.h>

#include <semaphore.h>

#define HTTP_OK 200L
#define REQUEST_TIMEOUT_SECONDS 2L
#define SNAME "/mysem"
#define URL_OK 0
#define URL_UNKNOWN -1
#define URL_ERROR -2
#define EXIT_SIGNAL 11

#define MAX_PROCESSES 1024

const char URL_PREFIX[] = "http";

volatile typedef struct ResultStruct{
		double sum;
		int amount, unknown;
} ResultStruct ;


void usage() {
	fprintf(stderr, "usage:\n\t./ex2 num_of_processes FILENAME\n");
	exit(EXIT_FAILURE);
}

void exit_signal_handler() {
	signal(EXIT_SIGNAL, exit_signal_handler);
	exit(EXIT_FAILURE);
}

double check_url(const char *url) {
	CURL *curl;
	CURLcode res;
	double response_time = URL_UNKNOWN;

	curl = curl_easy_init();

	if(strncmp(url, URL_PREFIX, strlen(URL_PREFIX)) != 0){
		return URL_ERROR;
	}

	if(curl) {
		curl_easy_setopt(curl, CURLOPT_URL, url);
		curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
		curl_easy_setopt(curl, CURLOPT_TIMEOUT, REQUEST_TIMEOUT_SECONDS);
		curl_easy_setopt(curl, CURLOPT_NOBODY, 1L); /* do a HEAD request */

		res = curl_easy_perform(curl);
		if(res == CURLE_OK) {
			curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &response_time);
		}

		curl_easy_cleanup(curl);

	}

	return response_time;

}

void serial_checker(const char *filename) {

	ResultStruct results = {0};

	FILE *toplist_file;
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	double res;
	int  rc, cd;

	ResultStruct *mmappedData;
	if((mmappedData = mmap(NULL, sizeof(ResultStruct), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) == MAP_FAILED)
	{
		perror("unable to create mapping");
		exit(EXIT_FAILURE);
	}
	mmappedData->sum = 0;
	mmappedData->amount = 0;
	mmappedData->unknown = 0;


	toplist_file = fopen(filename, "r");

	if (toplist_file == NULL) {
		exit(EXIT_FAILURE);
	}

	while ((read = getline(&line, &len, toplist_file)) != -1) {
		if (read == -1) {
			perror("unable to read line from file");
		}
		line[read-1] = '\0'; /* null-terminate the URL */
		if (URL_UNKNOWN == (res = check_url(line))) {
			mmappedData->unknown = mmappedData->unknown + 1;
		}
		else if(res == URL_ERROR){
			printf("Illegal url detected, exiting now\n");
			exit(0);
		}
		else {
			mmappedData->sum = mmappedData->sum + res;
			mmappedData->amount = mmappedData->amount + 1;
		}
	}

	free(line);
	fclose(toplist_file);



	if(mmappedData->amount > 0){ 
		printf("%.4f Average response time from %d sites, %d Unknown\n",
					mmappedData->sum / mmappedData->amount,
					mmappedData->amount,
					mmappedData->unknown);
	}
	else{
		printf("No Average response time from 0 sites, %d Unknown\n", results.unknown);
	}
	if((rc = munmap(mmappedData, sizeof(ResultStruct))) != 0) // משחרר את הזיכרון  שהגדיר
	{
		perror("unable to unmapping");
		exit(EXIT_FAILURE);
	}
}

/**
 * @define - handle single worker that run on child process
 */
void worker_mmap_checker(int worker_id, int num_of_workers, const char *filename, ResultStruct *mmappedData) {
	/*
	 * TODO: this checker function should operate almost like serial_checker(), except:
	 * 1. Only processing a distinct subset of the lines (hint: think Modulo)
	 * 2. Writing the results back to the parent using the mmap (i.e. and not to the screen)
 	 * 3. If an URL_ERROR returned, all processes (parent and children) should exit immediately and an error message should be printed (as in 'serial_checker')
	 */

	ResultStruct results = {0};

	double res;
	FILE *toplist_file;
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	int line_number = 0, cd;

	// TODO
	// open file for read only
	toplist_file = fopen(filename, "r");

	// validate file open successfully
	if (toplist_file == NULL) {
		exit(EXIT_FAILURE);
	}

	//use the named semaphore
	sem_t *sem = sem_open(SNAME, 0);

	// go over all the lines
	while ((read = getline(&line, &len, toplist_file)) != -1) {
	    if(worker_id == (line_number % num_of_workers)) {
	        line[read-1] = '\0'; /* null-terminate the URL */ // מכניס \0 לתו האחרון כדי שידע שנגמר המרחוזת. מפצל את הקובץ לשורה שורה
        	sem_wait(sem);
        	if (URL_UNKNOWN == (res = check_url(line))) { // check_url(line)) בודק אם הURL תקין
        		mmappedData->unknown = mmappedData->unknown + 1;
        	}
       		else if(res == URL_ERROR){
       			printf("Illegal url detected, exiting now\n");
       			sem_post(sem);
       			kill(0, EXIT_SIGNAL);
       			exit(EXIT_FAILURE);
       		}
       		else { // אם הצליח מעלה את הזמן שלקח לURL .
        		// מעלה את הURL שהצליח
       			mmappedData->sum = mmappedData->sum + res;
       			mmappedData->amount = mmappedData->amount + 1;
       		}

       		sem_post(sem);
	    }

	    line_number++;
	}

	sem_wait(sem); //use semaphore to write in the critical section
	// TODO write the result to mapping
	msync(mmappedData, sizeof(ResultStruct), MS_SYNC);
	sem_post(sem);

	// close the resources
	free(line);
	fclose(toplist_file);

}

/**
 * Handle separate the work between process and merge the results
 */
void parallel_mmap_checker(int num_of_processes, const char *filename) {
	ResultStruct *mmappedData;
	ResultStruct results = {0};
	int worker_id, fd, rc;

	// TODO initialize  mapping
	if((mmappedData = mmap(NULL, sizeof(ResultStruct), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0)) == MAP_FAILED) // מקצים זיכרון בגדול  ResultStruct
   	{
   		perror("unable to create mapping");
   		exit(EXIT_FAILURE);
    }

	//initailize semaphore for the childern to write in the same file
	sem_t *sem = sem_open(SNAME, O_CREAT, 0644, 1);

	signal(EXIT_SIGNAL, exit_signal_handler);

	// Start num_of_processes new workers
	for (worker_id = 0; worker_id  < num_of_processes; ++worker_id ) {
		// TODO - fork the children and call worker_mmap_checker.
		// Possible implementation: Let worker_mmap_checker on which rows to perform work (from file).
		// each worker is working in different process
        if (fork() == 0) {
            worker_mmap_checker(worker_id, num_of_processes, filename, mmappedData);
            exit(0);
        }
	}

	// TODO
	// pending all child process
    while ((wait(NULL)) > 0);

	// TODO
	// get results

	// print the total results
	if(mmappedData->amount > 0){
		printf("%.4f Average response time from %d sites, %d Unknown\n",
				mmappedData->sum / mmappedData->amount,
				mmappedData->amount,
				mmappedData->unknown);
	}
	else{
		printf("No Average response time from 0 sites, %d Unknown\n", results.unknown);
	}

	if((rc = munmap(mmappedData, sizeof(ResultStruct))) != 0) // משחרר את הזיכרון  שהגדיר
   	{
   		perror("unable to unmapping");
   		exit(EXIT_FAILURE);
    }
}


void worker_pipe_checker(int worker_id, int num_of_workers, const char *filename, int pipe_write_fd) {
	/*
	 * TODO: this checker function should operate almost like serial_checker(), except:
	 * 1. Only processing a distinct subset of the lines (hint: think Modulo)
	 * 2. Writing the results back to the parent using the pipe_write_fd (i.e. and not to the screen)
	 * 3. If an URL_ERROR returned, all processes (parent and children) should exit immediatly and an error message should be printed (as in 'serial_checker')
	 */

	ResultStruct results = {0};

	double res;
	FILE *toplist_file;
	char *line = NULL;
	size_t len = 0;
	ssize_t read;
	int line_number = 0;

	// TODO
	results.unknown = 0;
	results.sum = 0;
	results.amount = 0;

	toplist_file = fopen(filename, "r");

	// go over all the lines
	while ((read = getline(&line, &len, toplist_file)) != -1) {

		// TODO
		if(worker_id == (line_number % num_of_workers)) {
            line[read-1] = '\0'; /* null-terminate the URL */ // מכניס \0 לתו האחרון כדי שידע שנגמר המרחוזת. מפצל את הקובץ לשורה שורה
            if (URL_UNKNOWN == (res = check_url(line))) { // check_url(line)) בודק אם הURL תקין
                results.unknown = results.unknown + 1;
            }
            else if(res == URL_ERROR){
                kill(0, EXIT_SIGNAL);
                exit(EXIT_FAILURE);
            }
            else { // אם הצליח מעלה את הזמן שלקח לURL .
                // מעלה את הURL שהצליח
                results.sum = results.sum + res;
                results.amount = results.amount + 1;
           }
        }

        line_number++;
    }

	// TODO
    free(line); // משחרר את הזיכרון
    fclose(toplist_file); // סוגר את הקובץ

    if(write(pipe_write_fd, &results, sizeof(results)) == -1){
    	printf("Unable to write exiting...\n");
    	kill(0, EXIT_SIGNAL);
        exit(EXIT_FAILURE);
    }

    close(pipe_write_fd);
}

/**
 * Handle separate the work between process and merge the results
 */
void parallel_pipe_checker(int num_of_processes, const char *filename) {
	int worker_id;
	int pipefd[2];

	ResultStruct results = {0};
	ResultStruct results_buffer = {0};

	// initialize  pipe
	pipe(pipefd);

    signal(EXIT_SIGNAL, exit_signal_handler);

	// Start num_of_processes new workers
	for (worker_id = 0; worker_id  < num_of_processes; ++worker_id ) {

		// TODO - fork the children and call worker_pipe_checker.
		// Possible implementation: Let worker_pipe_checker on which rows to perform work (from file).
		if (fork() == 0) {
            worker_pipe_checker(worker_id, num_of_processes, filename, pipefd[1]);
            exit(EXIT_SUCCESS);
        }
	}

	// TODO
	while ((wait(NULL)) > 0);

	for (worker_id = 0; worker_id  < num_of_processes; ++worker_id ) {

		// TODO - sum the results
	    if(read(pipefd[0], &results_buffer, sizeof(ResultStruct)) < 0){
            printf("Failed to read, exiting...");\
            exit(EXIT_FAILURE);
       	}
        else {
    		results.sum = results.sum + results_buffer.sum;
       		results.amount = results.amount + results_buffer.amount;
        	results.unknown = results.unknown + results_buffer.unknown;
        }
	}

	close(pipefd[0]);

	// print the total results
	if(results.amount > 0){
		printf("%.4f Average response time from %d sites, %d Unknown\n",
						results.sum / results.amount,
						results.amount,
						results.unknown);
	}
	else{
		printf("No Average response time from 0 sites, %d Unknown\n", results.unknown);
	}


}


int main(int argc, char **argv) {
	int pipe_flag = 0;
		if(argc == 4 && !strcmp(argv[3],"-f"))
		{
			pipe_flag = 1;
		}
		if (argc != 3 && !pipe_flag) {
			usage();
		} else if (atoi(argv[1]) == 1) {
			serial_checker(argv[2]);
		} else
		{
			if(pipe_flag)
			{
				parallel_pipe_checker(atoi(argv[1]), argv[2]);
			}
			else
			{
				parallel_mmap_checker(atoi(argv[1]), argv[2]);
			}
		}
		return EXIT_SUCCESS;

}
