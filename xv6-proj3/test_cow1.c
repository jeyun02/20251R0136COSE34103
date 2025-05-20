#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char **argv)
{
	int before, after;
	int pid;

	printf(1, "TEST1: ");
	
	before = freemem();

	pid = fork();
	if(pid == 0){
		after = freemem();
		if(before - after == 68)
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
