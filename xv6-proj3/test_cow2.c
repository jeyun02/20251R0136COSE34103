#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char **argv)
{
	int before, after;
	int pid;

	printf(1, "TEST2: ");
	
	before = freemem();

	pid = fork();
	if(pid == 0){
		exit();
	}
	else{
		wait();
	}

	after = freemem();
	if(before == after)
		printf(1, "OK\n");
	else
		printf(1, "WRONG\n before: %d, after: %d\n", before, after);

	exit();
}
