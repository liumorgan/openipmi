/*
 * ipmi_malloc.h
 *
 * MontaVista IPMI interface, internal memory allocation
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002,2003,2004 MontaVista Software Inc.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public License
 *  as published by the Free Software Foundation; either version 2 of
 *  the License, or (at your option) any later version.
 *
 *
 *  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 *  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 *  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 *  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 *  OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *  ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 *  TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
 *  USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this program; if not, write to the Free
 *  Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef _IPMI_MALLOC_H
#define _IPMI_MALLOC_H

/* IPMI uses this for memory allocation, so it can easily be
   substituted, etc. */
void *ipmi_mem_alloc(int size);
void ipmi_mem_free(void *data);

/* strdup using the above memory allocation routines. */
char *ipmi_strdup(const char *str);

/* If you have debug allocations on, then you should call this to
   check for data you haven't freed (after you have freed all the
   data, of course).  It's safe to call even if malloc debugging is
   turned off. */
void ipmi_debug_malloc_cleanup(void);

extern int __ipmi_debug_malloc;
#define DEBUG_MALLOC	(__ipmi_debug_malloc)
#define DEBUG_MALLOC_ENABLE() __ipmi_debug_malloc = 1

#endif /* _IPMI_MALLOC_H */