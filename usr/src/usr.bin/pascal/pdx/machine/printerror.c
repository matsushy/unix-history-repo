/* Copyright (c) 1982 Regents of the University of California */

static char sccsid[] = "@(#)printerror.c 1.1 %G%";

/*
 * print out an execution time error
 *
 * Have to check if the -r option was specified; if so then
 * the object file information hasn't been read in yet.
 */

#include "defs.h"
#include <signal.h>
#include "machine.h"
#include "main.h"
#include "sym.h"
#include "process.h"
#include "source.h"
#include "object.h"
#include "mappings.h"
#include "pxerrors.h"
#include "process/process.rep"

printerror()
{
	register PROCESS *p;
	char *filename;
	int c;

	p = process;
	if (p->signo != ESIGNAL && p->signo != SIGINT) {
		error("signal %d at px pc %d, lc %d", p->signo, p->pc, pc);
	}
	if (option('r')) {
		init();
	}
	curline = srcline(pc);
	curfunc = whatblock(pc);
	if ((filename = srcfilename(pc)) != cursource) {
		skimsource(filename);
	}
	if (p->signo == ESIGNAL) {
		printf("\nerror at line %d", curline);
		if (nlhdr.nfiles > 1) {
			printf("in file %s", cursource);
		}
		if (errnum != 0) {
			printf(":  %s", pxerrmsg[errnum]);
		}
	} else {
		printf("\n\ninterrupt at line %d", curline);
		if (nlhdr.nfiles > 1) {
			printf(" in file %s", cursource);
		}
	}
	putchar('\n');
	printlines(curline, curline);
	erecover();
}
