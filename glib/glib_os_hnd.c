/*
 * glib_os_hnd.c
 *
 * GLIB OS-handlers for OpenIPMI
 *
 * Author: MontaVista Software, Inc.
 *         Corey Minyard <minyard@mvista.com>
 *         source@mvista.com
 *
 * Copyright 2002,2003 MontaVista Software Inc.
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

/* Get the rwlocks for GNU. */
#define _GNU_SOURCE

#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <signal.h>

#include <OpenIPMI/os_handler.h>
#include <OpenIPMI/selector.h>

#include <glib.h>

typedef struct g_os_hnd_data_s
{
    gint priority;
} g_os_hnd_data_t;


struct os_hnd_fd_id_s
{
    guint              ev_id;
    GIOChannel         *chan;
    int                fd;
    void               *cb_data;
    os_data_ready_t    data_ready;
    os_handler_t       *handler;
    os_fd_data_freed_t freed;
};

static gboolean
fd_handler(GIOChannel   *source,
	   GIOCondition condition,
	   gpointer     data)
{
    os_hnd_fd_id_t  *fd_data = (os_hnd_fd_id_t *) data;
    void            *cb_data;
    os_data_ready_t handler;
    int             fd;

    handler = fd_data->data_ready;
    cb_data = fd_data->cb_data;
    fd = fd_data->fd;
    handler(fd, cb_data, fd_data);
    return TRUE;
}

static void
free_fd_data(gpointer data)
{
    os_hnd_fd_id_t *fd_data = (void *) data;

    if (fd_data->freed)
        fd_data->freed(fd_data->fd, fd_data->cb_data);
    g_free(data);
}

static int
add_fd(os_handler_t       *handler,
       int                fd,
       os_data_ready_t    data_ready,
       void               *cb_data,
       os_fd_data_freed_t freed,
       os_hnd_fd_id_t     **id)
{
    os_hnd_fd_id_t  *fd_data;
    g_os_hnd_data_t *info = handler->internal_data;

    fd_data = g_malloc(sizeof(*fd_data));
    if (!fd_data)
	return ENOMEM;
    memset(fd_data, 0, sizeof(fd_data));
    fd_data->chan = g_io_channel_unix_new(fd);
    if (!fd_data->chan) {
	g_free(fd_data);
	return ENOMEM;
    }

    fd_data->fd = fd;
    fd_data->cb_data = cb_data;
    fd_data->data_ready = data_ready;
    fd_data->handler = handler;
    fd_data->freed = freed;

    fd_data->ev_id = g_io_add_watch_full(fd_data->chan,
					 info->priority,
					 G_IO_IN,
					 fd_handler,
					 fd_data,
					 free_fd_data);

    *id = fd_data;
    return 0;
}

static int
remove_fd(os_handler_t *handler, os_hnd_fd_id_t *fd_data)
{
    g_source_remove(fd_data->ev_id);
    /* fd_data gets freed in the free_fd_data callback registered at
       set time. */
    return 0;
}

struct os_hnd_timer_id_s
{
    void           *cb_data;
    os_timed_out_t timed_out;
    int            running;
    os_handler_t   *handler;
    guint          ev_id;
};

static gboolean
timer_handler(gpointer data)
{
    os_hnd_timer_id_t *timer_data = (os_hnd_timer_id_t *) data;
    /* Make a copy of this, because the handler may delete the timer
       data. */
    void              *cb_data;
    os_timed_out_t    timed_out;

    timed_out = timer_data->timed_out;
    cb_data = timer_data->cb_data;
    timer_data->running = 0;
    timed_out(cb_data, timer_data);
    return FALSE;
}

static int
start_timer(os_handler_t      *handler, 
	    os_hnd_timer_id_t *id,
	    struct timeval    *timeout,
	    os_timed_out_t    timed_out,
	    void              *cb_data)
{
    g_os_hnd_data_t *info = handler->internal_data;
    guint           interval;

    if (id->running)
	return EBUSY;

    id->running = 1;
    id->cb_data = cb_data;
    id->timed_out = timed_out;

    interval = (timeout->tv_sec * 1000) | ((timeout->tv_usec + 999) / 1000);
    id->ev_id = g_timeout_add_full(info->priority,
				   interval,
				   timer_handler,
				   id,
				   NULL);
    return 0;
}

static int
stop_timer(os_handler_t *handler, os_hnd_timer_id_t *id)
{
    if (!id->running)
	return EINVAL;

    g_source_remove(id->ev_id);

    return 0;
}

static int
alloc_timer(os_handler_t      *handler, 
	    os_hnd_timer_id_t **id)
{
    os_hnd_timer_id_t *timer_data;

    timer_data = g_malloc(sizeof(*timer_data));
    if (!timer_data)
	return ENOMEM;

    timer_data->running = 0;
    timer_data->timed_out = NULL;
    timer_data->handler = handler;

    *id = timer_data;
    return 0;
}

