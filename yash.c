#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <readline/readline.h>
#include <readline/history.h>

#define MAX_ARGS 100
#define MAX_CHARS 200
#define RUNNING "Running"
#define STOPPED "Stopped"
#define DONE "Done"

typedef struct process {
	pid_t pid;
	char **command;
	char *inputFilePath;
	char *outputFilePath;
} Process;

typedef struct job
{
	char *jstr;
	pid_t jid;
	pid_t pgid;
	char *state;
	bool isBackgroundJob;
	int numProcesses;
	Process* jobProcesses[2];
	struct job *nextJob;
} Job;


Job *headJob;
int shell_terminal = -1;
pid_t shell_process_group;
int pipefd[2];
pid_t pid_ch1, pid_ch2;

static void sig_int_default(int signo);
static void sig_tstp_default(int signo);
static void sig_int(int signo);
static void sig_tstp(int signo);
static void sig_chld(int signo);
void kill_all_jobs();
void initialize();
void bg_handler();
void fg_handler();
void jobs_handler();
void add_to_job_control(Job *newJob);
void completed_jobs_handler();
pid_t get_next_job_number();
void new_job_handler(char *inputString);
bool invalid_input(char *a);
void wait_job_finish(Job *newJob, bool isNewJob);
void execute_job_no_pipe(Job *newJob);
void execute_job_with_pipe(Job *newJob);
void run_process(Process *currProcess, bool forPipedJob, bool leftProcess, pid_t newPid);


static void sig_int_default(int signo) {
	printf("\n# ");
}

static void sig_tstp_default(int signo) {
	printf("\n# ");
}

static void sig_int(int signo) {
  kill(-pid_ch1,SIGINT);
}
static void sig_tstp(int signo) {
  kill(-pid_ch1,SIGTSTP);
}

static void sig_chld(int signo) {
	int wstatus;
	pid_t cpid;
	while((cpid = waitpid(-1, &wstatus, WNOHANG)) > 1) { //the WNOHANG is specifically for background processes
		if (WIFSIGNALED(wstatus) || WIFEXITED(wstatus)) {
			if (headJob != NULL) {
				Job *currJob = headJob;
				while (currJob != NULL) {
					if (currJob->pgid == cpid) {
						currJob->state = DONE;
						break;
					}
					currJob = currJob->nextJob;
				}
			}
		}
	}
}

void kill_all_jobs() {
	if (headJob == NULL) {
		return;
	}
	Job *curr = headJob;
	Job *prev = NULL;
    while (curr != NULL) { //checking the head of the list
    	kill(-(curr->pgid),SIGKILL);
    	headJob = curr->nextJob;
    	free(curr);
    	curr = headJob;
    }
}


void initialize() {
	signal(SIGINT, sig_int_default);
	signal(SIGTSTP, sig_tstp_default);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGCHLD, sig_chld);

	shell_process_group = getpid();
	setpgid(shell_process_group, shell_process_group); // putting shell pid into process group

	shell_terminal = STDIN_FILENO;
	tcsetpgrp (shell_terminal, shell_process_group);
}

void bg_handler() {//sends most recent process to background

	//find most recent stopped process group
	if (headJob == NULL)
		return;
	Job *mostRecentlyStopped = NULL;
	Job *traverseList = headJob;
	while (traverseList != NULL) {
		if ((!strcmp(traverseList->state,STOPPED))) {
			mostRecentlyStopped = traverseList;
		}
		traverseList = traverseList->nextJob;
	}
	if (mostRecentlyStopped == NULL) {
		return;
	}

	//add " &" to job name
	char *newJstr = malloc((strlen(mostRecentlyStopped->jstr) + 3) * sizeof(char));
	strcpy(newJstr, mostRecentlyStopped->jstr);
	strcat(newJstr, " &");
	strcpy(mostRecentlyStopped->jstr, newJstr);
	free(newJstr);

	//print job
	printf("[%d]+ %s\n", mostRecentlyStopped->jid, mostRecentlyStopped->jstr);

	//update job
	mostRecentlyStopped->state = RUNNING; 
	mostRecentlyStopped->isBackgroundJob = true;

	//send SIGCONT and wait job (this will be WNOHANG)
	if (kill((-1)*(mostRecentlyStopped->pgid), SIGCONT) == -1) {
		perror("sigcont bg handler");
		exit(EXIT_FAILURE);
	}
	wait_job_finish(mostRecentlyStopped, false);
}

