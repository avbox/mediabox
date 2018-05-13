/**
 * avbox - Toolkit for Embedded Multimedia Applications
 * Copyright (C) 2016-2018 Fernando Rodriguez
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License Version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */


#ifndef __INPUT_WEB_H__
#define __INPUT_WEB_H__

/**
 * Initialize the webinput driver.
 */
int
avbox_webinput_init(void);


/**
 * Shutdown the webinput driver
 */
void
avbox_webinput_shutdown(void);


#endif