static int
free_timer(os_handler_t *handler, os_hnd_timer_id_t *id)
{
    if (id->running)
	return EBUSY;

    g_free(id);
    return 0;
}


static int
get_random(os_handler_t *handler, void *data, unsigned int len)
{
#if GLIB_MAJOR_VERSION < 2
    int fd = open("/dev/urandom", O_RDONLY);
    int rv;

    if (fd == -1)
	return errno;

    rv = read(fd, data, len);

    close(fd);
    return rv;
#else
    gint32 val;
    char   *out = data;

    while (len >= sizeof(val)) {
	val = g_random_int();
	memcpy(out, &val, sizeof(val));
	len -= sizeof(val);
	out += sizeof(val);
    }

    if (len) {
	val = g_random_int();
	memcpy(out, &val, len);
	len -= sizeof(val);
    }

    return 0;
#endif
}

static GStaticPrivate vlog_private = G_STATIC_PRIVATE_INIT;

typedef struct vlog_data_s
{
    int  len;
    int  curr;
    char *data;
} vlog_data_t;

static void
vlog_data_destroy(gpointer data)
{
    vlog_data_t *info = data;

    if (info->data)
	g_free(info->data);
    g_free(info);
}

static vlog_data_t *
get_vlog_data(void)
{
    vlog_data_t *rv;

    rv = g_static_private_get(&vlog_private);
    if (!rv) {
	rv = g_malloc(sizeof(*rv));
	if (rv) {
	    memset(rv, 0, sizeof(*rv));
	    g_static_private_set(&vlog_private, rv, vlog_data_destroy);
	}
    }

    return rv;
}

static void
add_vlog_data(vlog_data_t *info,
	      char        *format,
	      va_list     ap)
{
    int len;

    len = vsnprintf(info->data+info->curr, info->len-info->curr, format, ap);
    if ((len + info->curr) > info->len) {
	char *nd;
	int  new_size;

	new_size = info->len + 64;
	while (new_size < (info->curr + info->len))
	    new_size += 64;

	nd = g_malloc(new_size);
	if (!nd)
	    return;
	if (info->data) {
	    memcpy(nd, info->data, info->curr);
	    g_free(info->data);
	    info->data = nd;
	}
	len = vsnprintf(info->data+info->curr, info->len-info->curr,
			format, ap);
    }

    info->curr += len;
}

static void
glib_vlog(os_handler_t         *handler,
	  enum ipmi_log_type_e log_type,
	  char                 *format,
	  va_list              ap)
{
    GLogLevelFlags flags;
    vlog_data_t    *info;

    switch (log_type) {
    case IPMI_LOG_INFO:		flags = G_LOG_LEVEL_INFO; break;
    case IPMI_LOG_WARNING:	flags = G_LOG_LEVEL_WARNING; break;
    case IPMI_LOG_SEVERE:	flags = G_LOG_LEVEL_CRITICAL; break;
    case IPMI_LOG_FATAL:	flags = G_LOG_LEVEL_ERROR; break;
    case IPMI_LOG_ERR_INFO:	flags = G_LOG_LEVEL_MESSAGE; break;
    case IPMI_LOG_DEBUG:	flags = G_LOG_LEVEL_DEBUG; break;

    case IPMI_LOG_DEBUG_END:
	info = get_vlog_data();
	if (!info)
	    return;
	add_vlog_data(info, format, ap);
	g_log("OpenIPMI", G_LOG_LEVEL_DEBUG, "%s", info->data);
	info->curr = 0;
	return;

    case IPMI_LOG_DEBUG_START:
	info = get_vlog_data();
	if (!info)
	    return;
	info->curr = 0;
	add_vlog_data(info, format, ap);
	return;

    case IPMI_LOG_DEBUG_CONT:
	info = get_vlog_data();
	if (!info)
	    return;
	add_vlog_data(info, format, ap);
	return;

    default:
	flags = G_LOG_LEVEL_INFO;
	break;
    }

    g_logv("OpenIPMI", flags, format, ap);
}

static void
glib_log(os_handler_t         *handler,
	 enum ipmi_log_type_e log_type,
	 char                 *format,
	 ...)
{
    va_list ap;

    va_start(ap, format);
    glib_vlog(handler, log_type, format, ap);
    va_end(ap);
}


struct os_hnd_lock_s
{
    GMutex *mutex;
};

static int
create_lock(os_handler_t  *handler,
	    os_hnd_lock_t **id)
{
    os_hnd_lock_t *lock;

    lock = g_malloc(sizeof(*lock));
    if (!lock)
	return ENOMEM;
    lock->mutex = g_mutex_new();
    if (!lock->mutex) {
	g_free(lock);
	return ENOMEM;
    }

    *id = lock;
    return 0;
}

static int
destroy_lock(os_handler_t  *handler,
	     os_hnd_lock_t *id)
{
    g_mutex_free(id->mutex);
    g_free(id);
    return 0;
}