void fg_handler() { //bring most recent process to foreground

	//find most recent background/stopped process group
	if (headJob == NULL) {
		return;
	}
	Job *mostRecentJob = NULL;
	Job *traverseList = headJob;
	while (traverseList != NULL) {
		if (traverseList->isBackgroundJob || (!strcmp(traverseList->state,STOPPED))) {
			mostRecentJob = traverseList;
		}
		traverseList = traverseList->nextJob;
	}
	if (mostRecentJob == NULL) {
		return;
	}

	//remove " &" from string	
	int size_jsr = strlen(mostRecentJob->jstr);
	if (mostRecentJob->jstr[size_jsr - 1] == '&') {
   		mostRecentJob->jstr[size_jsr - 1] = '\0';
   		mostRecentJob->jstr[size_jsr - 2] = '\0';
	}

	//print job
	printf("%s\n", mostRecentJob->jstr);

	//update job
	mostRecentJob->state = RUNNING; //in case it was stopped
	mostRecentJob->isBackgroundJob = false;	

	pid_ch1 = mostRecentJob->jobProcesses[0]->pid;
	if (mostRecentJob->numProcesses == 2) {
		pid_ch2 = mostRecentJob->jobProcesses[1]->pid;	
	}

	//send SIGCONT and wait job
	if (kill(-mostRecentJob->pgid, SIGCONT) == -1) {
		perror("sigcont fg handler");
	  	exit(EXIT_FAILURE);
	}
	wait_job_finish(mostRecentJob, false);
}

void jobs_handler() {

	if (headJob == NULL) {
		return;
	}

	Job *curr = headJob;
	while (curr != NULL) {

		char sign = '-';
		if (curr->nextJob == NULL) {//the most recent process
			sign = '+';
		}

		printf("[%d]%c %s    %s\n", curr->jid, sign, curr->state, curr->jstr);

		curr = curr -> nextJob;
	}
}

void add_to_job_control(Job *newJob) {
	newJob->jid = get_next_job_number();

	if (headJob == NULL) {
		headJob = newJob;
		return;
	}

	Job *listTraverse = headJob;
	while (listTraverse->nextJob != NULL)
		listTraverse = listTraverse -> nextJob;

	listTraverse->nextJob = newJob;
}

void wait_job_finish(Job *runningJob, bool isNewJob) {
	int wstatus;
	pid_t w;

	if (runningJob->isBackgroundJob) {
		w = waitpid(-pid_ch1, &wstatus, WNOHANG);
		return;
	}

	signal(SIGTTOU, SIG_IGN);
	tcsetpgrp (shell_terminal, runningJob->pgid);

	if(signal(SIGINT, sig_int) == SIG_ERR) {
		perror("signal(SIGINT) error");
	}
	if (signal(SIGTSTP, sig_tstp) == SIG_ERR) {
		perror("signal(SIGTSTP) error");
	}

	

	int count = 0;
	bool matchesPid = false;

	while(!strcmp(runningJob->state, RUNNING)) {
		w = waitpid(-1, &wstatus, WUNTRACED|WCONTINUED);

		if (w == -1) {
    	  perror("waitpid");
    	  exit(EXIT_FAILURE);
    	}

    	if (runningJob->numProcesses == 2) {
    		matchesPid = ((w == pid_ch1) || (w == pid_ch2));
    	} 

    	else {
    		matchesPid = (w == pid_ch1);
    	}

    	if (matchesPid) {
    		if (WIFSIGNALED(wstatus)) {
    			runningJob->state = DONE;
    		}
    		else if (WIFEXITED(wstatus)) {
    			count ++;
    			if (count == runningJob->numProcesses) {
					runningJob->state = DONE;
    			}
	    	} else if (WIFSTOPPED(wstatus)) {
    			runningJob->state = STOPPED;
				if (isNewJob) {
					add_to_job_control(runningJob);
				}
    		}
    	}
	}

	signal(SIGINT, sig_int_default);
	signal(SIGTSTP, sig_tstp_default);
	tcsetpgrp(shell_terminal, shell_process_group); //assigning fg to shell again
}


