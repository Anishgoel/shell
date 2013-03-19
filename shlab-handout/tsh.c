/* 
 * tsh - A tiny shell program with job control
 * 
 * Team Name: atb46 + jv325
 * Austin Blakeslee: atb46
 * John Van Note: jv325
 *
 * Baddest guys
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
	pid_t pid;              /* job PID */
	int jid;                /* job ID [1, 2, ...] */
	int state;              /* UNDEF, BG, FG, or ST */
	char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

/* A few of these we added*/
pid_t Fork(void);
void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
	char c;
	char cmdline[MAXLINE];
	int emit_prompt = 1; /* emit prompt (default) */

	/* Redirect stderr to stdout (so that driver will get all output
	 * on the pipe connected to stdout) */
	dup2(1, 2);

	/* Parse the command line */
	while ((c = getopt(argc, argv, "hvp")) != EOF) {
		switch (c) {
			case 'h':             /* print help message */
				usage();
				break;
			case 'v':             /* emit additional diagnostic info */
				verbose = 1;
				break;
			case 'p':             /* don't print a prompt */
				emit_prompt = 0;  /* handy for automatic testing */
				break;
			default:
				usage();
		}
	}

	/* Install the signal handlers */

	/* These are the ones you will need to implement */
	Signal(SIGINT,  sigint_handler);   /* ctrl-c */
	Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
	Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

	/* This one provides a clean way to kill the shell */
	Signal(SIGQUIT, sigquit_handler); 

	/* Initialize the job list */
	initjobs(jobs);

	/* Execute the shell's read/eval loop */
	while (1) {

		/* Read command line */
		if (emit_prompt) {
			printf("%s", prompt);
			fflush(stdout);
		}
		if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
			app_error("fgets error");
		if (feof(stdin)) { /* End of file (ctrl-d) */
			fflush(stdout);
			exit(0);
		}

		/* Evaluate the command line */
		eval(cmdline);
		fflush(stdout);
		fflush(stdout);
	} 

	exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
