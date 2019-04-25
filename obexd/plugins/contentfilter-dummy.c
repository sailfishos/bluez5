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

#include <glib.h>
#include "contentfilter.h"

int contentfilter_init(void)
{
	return 0;
}

void contentfilter_exit(void)
{
}

gboolean contentfilter_receive_file(const char *filename)
{
	/* Dummy contentfilter accepts all */
	return TRUE;
}