void run_process(Process *currProcess, bool forPipedJob, bool leftProcess, pid_t newPid) {
	setpgid(0, newPid);

	signal(SIGINT,SIG_DFL);
	signal(SIGTSTP,SIG_DFL);
	signal(SIGTTIN,SIG_DFL);

	if (forPipedJob) {
		if (leftProcess) {
			close(pipefd[0]); //close read end
			dup2(pipefd[1], STDOUT_FILENO);
		} else {
			close(pipefd[1]); // close the write end	
			dup2(pipefd[0], STDIN_FILENO);
		}
 	}

	if (strlen(currProcess->inputFilePath) > 0) {
		int ifd = open(currProcess->inputFilePath, O_RDONLY);
		if (ifd < 0) {
			perror("error with input file");
	  		exit(EXIT_FAILURE);
		}
		dup2(ifd, STDIN_FILENO);
	}

	if(strlen(currProcess->outputFilePath) > 0) {
		int ofd = open(currProcess->outputFilePath, O_CREAT|O_WRONLY|O_TRUNC, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH);
		if (ofd < 0) {
			perror("error with output file");
			exit(EXIT_FAILURE);	
		}
		dup2(ofd, STDOUT_FILENO);
	}

	execvp(currProcess->command[0], currProcess->command);

	perror ("execvp error\n");
  	exit (EXIT_FAILURE);
}

void execute_job_no_pipe(Job *newJob) {
	int wstatus;
	pid_t w;
	pid_ch1 = fork();
	if (pid_ch1 < 0) {
		perror("fork");
		exit(EXIT_FAILURE);
	}

	if (pid_ch1 > 0) { //parent
		newJob->jobProcesses[0]->pid = pid_ch1;
		newJob->pgid = pid_ch1;
		wait_job_finish(newJob, true);
	} else { //child
		run_process(newJob->jobProcesses[0], false, true, 0);
	}
}

void execute_job_with_pipe(Job *newJob) {
	int wstatus;
	pid_t w;

	if (pipe(pipefd) == -1) {
		perror("pipe");
		exit(EXIT_FAILURE);
    }

    pid_ch1 = fork();
    if (pid_ch1 < 0) {
    	perror("fork");
    	exit(EXIT_FAILURE);
    }
    if (pid_ch1 > 0){
    	pid_ch2 = fork();
    	if (pid_ch2 < 0) {
    		perror("fork");
    		exit(EXIT_FAILURE);
    	}
    	if (pid_ch2 > 0) { //parent
    		newJob->jobProcesses[0]->pid = pid_ch1;
    		newJob->jobProcesses[1]->pid = pid_ch2;
    		close(pipefd[0]);
      		close(pipefd[1]);
    		newJob->pgid = pid_ch1;
      		wait_job_finish(newJob, true);
    	} else { //child 2
    		sleep(1);
    		run_process(newJob->jobProcesses[1], true, false, pid_ch1);
    	}
    } else { //child 1
    	run_process(newJob->jobProcesses[0], true, true, 0);
    }
}

void completed_jobs_handler() {
	if (headJob == NULL) {
		return;
	}

	Job *curr = headJob;
	Job *prev = NULL;

    while (curr != NULL && (!strcmp(curr->state, DONE))) { //checking the head of the list
    	headJob = curr->nextJob;

    	if (curr->isBackgroundJob) {
			printf("[%d]- Done    %s\n", curr->jid, curr->jstr);
    	}

    	free(curr);
    	curr = headJob;
    }

    while (curr != NULL) {
    	if ((!strcmp(curr->state, DONE))) {
    		prev->nextJob = curr->nextJob;

    		if (curr->isBackgroundJob) {
				printf("[%d]- Done    %s\n", curr->jid, curr->jstr);
    		}

    		free(curr);
    	} else
    		prev = curr;
    	curr = prev->nextJob;
    }
}

int main(int argc, char *argv[]) {

	initialize();

	while (1)
	{
		char *userInput = readline("# ");

		// exit shell for CTRL+D
		if (userInput == NULL)
		{
			kill_all_jobs();
			break;
		}

		size_t userInputLength = strlen(userInput);
		if (userInput[userInputLength - 1] == '\n') {
			userInput[userInputLength - 1] == '\0';
		}

		completed_jobs_handler();

		if (strlen(userInput) == 0) { //user just pressed enter
			continue;
		}

		if (strcmp(userInput, "bg") == 0) {
			bg_handler();
		}

		else if (strcmp(userInput, "fg") == 0) {
			fg_handler();
		}

		else if (strcmp(userInput, "jobs") == 0) {
			jobs_handler();
		}

		else if (!invalid_input(userInput)) {
			new_job_handler(userInput);
		}
	}
}


