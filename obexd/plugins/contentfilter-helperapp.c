/*
 *  Plugin to accept or reject incoming content programmatically
 *
 *  Copyright (C) 2014  Jolla Ltd.
 *
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <glib.h>

#include "log.h"
#include "contentfilter.h"

/* Helper app returns success (0) if the operation is acceptable,
 * failure (1) otherwise. Command line arguments are as follows:
 *
 * --receive-file <filename> -- check file reception
 */
#define HELPER "/usr/libexec/obexd-contentfilter-helperapp"

int contentfilter_init(void)
{
	DBG("");
	return 0;
}

void contentfilter_exit(void)
{
	DBG("");
}

/*
 * Execute the external helper application to determine whether file
 * should be received or not. In the absence of the helper application
 * behave as the dummy filter and accept anything.
 */
gboolean contentfilter_receive_file(const char *filename)
{
	pid_t p;

	DBG("Checking '%s'", filename);

	/* No helper to determine status -- revert to accepting everything */
	if (access(HELPER, F_OK) < 0 && errno == ENOENT) {
		DBG("No helper, accepting.");
		return TRUE;
	}

	p = fork();

	if (p < 0) { /* fail */
		DBG("fork failed.");
		return FALSE;
	} else if (p > 0) { /* parent */
		int status;
		pid_t q = waitpid(p, &status, 0);
		if (q == p && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
			DBG("'%s' accepted.", filename);
			return TRUE; /* check succeeded */
		}
		DBG("'%s' rejected.", filename);
		return FALSE; /* check failed */
	} else { /* child */
		if (execl(HELPER,
				HELPER, "--receive-file", filename,
				(char *)NULL) < 0) {
			DBG("exec() failed, %s (%d).", strerror(errno), errno);
			exit(EXIT_FAILURE);
		}
		return FALSE; /* not reached, just keep gcc happy */
	}
}
