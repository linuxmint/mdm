/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*-
 *
 * MDM - The MDM Display Manager
 * Copyright (C) 1998, 1999, 2000 Martin K. Petersen <mkp@mkp.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifndef MDM_H
#define MDM_H

#define MDM_MAX_PASS 256	/* Define a value for password length. Glibc
				 * leaves MAX_PASS undefined. */

/* DO NOTE USE 1, that's used as error if x connection fails usually */
/* Note that there is no reason why these were a power of two, and note
 * that they have to fit in 256 */
/* These are the exit codes */
#define DISPLAY_REMANAGE 2	/* Restart display */
#define DISPLAY_ABORT 4		/* Houston, we have a problem */
#define DISPLAY_REBOOT 8	/* Rebewt */
#define DISPLAY_HALT 16		/* Halt */
#define DISPLAY_SUSPEND 17	/* Suspend (don't use, use the interrupt) */
#define DISPLAY_XFAILED 64	/* X failed */
#define DISPLAY_GREETERFAILED 65 /* greeter failed (crashed) */
#define DISPLAY_RESTARTGREETER 127 /* Restart greeter */
#define DISPLAY_RESTARTMDM 128	/* Restart MDM */

enum {
	DISPLAY_UNBORN /* Not yet started */,
	DISPLAY_ALIVE /* Yay! we're alive */,
	DISPLAY_DEAD /* Left for dead */,
	DISPLAY_CONFIG /* in process of being configured */
};

/* The dreaded miscellaneous category */
#define PIPE_SIZE 4096

#define MDM_SESSION_AUTO "auto"

#define MDM_STANDARD "Standard"

#define MDM_RESPONSE_CANCEL "MDM_RESPONSE_CANCEL"

/* If id == NULL, then get the first X server */
void		mdm_final_cleanup	(void);

#endif /* MDM_H */

/* EOF */