void eval(char *cmdline) 
{
	char* argv[MAXARGS];
	char buf[MAXLINE];
	int bg;
	pid_t pid;
	//copying the passed in arg to a buffer
	strcpy(buf,cmdline);
	//Parsing the line
	bg = parseline(buf,argv);
	//No args exist
	if(argv[0] == NULL) {
		return;
	}
	//Checks if the passed argument is built in
	if(!builtin_cmd(argv)) {
		sigset_t mask;
		if(sigemptyset(&mask) < 0) {
			fprintf(stderr,"Failed to create empty set.\n");
			exit(0);
		}
		if(sigaddset(&mask,SIGCHLD) < 0) {
			fprintf(stderr,"Failed to add SIGCHLD to set.\n");
			exit(0);
		}
		if(sigprocmask(SIG_BLOCK,&mask,NULL) < 0) {
			fprintf(stderr,"Failed to block SIGCHLD.\n");
			exit(0);
		}
		//Forking and executing
		if((pid = Fork()) == 0) {
			if(setpgid(0,0) < 0) {
				fprintf(stderr,"Failed to create process group.\n");
				exit(0);
			}
			if(sigprocmask(SIG_UNBLOCK,&mask,NULL) < 0) {
				fprintf(stderr,"Failed to unblock SIGCHLD.\n");
				exit(0);
			}
			if(execve(argv[0],argv,environ) < 0) {
				printf("%s: Command not found.\n",argv[0]);
				exit(1);
			}
		}
		if(!bg) {
			//Not a background job
			if(!addjob(jobs, pid, FG, cmdline)) {
				printf("Failed to add job.\n");
			}
			//int status;
			if(sigprocmask(SIG_UNBLOCK,&mask,NULL) < 0) {
				fprintf(stderr,"Failed to unblock SIGCHLD.\n");
				exit(0);
			}
			//Need to implement waitfg
			waitfg(pid);
		}
		else {
			//Background job
			if(!addjob(jobs,pid,BG,cmdline)) {
				printf("Failed to add job.\n");
			}
			sigprocmask(SIG_UNBLOCK,&mask,NULL);
			printf("%d %s\n", pid, cmdline);
		}
	}
	return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
	static char array[MAXLINE]; /* holds local copy of command line */
	char *buf = array;          /* ptr that traverses command line */
	char *delim;                /* points to first space delimiter */
	int argc;                   /* number of args */
	int bg;                     /* background job? */

	strcpy(buf, cmdline);
	buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
	while (*buf && (*buf == ' ')) /* ignore leading spaces */
		buf++;

	/* Build the argv list */
	argc = 0;
	if (*buf == '\'') {
		buf++;
		delim = strchr(buf, '\'');
	}
	else {
		delim = strchr(buf, ' ');
	}

	while (delim) {
		argv[argc++] = buf;
		*delim = '\0';
		buf = delim + 1;
		while (*buf && (*buf == ' ')) /* ignore spaces */
			buf++;

		if (*buf == '\'') {
			buf++;
			delim = strchr(buf, '\'');
		}
		else {
			delim = strchr(buf, ' ');
		}
	}
	argv[argc] = NULL;

	if (argc == 0)  /* ignore blank line */
		return 1;

	/* should the job run in the background? */
	if ((bg = (*argv[argc-1] == '&')) != 0) {
		argv[--argc] = NULL;
	}
	return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{
	if(strcmp(argv[0],"quit") == 0) {
		exit(0);
	}
	else if(strcmp(argv[0],"jobs") == 0) {
		listjobs(jobs);
	}
	else if(strcmp(argv[0],"fg") == 0) {
		do_bgfg(argv);
	}
	else if(strcmp(argv[0],"bg") == 0) {
		do_bgfg(argv);
	}
	else if(strcmp(argv[0],"dome") == 0) {
		printf("Put the pussy in a sarcophagus\n");
	}
	else {
		return 0;
	}
	return 1;     /* not a builtin command */
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
	//pid_t pid;
	int jid;
	//int i = 0;
	
	//Check if there is an additional argument
	if(argv[1]) {
		//Check for valid argument
		if(argv[1][0] == '%') {
			//Getting Job ID
			jid = argv[1][1] - '0';
			if(getjobjid(jobs,jid) == 0) {
				fprintf(stderr,"Not a valid job.\n");
				return;
			}
		}
		else {
			//Getting Process ID
			if(atoi(argv[1]) == 0) {
				fprintf(stderr,"Invalid Parameters\n");
				return;
			}
			else {
				jid = atoi(argv[1]);
				if(getjobpid(jobs,jid) == NULL) {
					fprintf(stderr,"Not a valid job.\n");
					return;
				}
			}
		}
		//Loop through jobs
		int i;
		for(i = 0; i < MAXJOBS; i++) {
			//Check if we found the job were looking for
			if((jobs[i].pid == (pid_t)jid) || (jobs[i].jid == (int)jid)) {
				//Foreground Command
				if(strcmp(argv[0],"fg") == 0) {
					//Background Job Found
					if(jobs[i].state == BG) {
						if(kill(-jobs[i].pid,SIGTSTP) < 0) {
							fprintf(stderr,"SIGTSTP failed\n");
							return;
						}
						jobs[i].state = FG; //Put in foreground
						if(kill(-jobs[i].pid,SIGCONT) < 0) {
							fprintf(stderr,"SIGCONT failed\n");
							return;
						}
						waitfg(jobs[i].pid);
					}
					else {
						//Stopped Jobs
						if(jobs[i].state == ST) {
							jobs[i].state = FG; //Put in foreground
							if(kill(-jobs[i].pid,SIGCONT) < 0) {
								fprintf(stderr,"SIGCONT failed\n");
								return;
							}
							waitfg(jobs[i].pid);
						}
						else {
							//Job is already foreground
							if(jobs[i].state == FG) {
								jobs[i].state = FG;
								if(kill(-jobs[i].pid,SIGCONT) < 0) {
									fprintf(stderr,"SIGCONT failed\n");
									return;
								}
								waitfg(jobs[i].pid);
							}
						}
					}
					return;
				}
				//Background Command
				else if (strcmp(argv[0],"bg") == 0) {
					//Stopped Jobs
					if(jobs[i].state == ST) {
						jobs[i].state = BG; //Put in Background
						if(kill(-jobs[i].pid,SIGCONT) < 0) {
							fprintf(stderr,"SIGCONT failed\n");
							return;
						}
					}
					else {
						//Cannot Background running jobs
						if(jobs[i].state == FG) {
							fprintf(stderr,"Background cannot get foreground job\n");
							return;		
						}
					}
					return;
				}
				else {
					printf("Error\n");
				}
			}
		}
	}	
	return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
	int status = 0;
	struct job_t* job;
	//Getting job from passed in pid
	job = getjobpid(jobs, pid);
	//While function is completeing is the foreground, wait
	while(waitpid(pid, &status, WNOHANG) == 0) {
		if(job->state != FG) {
			sleep(1);
			return;
		}
	}
	return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
    int status = 0;
    pid_t pid;
    struct job_t job;
    
    while((pid = waitpid(-1, &status, WNOHANG|WUNTRACED)) > 0) {
        jobs = getjobpid(jobs, pid);
        /* child process terminated normally */
        if(WIFEXITED(status)) {
            deletejob(jobs, pid);
            fprintf(stdout, "Job deleted, Exit signal");
        }
        /* child process terminated because of a signal was not caught */
        else if(WIFSIGNALED(status)) {
            deletejob(jobs, pid);
            fprintf(stdout, "Job deleted, signal not caught");
        }
        /* child process currently stopped */
        else if(WIFSTOPPED(status)) {
            job->state = ST;
            fprintf(stdout, "Job stopped, ");
        }
        /* something fucked up */
        else {
            unix_error("waitpid error\n");
        }
    }
	return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
    int status = 0;
    pid_t pid;
    struct job_t job;
    
    pid = fgpid(jobs);
    if(pid > 0) {
        job = getjobpid(jobs, pid);
        int kstatus = kill(-pid, SIGTINT);
        if(kstatus < 0) {
            unix_error("sigint_handler error")
        }
        fprintf(stdout, "SIGINT caught");
    }
    
	return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
    int status = 0;
    pid_t pid;
    struct job_t job;
    
    pid = fgpid(jobs);
    if(pid > 0) {
        job = getjobpid(jobs, pid);
        int kstatus = kill(-pid, SIGTSTP);
        if(kstatus < 0) {
            unix_error("sigtstp_handler error")
        }
        fprintf(stdout, "SIGTSTP caught");
    }
    
	return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
	int i;

	for (i = 0; i < MAXJOBS; i++)
		clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
	int i, max=0;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid > max)
			max = jobs[i].jid;
	return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
			jobs[i].pid = pid;
			jobs[i].state = state;
			jobs[i].jid = nextjid++;
			if (nextjid > MAXJOBS)
				nextjid = 1;
			strcpy(jobs[i].cmdline, cmdline);
			if(verbose){
				printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
			}
			return 1;
		}
	}
	printf("Tried to create too many jobs\n");
	return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
	int i;

	if (pid < 1)
		return 0;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == pid) {
			clearjob(&jobs[i]);
			nextjid = maxjid(jobs)+1;
			return 1;
		}
	}
	return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
	int i;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].state == FG)
			return jobs[i].pid;
	return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
	int i;

	if (pid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return &jobs[i];
	return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
	int i;

	if (jid < 1)
		return NULL;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid)
			return &jobs[i];
	return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
	int i;

	if (pid < 1)
		return 0;
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid) {
			return jobs[i].jid;
		}
	return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
	int i;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid != 0) {
			printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
			switch (jobs[i].state) {
				case BG: 
					printf("Running ");
					break;
				case FG: 
					printf("Foreground ");
					break;
				case ST: 
					printf("Stopped ");
					break;
				default:
					printf("listjobs: Internal error: job[%d].state=%d ", 
							i, jobs[i].state);
			}
			printf("%s", jobs[i].cmdline);
		}
	}
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * Fork - fork() function wrapper to error check. From Book ch 8
 */
pid_t Fork(void) {
	pid_t pid;

	if((pid = fork()) < 0) {
		unix_error("Fork Error.");	
	}
	return pid;
}

/*
 * usage - print a help message
 */
void usage(void) 
{
	printf("Usage: shell [-hvp]\n");
	printf("   -h   print this message\n");
	printf("   -v   print additional diagnostic information\n");
	printf("   -p   do not emit a command prompt\n");
	exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
	fprintf(stdout, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
	fprintf(stdout, "%s\n", msg);
	exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
	struct sigaction action, old_action;

	action.sa_handler = handler;  
	sigemptyset(&action.sa_mask); /* block sigs of type being handled */
	action.sa_flags = SA_RESTART; /* restart syscalls if possible */

	if (sigaction(signum, &action, &old_action) < 0)
		unix_error("Signal error");
	return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
	printf("Terminating after receipt of SIGQUIT signal\n");
	exit(1);
}



