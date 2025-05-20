#include "types.h"
#include "stat.h"
#include "user.h"

int global = 3;

int
main(int argc, char **argv)
{
	int before, after;
	int pid;

	printf(1, "TEST3: ");
	
	pid = fork();
	if(pid == 0){
		before = freemem();
		global = 4;
		after = freemem();
		if(before - after == 1)
			printf(1, "OK\n");
		else
			printf(1, "WRONG\n");
		exit();
	}
	else{
		wait();
	}

	exit();
}
