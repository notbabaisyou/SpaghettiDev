/**
* Spaghetti Display Server
 * Copyright (C) 2025  SpaghettiFork
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "meatball.h"

static LogToServerProc log_to_server;

void MBLog(enum MBLogType type, const char *format, ...)
{
	va_list args;
	va_start(args, format);

	/**
	 * If we have a handler for logging so that
	 * we can redirect all output to the server,
	 * take advantage of it.
	 */
	if (log_to_server)
	{
		log_to_server(type, format, args);
	}
	else
	{
		switch (type)
		{
			case MB_LOG_INFO:
			case MB_LOG_WARNING:
			{
				fprintf(stdout, format, args);
				break;
			}

			case MB_LOG_ERROR:
			case MB_LOG_DEBUG:
			{
				fprintf(stderr, format, args);
				break;
			}

			case MB_LOG_NONE: break;

			default:
			{
				abort(!"Unknown log type!");
			}
		}
	}

	va_end(args);
}

void InitLog(struct meatball_config *config)
{
	log_to_server = config->log_to_server;
}