static int
lock(os_handler_t  *handler,
     os_hnd_lock_t *id)
{
    g_mutex_lock(id->mutex);
    return 0;
}

static int
unlock(os_handler_t  *handler,
       os_hnd_lock_t *id)
{
    g_mutex_unlock(id->mutex);
    return 0;
}

struct os_hnd_cond_s
{
    GCond *cond;
};

static int
create_cond(os_handler_t  *handler,
	    os_hnd_cond_t **new_cond)
{
    os_hnd_cond_t *cond;

    cond = g_malloc(sizeof(*cond));
    if (!cond)
	return ENOMEM;
    cond->cond = g_cond_new();
    if (!cond->cond) {
	g_free(cond);
	return ENOMEM;
    }

    *new_cond = cond;
    return 0;
}

static int
destroy_cond(os_handler_t  *handler,
	     os_hnd_cond_t *cond)
{
    g_cond_free(cond->cond);
    g_free(cond);
    return 0;
}

static int
cond_wait(os_handler_t  *handler,
	  os_hnd_cond_t *cond,
	  os_hnd_lock_t *lock)
{
    g_cond_wait(cond->cond, lock->mutex);
    return 0;
}

static int
cond_timedwait(os_handler_t   *handler,
	       os_hnd_cond_t  *cond,
	       os_hnd_lock_t  *lock,
	       struct timeval *rtimeout)
{
    GTimeVal timeout;
    GTimeVal now;
    int      rv;

    g_get_current_time(&now);
    timeout.tv_sec = rtimeout->tv_sec + now.tv_sec;
    timeout.tv_usec = rtimeout->tv_usec + now.tv_usec;
    while (timeout.tv_usec > 1000000) {
	timeout.tv_sec += 1;
	timeout.tv_usec -= 1000000;
    }

    rv = g_cond_timed_wait(cond->cond, lock->mutex, &timeout);
    if (rv)
	return ETIMEDOUT;
    return 0;
}

static int
cond_wake(os_handler_t  *handler,
	  os_hnd_cond_t *cond)
{
     g_cond_signal(cond->cond);
     return 0;
}

static int
cond_broadcast(os_handler_t  *handler,
	       os_hnd_cond_t *cond)
{
     g_cond_broadcast(cond->cond);
     return 0;
}

#if 0
static int
create_thread(os_handler_t       *handler,
	      int                priority,
	      void               (*startup)(void *data),
	      void               *data)
{
    GThread *t;

    t = g_thread_create_full(startup,
			     data,
			     0,
			     FALSE,
			     FALSE,
			     priority,
			     NULL);

    if (!t)
	return ENOMEM;

    return 0;
}

static int
thread_exit(os_handler_t *handler)
{
    g_thread_exit(NULL);
}
#endif


static int
perform_one_op(os_handler_t   *os_hnd,
	       struct timeval *timeout)
{
    g_main_iteration(TRUE);
    return 0;
}

static void
operation_loop(os_handler_t *os_hnd)
{
    for (;;)
	g_main_iteration(TRUE);
}

static void
free_os_handler(os_handler_t *os_hnd)
{
    g_os_hnd_data_t *info = os_hnd->internal_data;

    g_free(info);
    g_free(os_hnd);
}

static void *
glib_malloc(int size)
{
    return g_malloc(size);
}

static void
glib_free(void *data)
{
    g_free(data);
}

static os_handler_t ipmi_glib_os_handler =
{
    .mem_alloc = glib_malloc,
    .mem_free = glib_free,

    .add_fd_to_wait_for = add_fd,
    .remove_fd_to_wait_for = remove_fd,

    .start_timer = start_timer,
    .stop_timer = stop_timer,
    .alloc_timer = alloc_timer,
    .free_timer = free_timer,

    .get_random = get_random,
    .log = glib_log,
    .vlog = glib_vlog,

    .create_lock = create_lock,
    .destroy_lock = destroy_lock,
    .lock = lock,
    .unlock = unlock,

    .create_cond = create_cond,
    .destroy_cond = destroy_cond,
    .cond_wait = cond_wait,
    .cond_timedwait = cond_timedwait,
    .cond_wake = cond_wake,
    .cond_broadcast = cond_broadcast,

#if 0
    .create_thread = create_thread,
    .thread_exit = thread_exit,
#endif

    .free_os_handler = free_os_handler,

    .perform_one_op = perform_one_op,
    .operation_loop = operation_loop,
};

os_handler_t *
ipmi_glib_get_os_handler(int priority)
{
    os_handler_t    *rv;
    g_os_hnd_data_t *info;

    rv = g_malloc(sizeof(*rv));
    if (!rv)
	return NULL;

    memcpy(rv, &ipmi_glib_os_handler, sizeof(*rv));

    info = g_malloc(sizeof(*info));
    if (! info) {
	g_free(rv);
	return NULL;
    }

    info->priority = priority;
    rv->internal_data = info;

    return rv;
}