pid_t get_next_job_number() {
	if (headJob == NULL) {
		return 1;
	}

	pid_t highestJobNumber = -1;
	Job *listTraverse = headJob;
	while (listTraverse != NULL) {
		if (listTraverse->jid > highestJobNumber) {
			highestJobNumber = listTraverse->jid;
		}
		listTraverse = listTraverse -> nextJob;
	}

	return highestJobNumber + 1;
}


bool invalid_input(char *a) {

	return (a[0] == '|') || (a[0] == '<') || (a[0] == '>') || (a[0] == '&'); 
}

void new_job_handler(char *inputString) {
	Job *newJob = (Job *)malloc(sizeof(Job));
	newJob->jstr = strdup(inputString);
	newJob->jid = -1;
	newJob->pgid = -1; //will change later
	newJob->state = RUNNING;
	newJob->isBackgroundJob = false;
	newJob->numProcesses = 1;
	newJob->nextJob = NULL;

	newJob->jobProcesses[0] = (Process *)malloc(sizeof(Process));
	newJob->jobProcesses[0]->command = malloc(MAX_ARGS * sizeof(char *));
	newJob->jobProcesses[0]->inputFilePath = malloc(MAX_CHARS * sizeof(char));
	newJob->jobProcesses[0]->outputFilePath = malloc(MAX_CHARS * sizeof(char));
	newJob->jobProcesses[0]->pid = -1;

	newJob->jobProcesses[1] = NULL;

	char *str1, *str2, *token, *subtoken;
	char *saveptr1, *saveptr2;
	int commandNumber;

	int leftCommandIndex = 0;
	int rightCommandIndex = 0;

	bool inputFileFlag = false;
	bool outputFileFlag = false;

	for (commandNumber = 0, str1 = inputString;; commandNumber++, str1 = NULL) {
		token = strtok_r(str1, "|", &saveptr1);
		if (token == NULL) {
			break;
		}

		if (commandNumber > 0) { //more than 1 commands, so there is a pipe
			newJob->numProcesses = 2;

			newJob->jobProcesses[1] = (Process *)malloc(sizeof(Process));
			newJob->jobProcesses[1]->command = malloc(MAX_ARGS * sizeof(char *));
			newJob->jobProcesses[1]->inputFilePath = malloc(MAX_CHARS * sizeof(char));
			newJob->jobProcesses[1]->outputFilePath = malloc(MAX_CHARS * sizeof(char));
			newJob->jobProcesses[1]->pid = -1;
		}

		for (str2 = token;; str2 = NULL)
		{
			subtoken = strtok_r(str2, " ", &saveptr2);
			if (subtoken == NULL) {
				break;
			}

			if (inputFileFlag) {
				strcpy(newJob->jobProcesses[commandNumber]->inputFilePath, subtoken);
				inputFileFlag = false;
				continue;
			}

			else if (outputFileFlag) {
				strcpy(newJob->jobProcesses[commandNumber]->outputFilePath, subtoken);
				outputFileFlag = false;
				continue;
			}

			if (strcmp(subtoken, "&") == 0) {
				newJob->isBackgroundJob = true;
			}

			else if (strcmp(subtoken, "<") == 0) {
				inputFileFlag = true; // this indicates that, in the next iteration, the token will be the input file
			}

		 	else if (strcmp(subtoken, ">") == 0) {
		 		outputFileFlag = true; // this indicates that, in the next iteration, the token will be the output file  
		 	}

			else {
				if (commandNumber == 0) {
					newJob->jobProcesses[commandNumber]->command[leftCommandIndex] = subtoken;
					leftCommandIndex ++;
				}
				else {
					newJob->jobProcesses[commandNumber]->command[rightCommandIndex] = subtoken;
					rightCommandIndex ++;
				}
			}
		}
	}

	newJob->jobProcesses[0]->command[leftCommandIndex] = NULL;

	if (newJob->numProcesses == 2) {
		newJob->jobProcesses[1]->command[rightCommandIndex] = NULL;
	}

	if (newJob->isBackgroundJob) {
		add_to_job_control(newJob);
	}

	if (newJob->numProcesses == 2) { //user string contained pipe
		execute_job_with_pipe(newJob);
	}
	else {
		execute_job_no_pipe(newJob);
	}
}
