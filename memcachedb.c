/*
 *  MemcacheDB - A distributed key-value storage system designed for persistent:
 *
 *      http://memcachedb.googlecode.com
 *
 *  The source code of Memcachedb is most based on Memcached:
 *
 *      http://danga.com/memcached/
 *
 *  Copyright 2008 Steve Chu.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Steve Chu <stvchu@gmail.com>
 *
 */

#include "memcachedb.h"
#include <sys/stat.h>
#ifndef WIN32
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/resource.h>
#include <sys/uio.h>
#endif
#include <signal.h>



/* some POSIX systems need the following definition
 * to get mlockall flags out of sys/mman.h.  */
#ifndef _P1003_1B_VISIBLE
#define _P1003_1B_VISIBLE
#endif
/* need this to get IOV_MAX on some platforms. */
#ifndef __need_IOV_MAX
#define __need_IOV_MAX
#endif
#ifndef WIN32
#include <pwd.h>
#include <sys/mman.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#else
#include "bsd_getopt.h"
#include <process.h>
#include "config.h"
#include "ntservice.h"
#endif

#include <fcntl.h>


#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <limits.h>

#ifdef HAVE_MALLOC_H
/* OpenBSD has a malloc.h, but warns to use stdlib.h instead */
#ifndef __OpenBSD__
#include <malloc.h>
#endif
#endif

/* FreeBSD 4.x doesn't have IOV_MAX exposed. */
#ifndef IOV_MAX
#if defined(__FreeBSD__) || defined(__APPLE__)
# define IOV_MAX 1024
#endif
#endif

#ifndef IOV_MAX
# define IOV_MAX 1024
#endif

/*
 * forward declarations
 */
static void drive_machine(conn *c);
static int new_socket(struct addrinfo *ai);
static int server_socket(const int port, const bool is_udp);
static int try_read_command(conn *c);
static int try_read_network(conn *c);
static int try_read_udp(conn *c);

/* stats */
static void stats_reset(void);
static void stats_init(void);

/* defaults */
static void settings_init(void);

/* event handling, network IO */
static void event_handler(const int fd, const short which, void *arg);
static void conn_close(conn *c);
static void conn_init(void);
static void accept_new_conns(const bool do_accept);
static bool update_event(conn *c, const int new_flags);
static void complete_nread(conn *c);
static void process_command(conn *c, char *command);
static int transmit(conn *c);
static int ensure_iov_space(conn *c);
static int add_iov(conn *c, const void *buf, int len);
static int add_msghdr(conn *c);

static void conn_free(conn *c);

/** exported globals **/
struct stats stats;
struct settings settings;

struct bdb_settings bdb_settings;
struct bdb_version bdb_version;
DB_ENV *env;
DB *dbp;

int daemon_quit = 0;

/** file scope variables **/
static conn *listen_conn = NULL;
static struct event_base *main_base;

#define TRANSMIT_COMPLETE   0
#define TRANSMIT_INCOMPLETE 1
#define TRANSMIT_SOFT_ERROR 2
#define TRANSMIT_HARD_ERROR 3

static void stats_init(void) {
    stats.curr_conns = stats.total_conns = stats.conn_structs = 0;
    stats.get_cmds = stats.set_cmds = stats.get_hits = stats.get_misses = 0;
    stats.bytes_read = stats.bytes_written = 0;

    /* make the time we started always be 2 seconds before we really
       did, so time(0) - time.started is never zero.  if so, things
       like 'settings.oldest_live' which act as booleans as well as
       values are now false in boolean context... */
    stats.started = time(0) - 2;
}

static void stats_reset(void) {
    STATS_LOCK();
    stats.total_conns = 0;
    stats.get_cmds = stats.set_cmds = stats.get_hits = stats.get_misses = 0;
    stats.bytes_read = stats.bytes_written = 0;
    STATS_UNLOCK();
}

static void settings_init(void) {
    settings.access=0700;
    settings.port = 21201;
    settings.udpport = 0;
    /* By default this string should be NULL for getaddrinfo() */
    settings.inter = NULL;
    settings.item_buf_size = 2 * 1024;     /* default is 2KB */
    settings.maxconns = 4 * 1024;         /* to limit connections-related memory to about 5MB * 4 */
    settings.verbose = 0;
    settings.socketpath = NULL;       /* by default, not using a unix socket */
#ifdef USE_THREADS
    settings.num_threads = 4;
#else
    settings.num_threads = 1;
#endif
}

/*
 * Adds a message header to a connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int add_msghdr(conn *c)
{
    struct msghdr *msg;

    assert(c != NULL);

    if (c->msgsize == c->msgused) {
        msg = realloc(c->msglist, c->msgsize * 2 * sizeof(struct msghdr));
        if (! msg)
            return -1;
        c->msglist = msg;
        c->msgsize *= 2;
    }

    msg = c->msglist + c->msgused;

    /* this wipes msg_iovlen, msg_control, msg_controllen, and
       msg_flags, the last 3 of which aren't defined on solaris: */
    memset(msg, 0, sizeof(struct msghdr));

    msg->msg_iov = &c->iov[c->iovused];

    if (c->request_addr_size > 0) {
        msg->msg_name = &c->request_addr;
        msg->msg_namelen = c->request_addr_size;
    }

    c->msgbytes = 0;
    c->msgused++;

    if (c->udp) {
        /* Leave room for the UDP header, which we'll fill in later. */
        return add_iov(c, NULL, UDP_HEADER_SIZE);
    }

    return 0;
}


/*
 * Free list management for connections.
 */

static conn **freeconns;
static int freetotal;
static int freecurr;


static void conn_init(void) {
    freetotal = 200;
    freecurr = 0;
    if ((freeconns = (conn **)malloc(sizeof(conn *) * freetotal)) == NULL) {
        fprintf(stderr, "malloc()\n");
    }
    return;
}

/*
 * Returns a connection from the freelist, if any. Should call this using
 * conn_from_freelist() for thread safety.
 */
conn *do_conn_from_freelist() {
    conn *c;

    if (freecurr > 0) {
        c = freeconns[--freecurr];
    } else {
        c = NULL;
    }

    return c;
}

/*
 * Adds a connection to the freelist. 0 = success. Should call this using
 * conn_add_to_freelist() for thread safety.
 */
bool do_conn_add_to_freelist(conn *c) {
    if (freecurr < freetotal) {
        freeconns[freecurr++] = c;
        return false;
    } else {
        /* try to enlarge free connections array */
        conn **new_freeconns = realloc(freeconns, sizeof(conn *) * freetotal * 2);
        if (new_freeconns) {
            freetotal *= 2;
            freeconns = new_freeconns;
            freeconns[freecurr++] = c;
            return false;
        }
    }
    return true;
}

conn *conn_new(const int sfd, const int init_state, const int event_flags,
                const int read_buffer_size, const bool is_udp, struct event_base *base) {
    conn *c = conn_from_freelist();

    if (NULL == c) {
        if (!(c = (conn *)calloc(1, sizeof(conn)))) {
            fprintf(stderr, "calloc()\n");
            return NULL;
        }
        c->rbuf = c->wbuf = 0;
        c->ilist = 0;
        c->iov = 0;
        c->msglist = 0;
        c->hdrbuf = 0;

        c->rsize = read_buffer_size;
        c->wsize = DATA_BUFFER_SIZE;
        c->isize = ITEM_LIST_INITIAL;
        c->iovsize = IOV_LIST_INITIAL;
        c->msgsize = MSG_LIST_INITIAL;
        c->hdrsize = 0;

        c->rbuf = (char *)malloc((size_t)c->rsize);
        c->wbuf = (char *)malloc((size_t)c->wsize);
        c->ilist = (item **)malloc(sizeof(item *) * c->isize);
        c->iov = (struct iovec *)malloc(sizeof(struct iovec) * c->iovsize);
        c->msglist = (struct msghdr *)malloc(sizeof(struct msghdr) * c->msgsize);

        if (c->rbuf == 0 || c->wbuf == 0 || c->ilist == 0 || c->iov == 0 ||
                c->msglist == 0) {
            conn_free(c);
            fprintf(stderr, "malloc()\n");
            return NULL;
        }

        STATS_LOCK();
        stats.conn_structs++;
        STATS_UNLOCK();
    }

    if (settings.verbose > 1) {
        if (init_state == conn_listening)
            fprintf(stderr, "<%d server listening\n", sfd);
        else if (is_udp)
            fprintf(stderr, "<%d server listening (udp)\n", sfd);
        else
            fprintf(stderr, "<%d new client connection\n", sfd);
    }

    c->sfd = sfd;
    c->udp = is_udp;
    c->state = init_state;
    c->rlbytes = 0;
    c->rbytes = c->wbytes = 0;
    c->wcurr = c->wbuf;
    c->rcurr = c->rbuf;
    c->ritem = 0;
    c->icurr = c->ilist;
    c->ileft = 0;
    c->iovused = 0;
    c->msgcurr = 0;
    c->msgused = 0;

    c->write_and_go = conn_read;
    c->write_and_free = 0;
    c->item = 0;

    event_set(&c->event, sfd, event_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = event_flags;

    if (event_add(&c->event, 0) == -1) {
        if (conn_add_to_freelist(c)) {
            conn_free(c);
        }
        perror("event_add");
        return NULL;
    }

    STATS_LOCK();
    stats.curr_conns++;
    stats.total_conns++;
    STATS_UNLOCK();

    return c;
}

static void conn_cleanup(conn *c) {
    assert(c != NULL);

    if (c->item) {
        item_free(c->item);
        c->item = 0;
    }

    if (c->ileft != 0) {
        for (; c->ileft > 0; c->ileft--,c->icurr++) {
            item_free(*(c->icurr));
        }
    }

    if (c->write_and_free) {
        free(c->write_and_free);
        c->write_and_free = 0;
    }
}

/*
 * Frees a connection.
 */
void conn_free(conn *c) {
    if (c) {
        if (c->hdrbuf)
            free(c->hdrbuf);
        if (c->msglist)
            free(c->msglist);
        if (c->rbuf)
            free(c->rbuf);
        if (c->wbuf)
            free(c->wbuf);
        if (c->ilist)
            free(c->ilist);
        if (c->iov)
            free(c->iov);
        free(c);
    }
}

static void conn_close(conn *c) {
    assert(c != NULL);

    /* delete the event, the socket and the conn */
    event_del(&c->event);

    if (settings.verbose > 1)
        fprintf(stderr, "<%d connection closed.\n", c->sfd);

    close(c->sfd);
    accept_new_conns(true);
    conn_cleanup(c);

    /* if the connection has big buffers, just free it */
    if (c->rsize > READ_BUFFER_HIGHWAT || conn_add_to_freelist(c)) {
        conn_free(c);
    }

    STATS_LOCK();
    stats.curr_conns--;
    STATS_UNLOCK();

    return;
}


/*
 * Shrinks a connection's buffers if they're too big.  This prevents
 * periodic large "get" requests from permanently chewing lots of server
 * memory.
 *
 * This should only be called in between requests since it can wipe output
 * buffers!
 */
static void conn_shrink(conn *c) {
    assert(c != NULL);

    if (c->udp)
        return;

    if (c->rsize > READ_BUFFER_HIGHWAT && c->rbytes < DATA_BUFFER_SIZE) {
        char *newbuf;

        if (c->rcurr != c->rbuf)
            memmove(c->rbuf, c->rcurr, (size_t)c->rbytes);

        newbuf = (char *)realloc((void *)c->rbuf, DATA_BUFFER_SIZE);

        if (newbuf) {
            c->rbuf = newbuf;
            c->rsize = DATA_BUFFER_SIZE;
        }
        /* TODO check other branch... */
        c->rcurr = c->rbuf;
    }

    if (c->isize > ITEM_LIST_HIGHWAT) {
        item **newbuf = (item**) realloc((void *)c->ilist, ITEM_LIST_INITIAL * sizeof(c->ilist[0]));
        if (newbuf) {
            c->ilist = newbuf;
            c->isize = ITEM_LIST_INITIAL;
        }
    /* TODO check error condition? */
    }

    if (c->msgsize > MSG_LIST_HIGHWAT) {
        struct msghdr *newbuf = (struct msghdr *) realloc((void *)c->msglist, MSG_LIST_INITIAL * sizeof(c->msglist[0]));
        if (newbuf) {
            c->msglist = newbuf;
            c->msgsize = MSG_LIST_INITIAL;
        }
    /* TODO check error condition? */
    }

    if (c->iovsize > IOV_LIST_HIGHWAT) {
        struct iovec *newbuf = (struct iovec *) realloc((void *)c->iov, IOV_LIST_INITIAL * sizeof(c->iov[0]));
        if (newbuf) {
            c->iov = newbuf;
            c->iovsize = IOV_LIST_INITIAL;
        }
    /* TODO check return value */
    }
}

/*
 * Sets a connection's current state in the state machine. Any special
 * processing that needs to happen on certain state transitions can
 * happen here.
 */
static void conn_set_state(conn *c, int state) {
    assert(c != NULL);

    if (state != c->state) {
        if (state == conn_read) {
            conn_shrink(c);
        }
        c->state = state;
    }
}


/*
 * Ensures that there is room for another struct iovec in a connection's
 * iov list.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */
static int ensure_iov_space(conn *c) {
    assert(c != NULL);

    if (c->iovused >= c->iovsize) {
        int i, iovnum;
        struct iovec *new_iov = (struct iovec *)realloc(c->iov,
                                (c->iovsize * 2) * sizeof(struct iovec));
        if (! new_iov)
            return -1;
        c->iov = new_iov;
        c->iovsize *= 2;

        /* Point all the msghdr structures at the new list. */
        for (i = 0, iovnum = 0; i < c->msgused; i++) {
            c->msglist[i].msg_iov = &c->iov[iovnum];
            iovnum += c->msglist[i].msg_iovlen;
        }
    }

    return 0;
}


/*
 * Adds data to the list of pending data that will be written out to a
 * connection.
 *
 * Returns 0 on success, -1 on out-of-memory.
 */

static int add_iov(conn *c, const void *buf, int len) {
    struct msghdr *m;
    int leftover;
    bool limit_to_mtu;

    assert(c != NULL);

    do {
        m = &c->msglist[c->msgused - 1];

        /*
         * Limit UDP packets, and the first payloads of TCP replies, to
         * UDP_MAX_PAYLOAD_SIZE bytes.
         */
        limit_to_mtu = c->udp || (1 == c->msgused);

        /* We may need to start a new msghdr if this one is full. */
        if (m->msg_iovlen == IOV_MAX ||
            (limit_to_mtu && c->msgbytes >= UDP_MAX_PAYLOAD_SIZE)) {
            add_msghdr(c);
            m = &c->msglist[c->msgused - 1];
        }

        if (ensure_iov_space(c) != 0)
            return -1;

        /* If the fragment is too big to fit in the datagram, split it up */
        if (limit_to_mtu && len + c->msgbytes > UDP_MAX_PAYLOAD_SIZE) {
            leftover = len + c->msgbytes - UDP_MAX_PAYLOAD_SIZE;
            len -= leftover;
        } else {
            leftover = 0;
        }

        m = &c->msglist[c->msgused - 1];
        m->msg_iov[m->msg_iovlen].iov_base = (void *)buf;
        m->msg_iov[m->msg_iovlen].iov_len = len;

        c->msgbytes += len;
        c->iovused++;
        m->msg_iovlen++;

        buf = ((char *)buf) + len;
        len = leftover;
    } while (leftover > 0);

    return 0;
}


/*
 * Constructs a set of UDP headers and attaches them to the outgoing messages.
 */
static int build_udp_headers(conn *c) {
    int i;
    unsigned char *hdr;

    assert(c != NULL);

    if (c->msgused > c->hdrsize) {
        void *new_hdrbuf;
        if (c->hdrbuf)
            new_hdrbuf = realloc(c->hdrbuf, c->msgused * 2 * UDP_HEADER_SIZE);
        else
            new_hdrbuf = malloc(c->msgused * 2 * UDP_HEADER_SIZE);
        if (! new_hdrbuf)
            return -1;
        c->hdrbuf = (unsigned char *)new_hdrbuf;
        c->hdrsize = c->msgused * 2;
    }

    hdr = c->hdrbuf;
    for (i = 0; i < c->msgused; i++) {
        c->msglist[i].msg_iov[0].iov_base = hdr;
        c->msglist[i].msg_iov[0].iov_len = UDP_HEADER_SIZE;
        *hdr++ = c->request_id / 256;
        *hdr++ = c->request_id % 256;
        *hdr++ = i / 256;
        *hdr++ = i % 256;
        *hdr++ = c->msgused / 256;
        *hdr++ = c->msgused % 256;
        *hdr++ = 0;
        *hdr++ = 0;
        assert((char FAR*) hdr == (char FAR*)c->msglist[i].msg_iov[0].iov_base + UDP_HEADER_SIZE);
    }

    return 0;
}


static void out_string(conn *c, const char *str) {
    size_t len;

    assert(c != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, ">%d %s\n", c->sfd, str);

    len = strlen(str);
    if ((len + 2) > c->wsize) {
        /* ought to be always enough. just fail for simplicity */
        str = "SERVER_ERROR output line too long";
        len = strlen(str);
    }

    memcpy(c->wbuf, str, len);
    memcpy(c->wbuf + len, "\r\n", 2);
    c->wbytes = len + 2;
    c->wcurr = c->wbuf;

    conn_set_state(c, conn_write);
    c->write_and_go = conn_read;
    return;
}

/*
 * we get here after reading the value in set/add/replace commands. The command
 * has been stored in c->item_comm, and the item is ready in c->item.
 */

static void complete_nread(conn *c) {
	item *it;
	int comm;
	int ret;
    assert(c != NULL);

    it = c->item;
    comm = c->item_comm;

    STATS_LOCK();
    stats.set_cmds++;
    STATS_UNLOCK();

    if (strncmp(ITEM_data(it) + it->nbytes - 2, "\r\n", 2) != 0) {
        out_string(c, "CLIENT_ERROR bad data chunk");
    } else {
      ret = store_item(it, comm);
      if (ret == 1)
          out_string(c, "STORED");
      else if(ret == 2)
          out_string(c, "EXISTS");
      else if(ret == 3)
          out_string(c, "NOT_FOUND");
      else
          out_string(c, "NOT_STORED");
    }

    item_free(c->item);
    c->item = 0;
}

/*
 * Stores an item in the cache according to the semantics of one of the set
 * commands. In threaded mode, this is protected by the cache lock.
 *
 * Returns true if the item was stored.
 */
int do_store_item(item *it, int comm) {
    char *key = ITEM_key(it);
    int ret;
    item *old_it = NULL;
    item *new_it = NULL;
    int stored = 0;
    int flags;

    if (comm == NREAD_ADD || comm == NREAD_REPLACE) {
        ret = item_exists(key, strlen(key));
        if ((ret == 0 && comm == NREAD_REPLACE) ||
            (ret == 1 && comm == NREAD_ADD) ){
               return 0;
        }
    } else if (comm == NREAD_APPEND || comm == NREAD_PREPEND){
        /* get orignal item */
        old_it = item_get(key, strlen(key));
        if (old_it == NULL){
            return 0;
        }
        
        /* we have it and old_it here - alloc memory to hold both */
        /* flags was already lost - so recover them from ITEM_suffix(it) */
        flags = (int) strtol(ITEM_suffix(old_it), (char **) NULL, 10);        
        new_it = item_alloc1(key, it->nkey, flags, it->nbytes + old_it->nbytes - 2 /* CRLF */);
        if (new_it == NULL) {
            /* SERVER_ERROR out of memory */
            if (old_it != NULL)
                item_free(old_it);
            return 0;
        }
        
        /* copy data from it and old_it to new_it */        
        if (comm == NREAD_APPEND) {
            memcpy(ITEM_data(new_it), ITEM_data(old_it), old_it->nbytes);
            memcpy(ITEM_data(new_it) + old_it->nbytes - 2 /* CRLF */, ITEM_data(it), it->nbytes);
        } else {
            /* NREAD_PREPEND */
            memcpy(ITEM_data(new_it), ITEM_data(it), it->nbytes);
            memcpy(ITEM_data(new_it) + it->nbytes - 2 /* CRLF */, ITEM_data(old_it), old_it->nbytes);
        }
        
        it = new_it;
    }
        
    ret = item_put(key, strlen(key), it);
    
    if (old_it != NULL)
        item_free(old_it);
    if (new_it != NULL)
        item_free(new_it);

    if (ret  == 0) {
        return 1;
    } else {
        return 0;
    }
}

typedef struct token_s {
    char *value;
    size_t length;
} token_t;

#define COMMAND_TOKEN 0
#define SUBCOMMAND_TOKEN 1
#define KEY_TOKEN 1
#define KEY_MAX_LENGTH 250

#define MAX_TOKENS 8

/*
 * Tokenize the command string by replacing whitespace with '\0' and update
 * the token array tokens with pointer to start of each token and length.
 * Returns total number of tokens.  The last valid token is the terminal
 * token (value points to the first unprocessed character of the string and
 * length zero).
 *
 * Usage example:
 *
 *  while(tokenize_command(command, ncommand, tokens, max_tokens) > 0) {
 *      for(int ix = 0; tokens[ix].length != 0; ix++) {
 *          ...
 *      }
 *      ncommand = tokens[ix].value - command;
 *      command  = tokens[ix].value;
 *   }
 */
static size_t tokenize_command(char *command, token_t *tokens, const size_t max_tokens) {
    char *s, *e;
    size_t ntokens = 0;

    assert(command != NULL && tokens != NULL && max_tokens > 1);

    for (s = e = command; ntokens < max_tokens - 1; ++e) {
        if (*e == ' ') {
            if (s != e) {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
                *e = '\0';
            }
            s = e + 1;
        }
        else if (*e == '\0') {
            if (s != e) {
                tokens[ntokens].value = s;
                tokens[ntokens].length = e - s;
                ntokens++;
            }

            break; /* string end */
        }
    }

    /*
     * If we scanned the whole string, the terminal value pointer is null,
     * otherwise it is the first unprocessed character.
     */
    tokens[ntokens].value =  *e == '\0' ? NULL : e;
    tokens[ntokens].length = 0;
    ntokens++;

    return ntokens;
}

/* set up a connection to write a buffer then free it, used for stats */
static void write_and_free(conn *c, char *buf, int bytes) {
    if (buf) {
        c->write_and_free = buf;
        c->wcurr = buf;
        c->wbytes = bytes;
        conn_set_state(c, conn_write);
        c->write_and_go = conn_read;
    } else {
        out_string(c, "SERVER_ERROR out of memory writing stats");
    }
}

static void process_stat(conn *c, token_t *tokens, const size_t ntokens) {
    time_t now = time(0);
    char *command;
    char *subcommand;

    assert(c != NULL);

    if(ntokens < 2) {
        out_string(c, "CLIENT_ERROR bad command line");
        return;
    }

    command = tokens[COMMAND_TOKEN].value;

    if (ntokens == 2 && strcmp(command, "stats") == 0) {
        char temp[1024];
        pid_t pid = getpid();
        char *pos = temp;

#ifndef WIN32
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);
#endif /* !WIN32 */

        STATS_LOCK();
        pos += sprintf(pos, "STAT pid %u\r\n", pid);
        pos += sprintf(pos, "STAT uptime %ld\r\n", now - stats.started);
        pos += sprintf(pos, "STAT time %ld\r\n", now);
        pos += sprintf(pos, "STAT version " VERSION "\r\n");
        pos += sprintf(pos, "STAT pointer_size %d\r\n", 8 * sizeof(void *));
#ifndef WIN32
        pos += sprintf(pos, "STAT rusage_user %ld.%06ld\r\n", usage.ru_utime.tv_sec, usage.ru_utime.tv_usec);
        pos += sprintf(pos, "STAT rusage_system %ld.%06ld\r\n", usage.ru_stime.tv_sec, usage.ru_stime.tv_usec);
#endif /* !WIN32 */
        pos += sprintf(pos, "STAT ibuffer_size %u\r\n", settings.item_buf_size);
        pos += sprintf(pos, "STAT curr_connections %u\r\n", stats.curr_conns - 1); /* ignore listening conn */
        pos += sprintf(pos, "STAT total_connections %u\r\n", stats.total_conns);
        pos += sprintf(pos, "STAT connection_structures %u\r\n", stats.conn_structs);
        pos += sprintf(pos, "STAT cmd_get %llu\r\n", stats.get_cmds);
        pos += sprintf(pos, "STAT cmd_set %llu\r\n", stats.set_cmds);
        pos += sprintf(pos, "STAT get_hits %llu\r\n", stats.get_hits);
        pos += sprintf(pos, "STAT get_misses %llu\r\n", stats.get_misses);
        pos += sprintf(pos, "STAT bytes_read %llu\r\n", stats.bytes_read);
        pos += sprintf(pos, "STAT bytes_written %llu\r\n", stats.bytes_written);
        pos += sprintf(pos, "STAT threads %u\r\n", settings.num_threads);
        pos += sprintf(pos, "END");
        STATS_UNLOCK();
        out_string(c, temp);
        return;
    }

    subcommand = tokens[SUBCOMMAND_TOKEN].value;

    if (strcmp(subcommand, "reset") == 0) {
        stats_reset();
        out_string(c, "RESET");
        return;
    }

    /* for bdb stats */
    if (strcmp(subcommand, "bdb") == 0) {
        char temp[512];
        stats_bdb(temp);
        out_string(c, temp);
        return;
    }
    
    /* for replication stats */
    if (bdb_settings.is_replicated){
        if (strcmp(subcommand, "rep") == 0) {
            char temp[2048];
            stats_rep(temp);
            out_string(c, temp);
            return;
        }
        if (strcmp(subcommand, "repmgr") == 0) {
            char temp[256];
            stats_repmgr(temp);
            out_string(c, temp);
            return;
        }
        if (strcmp(subcommand, "repcfg") == 0) {
            char temp[512];
            stats_repcfg(temp);
            out_string(c, temp);
            return;
        }
        if (strcmp(subcommand, "repms") == 0) {
            char temp[256];
            stats_repms(temp);
            out_string(c, temp);
            return;
        }
    }

#ifdef HAVE_MALLOC_H
#ifdef HAVE_STRUCT_MALLINFO
    if (strcmp(subcommand, "malloc") == 0) {
        char temp[512];
        struct mallinfo info;
        char *pos = temp;

        info = mallinfo();
        pos += sprintf(pos, "STAT arena_size %d\r\n", info.arena);
        pos += sprintf(pos, "STAT free_chunks %d\r\n", info.ordblks);
        pos += sprintf(pos, "STAT fastbin_blocks %d\r\n", info.smblks);
        pos += sprintf(pos, "STAT mmapped_regions %d\r\n", info.hblks);
        pos += sprintf(pos, "STAT mmapped_space %d\r\n", info.hblkhd);
        pos += sprintf(pos, "STAT max_total_alloc %d\r\n", info.usmblks);
        pos += sprintf(pos, "STAT fastbin_space %d\r\n", info.fsmblks);
        pos += sprintf(pos, "STAT total_alloc %d\r\n", info.uordblks);
        pos += sprintf(pos, "STAT total_free %d\r\n", info.fordblks);
        pos += sprintf(pos, "STAT releasable_space %d\r\nEND", info.keepcost);
        out_string(c, temp);
        return;
    }
#endif /* HAVE_STRUCT_MALLINFO */
#endif /* HAVE_MALLOC_H */

#if !defined(WIN32) || !defined(__APPLE__)
    if (strcmp(subcommand, "maps") == 0) {
        char *wbuf;
        int wsize = 8192; /* should be enough */
        int fd;
        int res;

        if ((wbuf = (char *)malloc(wsize)) == NULL) {
            out_string(c, "SERVER_ERROR out of memory writing stats maps");
            return;
        }

        fd = open("/proc/self/maps", O_RDONLY);
        if (fd == -1) {
            out_string(c, "SERVER_ERROR cannot open the maps file");
            free(wbuf);
            return;
        }

        res = read(fd, wbuf, wsize - 6);  /* 6 = END\r\n\0 */
        if (res == wsize - 6) {
            out_string(c, "SERVER_ERROR buffer overflow");
            free(wbuf); close(fd);
            return;
        }
        if (res == 0 || res == -1) {
            out_string(c, "SERVER_ERROR can't read the maps file");
            free(wbuf); close(fd);
            return;
        }
        memcpy(wbuf + res, "END\r\n", 5);
        write_and_free(c, wbuf, res + 5);
        close(fd);
        return;
    }
#endif

    out_string(c, "ERROR");
}

/* ntokens is overwritten here... shrug.. */
static inline void process_get_command(conn *c, token_t *tokens, size_t ntokens) {
    char *key;
    size_t nkey;
    int i = 0;
    item *it = NULL;
    token_t *key_token = &tokens[KEY_TOKEN];
    int stats_get_cmds   = 0;
    int stats_get_hits   = 0;
    int stats_get_misses = 0;
    assert(c != NULL);

    do {
        while(key_token->length != 0) {

            key = key_token->value;
            nkey = key_token->length;

            if(nkey > KEY_MAX_LENGTH) {
                STATS_LOCK();
                stats.get_cmds   += stats_get_cmds;
                stats.get_hits   += stats_get_hits;
                stats.get_misses += stats_get_misses;
                STATS_UNLOCK();
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }

            stats_get_cmds++;
            
            it = item_get(key, nkey);

            if (it) {
                if (i >= c->isize) {
                    item **new_list = realloc(c->ilist, sizeof(item *) * c->isize * 2);
                    if (new_list) {
                        c->isize *= 2;
                        c->ilist = new_list;
                    } else { 
                        item_free(it);
                        it = NULL;
                        break;
                    }
                }

                /*
                 * Construct the response. Each hit adds three elements to the
                 * outgoing data list:
                 *   "VALUE "
                 *   key
                 *   " " + flags + " " + data length + "\r\n" + data (with \r\n)
                 */

                if (add_iov(c, "VALUE ", 6) != 0 ||
                   add_iov(c, ITEM_key(it), it->nkey) != 0 ||
                   add_iov(c, ITEM_suffix(it), it->nsuffix + it->nbytes) != 0)
                   {
                       item_free(it);
                       it = NULL;
                       break;
                   }

                if (settings.verbose > 1)
                    fprintf(stderr, ">%d sending key %s\n", c->sfd, ITEM_key(it));

                stats_get_hits++;
                *(c->ilist + i) = it;
                i++;

            } else {
                stats_get_misses++;
            }

            key_token++;
        }

        /*
         * If the command string hasn't been fully processed, get the next set
         * of tokens.
         */
        if(key_token->value != NULL) {
            ntokens = tokenize_command(key_token->value, tokens, MAX_TOKENS);
            key_token = tokens;
        }

    } while(key_token->value != NULL);

    c->icurr = c->ilist;
    c->ileft = i;

    if (settings.verbose > 1)
        fprintf(stderr, ">%d END\n", c->sfd);

    /*
        If the loop was terminated because of out-of-memory, it is not
        reliable to add END\r\n to the buffer, because it might not end
        in \r\n. So we send SERVER_ERROR instead.
    */
    if (key_token->value != NULL || add_iov(c, "END\r\n", 5) != 0
        || (c->udp && build_udp_headers(c) != 0)) {
        out_string(c, "SERVER_ERROR out of memory writing get response");
    }
    else {
        conn_set_state(c, conn_mwrite);
        c->msgcurr = 0;
    }

    STATS_LOCK();
    stats.get_cmds   += stats_get_cmds;
    stats.get_hits   += stats_get_hits;
    stats.get_misses += stats_get_misses;
    STATS_UNLOCK();

    return;
}

static inline void process_rget_command(conn *c, token_t *tokens, size_t ntokens) {
    char *start;
    size_t nstart;
    char *end;
    size_t nend;
    bool is_left_open;
    bool is_right_open;
    uint32_t max_items;
    
    DB_TXN *txn = NULL;
    DBC *cursorp = NULL;
    
    int i = 0;
    int ret = 0;
    item *it = NULL;
    bool is_left_checked = false;

    assert(c != NULL);
    
    start = tokens[1].value;
    nstart = tokens[1].length;
    end = tokens[2].value;
    nend = tokens[2].length;

    if(nstart > KEY_MAX_LENGTH || nend > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }
    
    is_left_open = (tokens[3].value[0] == '1');
    is_right_open = (tokens[4].value[0] == '1');
        
    max_items = strtoul(tokens[5].value, NULL, 10);
    if(errno == ERANGE || max_items > RGET_MAX_ITEMS) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }
    
    /* transcation and cursor */
    ret = env->txn_begin(env, NULL, &txn, 0);
    if (ret != 0) {
        fprintf(stderr, "envp->txn_begin: %s\n", db_strerror(ret));
        out_string(c, "SERVER_ERROR envp->txn_begin");
        return;
    }
    
    /* Get a cursor, we use 2 degree isolation */
    ret = dbp->cursor(dbp, txn, &cursorp, DB_READ_COMMITTED); 
    if (ret != 0) {
        fprintf(stderr, "dbp->cursor: %s\n", db_strerror(ret));
        out_string(c, "SERVER_ERROR dbp->cursor");
        return;
    }
    
    it = item_cget(cursorp, start, nstart, DB_SET_RANGE);

    while(it) {
        /* skip first item? */
        if (!is_left_checked && is_left_open &&
             0 == bdb_defcmp(start, nstart, ITEM_key(it), it->nkey)){
            item_free(it);
            it = NULL;
            is_left_checked = true;
            it = item_cget(cursorp, NULL, 0, DB_NEXT);
            continue;
        }
    
        /* got the end? */
        ret = bdb_defcmp(end, nend, ITEM_key(it), it->nkey);
        if (ret < 0 || (ret == 0 && is_right_open)){
            item_free(it);
            it = NULL;
            break;
        }
                
        /* let vaild item out */
        if (i >= c->isize) {
            item **new_list = realloc(c->ilist, sizeof(item *) * c->isize * 2);
            if (new_list) {
                c->isize *= 2;
                c->ilist = new_list;
            } else { 
                item_free(it);
                it = NULL;
                break;
            }
        }
        
        /*
         * Construct the response. Each hit adds three elements to the
         * outgoing data list:
         *   "VALUE "
         *   key
         *   " " + flags + " " + data length + "\r\n" + data (with \r\n)
         */
        
        if (add_iov(c, "VALUE ", 6) != 0 ||
           add_iov(c, ITEM_key(it), it->nkey) != 0 ||
           add_iov(c, ITEM_suffix(it), it->nsuffix + it->nbytes) != 0)
           {
               item_free(it);
               it = NULL;
               break;
           }
        
        if (settings.verbose > 1)
            fprintf(stderr, ">%d sending key %s\n", c->sfd, ITEM_key(it));
        
        *(c->ilist + i) = it;
        i++;
        /* got enough? */
        if (i == max_items){
            break;
        }
        /* move to the next item */    
        it = item_cget(cursorp, NULL, 0, DB_NEXT);
    }
    
    if (cursorp != NULL){
        cursorp->close(cursorp);
    }

    /* txn commit */
    ret = txn->commit(txn, 0);
    if (ret != 0) {
        fprintf(stderr, "txn->commit: %s\n", db_strerror(ret));
        txn->abort(txn);
    }
    
    c->icurr = c->ilist;
    c->ileft = i;

    if (settings.verbose > 1)
        fprintf(stderr, ">%d END\n", c->sfd);

    /*
        If the loop was terminated because of out-of-memory, it is not
        reliable to add END\r\n to the buffer, because it might not end
        in \r\n. So we send SERVER_ERROR instead.
    */
    if (add_iov(c, "END\r\n", 5) != 0
        || (c->udp && build_udp_headers(c) != 0)) {
        out_string(c, "SERVER_ERROR out of memory writing rget response");
    } else {
        conn_set_state(c, conn_mwrite);
        c->msgcurr = 0;
    }

    return;
}

static void process_update_command(conn *c, token_t *tokens, const size_t ntokens, int comm) {
    char *key;
    size_t nkey;
    int flags;
    time_t exptime;
    int vlen;
    item *it = NULL;

    assert(c != NULL);

    if (tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    flags = strtoul(tokens[2].value, NULL, 10);
    exptime = strtol(tokens[3].value, NULL, 10);
    vlen = strtol(tokens[4].value, NULL, 10);

    if(errno == ERANGE || ((flags == 0 || exptime == 0) && errno == EINVAL)) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    it = item_alloc1(key, nkey, flags, vlen+2);

    if (it == NULL) {
        out_string(c, "SERVER_ERROR out of memory storing object");
        /* swallow the data line */
        c->write_and_go = conn_swallow;
        c->sbytes = vlen + 2;
        return;
    }

    c->item = it;
    c->ritem = ITEM_data(it);
    c->rlbytes = it->nbytes;
    c->item_comm = comm;
    conn_set_state(c, conn_nread);
}

static void process_arithmetic_command(conn *c, token_t *tokens, const size_t ntokens, const bool incr) {
    char temp[sizeof("18446744073709551615")];
    int64_t delta;
    char *key;
    size_t nkey;

    assert(c != NULL);

    if(tokens[KEY_TOKEN].length > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;

    delta = strtoll(tokens[2].value, NULL, 10);

    if(errno == ERANGE) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }

    out_string(c, add_delta(incr, delta, temp, key, nkey));
}

/*
 * adds a delta value to a numeric item.
 *
 * it    item to adjust
 * incr  true to increment value, false to decrement
 * delta amount to adjust value by
 * buf   buffer for response string
 *
 * returns a response string to send back to the client.
 */
char *do_add_delta(const bool incr, const int64_t delta, char *buf, char *key, size_t nkey) {
    char *ptr;
    int64_t value;
    int vlen, flags, ret;
    item *old_it = NULL;
    item *new_it = NULL;

    /* get orignal item */
    old_it = item_get(key, nkey);
    if (old_it == NULL){
        return "NOT_FOUND";
    }

    /* get orignal digital value */
    ptr = ITEM_data(old_it);
    while ((*ptr != '\0') && (*ptr < '0' && *ptr > '9')) ptr++;   // BUG: can't be true
    /* non-digit will cause strtoull stop :) This is a triky. */
    value = strtoull(ptr, NULL, 10);
    if(errno == ERANGE) {
        return "CLIENT_ERROR cannot increment or decrement non-numeric value";
    }

    /* cacl new value */
    if (incr)
        value += delta;
    else {
        if (delta >= value) value = 0;
        else value -= delta;
    }
    vlen = sprintf(buf, "%llu", value);

    /* flags was already lost - so recover them from ITEM_suffix(it) */
    flags = (int) strtol(ITEM_suffix(old_it), (char **) NULL, 10);
            
    /* construct new item */
    new_it = item_alloc1(key, nkey, flags, vlen + 2);
    if (new_it == NULL) {
        /* SERVER_ERROR out of memory */
        if (old_it != NULL)
            item_free(old_it);
        return "SERVER_ERROR out of memory processing arithmetic";
    }
    memcpy(ITEM_data(new_it), buf, vlen);
    memcpy(ITEM_data(new_it) + vlen, "\r\n", 2);
    
    /* put new item into storage */
    ret = item_put(key, nkey, new_it);

    if (old_it != NULL)
        item_free(old_it);
    if (old_it != NULL)
        item_free(new_it);

    if (ret != 0) {
        return "SERVER_ERROR while put a item";
    }   
    return buf;
}

static void process_delete_command(conn *c, token_t *tokens, const size_t ntokens) {
    char *key;
    size_t nkey;
    int ret;
    assert(c != NULL);
    key = tokens[KEY_TOKEN].value;
    nkey = tokens[KEY_TOKEN].length;
    if(nkey > KEY_MAX_LENGTH) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }
    switch (ret = item_delete(key, nkey)) {
    case 0:
        out_string(c, "DELETED");
        break;
    case 1:
        out_string(c, "NOT_FOUND");
        break;
    case -1:
        out_string(c, "SERVER_ERROR while delete a item");
        break;
    default:
        out_string(c, "SERVER_ERROR nothing to do");
    }
    return;
}

static void process_verbosity_command(conn *c, token_t *tokens, const size_t ntokens) {
    unsigned int level;

    assert(c != NULL);

    level = strtoul(tokens[1].value, NULL, 10);
    if(errno == ERANGE) {
        out_string(c, "CLIENT_ERROR bad command line format");
        return;
    }
    settings.verbose = level > MAX_VERBOSITY_LEVEL ? MAX_VERBOSITY_LEVEL : level;
    out_string(c, "OK");
    return;
}

static void process_rep_command(conn *c, token_t *tokens, const size_t ntokens) {

    assert(c != NULL);

    if (bdb_settings.is_replicated){
        if (ntokens == 3 && strcmp(tokens[COMMAND_TOKEN].value, "rep_set_priority") == 0){
            int priority;
            priority = strtoul(tokens[1].value, NULL, 10);
            if(errno == ERANGE || priority < 0) {
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }
            bdb_settings.rep_priority = priority > MAX_REP_PRIORITY ? MAX_REP_PRIORITY : priority;
            if (env->rep_set_priority(env, bdb_settings.rep_priority) != 0){
                out_string(c, "SERVER_ERROR env->rep_set_priority");
                return;
            }
            out_string(c, "OK");
            return;

        } else if (ntokens == 3 && strcmp(tokens[COMMAND_TOKEN].value, "rep_set_ack_policy") == 0){
            int ack_policy;
            ack_policy = strtoul(tokens[1].value, NULL, 10);
            if(errno == ERANGE || ack_policy <= 0) {
                out_string(c, "CLIENT_ERROR bad command line format");
                return;
            }
            bdb_settings.rep_ack_policy = ack_policy > MAX_REP_ACK_POLICY ? MAX_REP_ACK_POLICY : ack_policy;
            if (env->repmgr_set_ack_policy(env, bdb_settings.rep_ack_policy) != 0){
                out_string(c, "SERVER_ERROR env->repmgr_set_ack_policy");
                return;
            }
            out_string(c, "OK");
            return;

        }else {
            out_string(c, "ERROR");
        }
    } else {
        out_string(c, "ERROR");
    }
    return;
}

static void process_bdb_command(conn *c, token_t *tokens, const size_t ntokens) {
	int ret;
    assert(c != NULL);

    if (strcmp(tokens[COMMAND_TOKEN].value, "db_archive") == 0){
        if(0 != (ret = env->log_archive(env, NULL, DB_ARCH_REMOVE))){
            if (settings.verbose > 1) {
                fprintf(stderr, "env->log_archive: %s\n", db_strerror(ret));
			}
            out_string(c, "ERROR");
        }else{
            out_string(c, "OK");
        }
        return;
    
    }else if (strcmp(tokens[COMMAND_TOKEN].value, "db_checkpoint") == 0){
        if(0 != (ret = env->txn_checkpoint(env, 0, 0, 0))){
            if (settings.verbose > 1) {
                fprintf(stderr, "env->txn_checkpoint: %s\n", db_strerror(ret));
			}
            out_string(c, "ERROR");
        }else{
            out_string(c, "OK");
        }
        return;
    
    }else if (strcmp(tokens[COMMAND_TOKEN].value, "db_compact") == 0){
		DB_COMPACT c_data;
        if(0 != (ret = dbp->compact(dbp, NULL, NULL, NULL, &c_data, DB_FREE_SPACE, NULL))){
            if (settings.verbose > 1) {
                fprintf(stderr, "dbp->compact: %s\n", db_strerror(ret));
			}
            out_string(c, "ERROR");
        }else{
            out_string(c, "OK");
        }
        return;
    }else {
        out_string(c, "ERROR");
    }
    return;
}


static void process_command(conn *c, char *command) {

    token_t tokens[MAX_TOKENS];
    size_t ntokens;
    int comm;

    assert(c != NULL);

    if (settings.verbose > 1)
        fprintf(stderr, "<%d %s\n", c->sfd, command);

    /*
     * for commands set/add/replace, we build an item and read the data
     * directly into it, then continue in nread_complete().
     */

    c->msgcurr = 0;
    c->msgused = 0;
    c->iovused = 0;
    if (add_msghdr(c) != 0) {
        out_string(c, "SERVER_ERROR out of memory preparing response");
        return;
    }

    ntokens = tokenize_command(command, tokens, MAX_TOKENS);
    if (ntokens >= 3 &&
        (strcmp(tokens[COMMAND_TOKEN].value, "get") == 0) ) {

        process_get_command(c, tokens, ntokens);

    } else if ((ntokens == 6 || ntokens == 7) &&
               ((strcmp(tokens[COMMAND_TOKEN].value, "add") == 0 && (comm = NREAD_ADD)) ||
                (strcmp(tokens[COMMAND_TOKEN].value, "set") == 0 && (comm = NREAD_SET)) ||
                (strcmp(tokens[COMMAND_TOKEN].value, "replace") == 0 && (comm = NREAD_REPLACE)) ||
                (strcmp(tokens[COMMAND_TOKEN].value, "prepend") == 0 && (comm = NREAD_PREPEND)) ||
                (strcmp(tokens[COMMAND_TOKEN].value, "append") == 0 && (comm = NREAD_APPEND)) )) {

        process_update_command(c, tokens, ntokens, comm);

    } else if (ntokens == 7 && (strcmp(tokens[COMMAND_TOKEN].value, "rget") == 0)) {
    
        process_rget_command(c, tokens, ntokens);
    
    } else if (ntokens == 4 && (strcmp(tokens[COMMAND_TOKEN].value, "incr") == 0)) {

        process_arithmetic_command(c, tokens, ntokens, 1);

    } else if (ntokens == 4 && (strcmp(tokens[COMMAND_TOKEN].value, "decr") == 0)) {

        process_arithmetic_command(c, tokens, ntokens, 0);

    } else if (ntokens >= 3 && ntokens <= 4 && (strcmp(tokens[COMMAND_TOKEN].value, "delete") == 0)) {

        process_delete_command(c, tokens, ntokens);

    } else if (ntokens >= 2 && (strcmp(tokens[COMMAND_TOKEN].value, "stats") == 0)) {

        process_stat(c, tokens, ntokens);

    } else if (ntokens >= 2 && ntokens <= 3 && (strcmp(tokens[COMMAND_TOKEN].value, "flush_all") == 0)) {
        out_string(c, "OK");
        return;

    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "version") == 0)) {

        out_string(c, "VERSION " VERSION);

    } else if (ntokens == 2 && (strcmp(tokens[COMMAND_TOKEN].value, "quit") == 0)) {

        conn_set_state(c, conn_closing);

    } else if (ntokens == 3 && (strcmp(tokens[COMMAND_TOKEN].value, "verbosity") == 0)) {

        process_verbosity_command(c, tokens, ntokens);

    } else if (ntokens == 3 && ((strcmp(tokens[COMMAND_TOKEN].value, "rep_set_ack_policy") == 0) ||
                                (strcmp(tokens[COMMAND_TOKEN].value, "rep_set_priority") == 0) )) {

        process_rep_command(c, tokens, ntokens);

    } else if (ntokens == 2 && 
              ((strcmp(tokens[COMMAND_TOKEN].value, "db_archive") == 0 ) ||
               (strcmp(tokens[COMMAND_TOKEN].value, "db_checkpoint") == 0 ) ||
               (strcmp(tokens[COMMAND_TOKEN].value, "db_compact") == 0 ))) {

        process_bdb_command(c, tokens, ntokens);

    } else {
        out_string(c, "ERROR");
    }
    return;
}

/*
 * if we have a complete line in the buffer, process it.
 */
static int try_read_command(conn *c) {
    char *el, *cont;

    assert(c != NULL);
    assert(c->rcurr <= (c->rbuf + c->rsize));

    if (c->rbytes == 0)
        return 0;
    el = memchr(c->rcurr, '\n', c->rbytes);
    if (!el)
        return 0;
    cont = el + 1;
    if ((el - c->rcurr) > 1 && *(el - 1) == '\r') {
        el--;
    }
    *el = '\0';

    assert(cont <= (c->rcurr + c->rbytes));

    process_command(c, c->rcurr);

    c->rbytes -= (cont - c->rcurr);
    c->rcurr = cont;

    assert(c->rcurr <= (c->rbuf + c->rsize));

    return 1;
}

/*
 * read a UDP request.
 * return 0 if there's nothing to read.
 */
static int try_read_udp(conn *c) {
    int res;

    assert(c != NULL);

    c->request_addr_size = sizeof(c->request_addr);
    res = recvfrom(c->sfd, c->rbuf, c->rsize,
                   0, &c->request_addr, &c->request_addr_size);
    if (res > 8) {
        unsigned char *buf = (unsigned char *)c->rbuf;
        STATS_LOCK();
        stats.bytes_read += res;
        STATS_UNLOCK();

        /* Beginning of UDP packet is the request ID; save it. */
        c->request_id = buf[0] * 256 + buf[1];

        /* If this is a multi-packet request, drop it. */
        if (buf[4] != 0 || buf[5] != 1) {
            out_string(c, "SERVER_ERROR multi-packet request not supported");
            return 0;
        }

        /* Don't care about any of the rest of the header. */
        res -= 8;
        memmove(c->rbuf, c->rbuf + 8, res);

        c->rbytes += res;
        c->rcurr = c->rbuf;
        return 1;
    }
    return 0;
}

/*
 * read from network as much as we can, handle buffer overflow and connection
 * close.
 * before reading, move the remaining incomplete fragment of a command
 * (if any) to the beginning of the buffer.
 * return 0 if there's nothing to read on the first read.
 */
static int try_read_network(conn *c) {
    int gotdata = 0;
    int res;

    assert(c != NULL);

    if (c->rcurr != c->rbuf) {
        if (c->rbytes != 0) /* otherwise there's nothing to copy */
            memmove(c->rbuf, c->rcurr, c->rbytes);
        c->rcurr = c->rbuf;
    }

    while (1) {
		int avail;
        if (c->rbytes >= c->rsize) {
            char *new_rbuf = realloc(c->rbuf, c->rsize * 2);
            if (!new_rbuf) {
                if (settings.verbose > 0)
                    fprintf(stderr, "Couldn't realloc input buffer\n");
                c->rbytes = 0; /* ignore what we read */
                out_string(c, "SERVER_ERROR out of memory reading request");
                c->write_and_go = conn_closing;
                return 1;
            }
            c->rcurr = c->rbuf = new_rbuf;
            c->rsize *= 2;
        }

        /* unix socket mode doesn't need this, so zeroed out.  but why
         * is this done for every command?  presumably for UDP
         * mode.  */
        if (!settings.socketpath) {
            c->request_addr_size = sizeof(c->request_addr);
        } else {
            c->request_addr_size = 0;
        }

        avail = c->rsize - c->rbytes;
        res = read(c->sfd, c->rbuf + c->rbytes, avail);
        if (res > 0) {
            STATS_LOCK();
            stats.bytes_read += res;
            STATS_UNLOCK();
            gotdata = 1;
            c->rbytes += res;
            if (res == avail) {
                continue;
            } else {
                break;
            }
        }
        if (res == 0) {
            /* connection closed */
            conn_set_state(c, conn_closing);
            return 1;
        }
        if (res == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            /* Should close on unhandled errors. */
            conn_set_state(c, conn_closing);
            return 1;
        }
    }
    return gotdata;
}

static bool update_event(conn *c, const int new_flags) {
	struct event_base *base;
    assert(c != NULL);

    base = c->event.ev_base;
    if (c->ev_flags == new_flags)
        return true;
    if (event_del(&c->event) == -1) return false;
    event_set(&c->event, c->sfd, new_flags, event_handler, (void *)c);
    event_base_set(base, &c->event);
    c->ev_flags = new_flags;
    if (event_add(&c->event, 0) == -1) return false;
    return true;
}

/*
 * Sets whether we are listening for new connections or not.
 */
void accept_new_conns(const bool do_accept) {
    conn *next;

    if (! is_listen_thread())
        return;

    for (next = listen_conn; next; next = next->next) {
        if (do_accept) {
            update_event(next, EV_READ | EV_PERSIST);
            if (listen(next->sfd, 1024) != 0) {
                perror("listen");
            }
        }
        else {
            update_event(next, 0);
            if (listen(next->sfd, 0) != 0) {
                perror("listen");
            }
        }
    }
}


/*
 * Transmit the next chunk of data from our list of msgbuf structures.
 *
 * Returns:
 *   TRANSMIT_COMPLETE   All done writing.
 *   TRANSMIT_INCOMPLETE More data remaining to write.
 *   TRANSMIT_SOFT_ERROR Can't write any more right now.
 *   TRANSMIT_HARD_ERROR Can't write (c->state is set to conn_closing)
 */
static int transmit(conn *c) {
    assert(c != NULL);

    if (c->msgcurr < c->msgused &&
            c->msglist[c->msgcurr].msg_iovlen == 0) {
        /* Finished writing the current msg; advance to the next. */
        c->msgcurr++;
    }
    if (c->msgcurr < c->msgused) {
        ssize_t res;
        struct msghdr *m = &c->msglist[c->msgcurr];

        res = sendmsg(c->sfd, m, 0);
        if (res > 0) {
            STATS_LOCK();
            stats.bytes_written += res;
            STATS_UNLOCK();

            /* We've written some of the data. Remove the completed
               iovec entries from the list of pending writes. */
            while (m->msg_iovlen > 0 && res >= m->msg_iov->iov_len) {
                res -= m->msg_iov->iov_len;
                m->msg_iovlen--;
                m->msg_iov++;
            }

            /* Might have written just part of the last iovec entry;
               adjust it so the next write will do the rest. */
            if (res > 0) {
                m->msg_iov->iov_base += res;
                m->msg_iov->iov_len -= res;
            }
            return TRANSMIT_INCOMPLETE;
        }
        if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (!update_event(c, EV_WRITE | EV_PERSIST)) {
                if (settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                return TRANSMIT_HARD_ERROR;
            }
            return TRANSMIT_SOFT_ERROR;
        }
        /* if res==0 or res==-1 and error is not EAGAIN or EWOULDBLOCK,
           we have a real error, on which we close the connection */
        if (settings.verbose > 0)
            perror("Failed to write, and not due to blocking");

        if (c->udp)
            conn_set_state(c, conn_read);
        else
            conn_set_state(c, conn_closing);
        return TRANSMIT_HARD_ERROR;
    } else {
        return TRANSMIT_COMPLETE;
    }
}

static void drive_machine(conn *c) {
    bool stop = false;
    int sfd, flags = 1;
    socklen_t addrlen;
    struct sockaddr_storage addr;
    int res;

    assert(c != NULL);

    while (!stop) {

        switch(c->state) {
        case conn_listening:
            addrlen = sizeof(addr);
            if ((sfd = accept(c->sfd, (struct sockaddr *)&addr, &addrlen)) == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    /* these are transient, so don't log anything */
                    stop = true;
                } else if (errno == EMFILE) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Too many open connections\n");
                    accept_new_conns(false);
                    stop = true;
                } else {
                    perror("accept()");
                    stop = true;
                }
                break;
            }
            if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
                fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
                perror("setting O_NONBLOCK");
                close(sfd);
                break;
            }
            dispatch_conn_new(sfd, conn_read, EV_READ | EV_PERSIST,
                                     DATA_BUFFER_SIZE, false);
            break;

        case conn_read:
            if (try_read_command(c) != 0) {
                continue;
            }
            if ((c->udp ? try_read_udp(c) : try_read_network(c)) != 0) {
                continue;
            }
            /* we have no command line and no data to read from network */
            if (!update_event(c, EV_READ | EV_PERSIST)) {
                if (settings.verbose > 0)
                    fprintf(stderr, "Couldn't update event\n");
                conn_set_state(c, conn_closing);
                break;
            }
            stop = true;
            break;

        case conn_nread:
            /* we are reading rlbytes into ritem; */
            if (c->rlbytes == 0) {
                complete_nread(c);
                break;
            }
            /* first check if we have leftovers in the conn_read buffer */
            if (c->rbytes > 0) {
                int tocopy = c->rbytes > c->rlbytes ? c->rlbytes : c->rbytes;
                memcpy(c->ritem, c->rcurr, tocopy);
                c->ritem += tocopy;
                c->rlbytes -= tocopy;
                c->rcurr += tocopy;
                c->rbytes -= tocopy;
                break;
            }

            /*  now try reading from the socket */
            res = read(c->sfd, c->ritem, c->rlbytes);
            if (res > 0) {
                STATS_LOCK();
                stats.bytes_read += res;
                STATS_UNLOCK();
                c->ritem += res;
                c->rlbytes -= res;
                break;
            }
            if (res == 0) { /* end of stream */
                conn_set_state(c, conn_closing);
                break;
            }
            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
                stop = true;
                break;
            }
            /* otherwise we have a real error, on which we close the connection */
            if (settings.verbose > 0)
                fprintf(stderr, "Failed to read, and not due to blocking\n");
            conn_set_state(c, conn_closing);
            break;

        case conn_swallow:
            /* we are reading sbytes and throwing them away */
            if (c->sbytes == 0) {
                conn_set_state(c, conn_read);
                break;
            }

            /* first check if we have leftovers in the conn_read buffer */
            if (c->rbytes > 0) {
                int tocopy = c->rbytes > c->sbytes ? c->sbytes : c->rbytes;
                c->sbytes -= tocopy;
                c->rcurr += tocopy;
                c->rbytes -= tocopy;
                break;
            }

            /*  now try reading from the socket */
            res = read(c->sfd, c->rbuf, c->rsize > c->sbytes ? c->sbytes : c->rsize);
            if (res > 0) {
                STATS_LOCK();
                stats.bytes_read += res;
                STATS_UNLOCK();
                c->sbytes -= res;
                break;
            }
            if (res == 0) { /* end of stream */
                conn_set_state(c, conn_closing);
                break;
            }
            if (res == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!update_event(c, EV_READ | EV_PERSIST)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't update event\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
                stop = true;
                break;
            }
            /* otherwise we have a real error, on which we close the connection */
            if (settings.verbose > 0)
                fprintf(stderr, "Failed to read, and not due to blocking\n");
            conn_set_state(c, conn_closing);
            break;

        case conn_write:
            /*
             * We want to write out a simple response. If we haven't already,
             * assemble it into a msgbuf list (this will be a single-entry
             * list for TCP or a two-entry list for UDP).
             */
            if (c->iovused == 0 || (c->udp && c->iovused == 1)) {
                if (add_iov(c, c->wcurr, c->wbytes) != 0 ||
                    (c->udp && build_udp_headers(c) != 0)) {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Couldn't build response\n");
                    conn_set_state(c, conn_closing);
                    break;
                }
            }

            /* fall through... */

        case conn_mwrite:
            switch (transmit(c)) {
            case TRANSMIT_COMPLETE:
                if (c->state == conn_mwrite) {
                    while (c->ileft > 0) {
                        item *it = *(c->icurr);
                        item_free(it);
                        c->icurr++;
                        c->ileft--;
                    }
                    conn_set_state(c, conn_read);
                } else if (c->state == conn_write) {
                    if (c->write_and_free) {
                        free(c->write_and_free);
                        c->write_and_free = 0;
                    }
                    conn_set_state(c, c->write_and_go);
                } else {
                    if (settings.verbose > 0)
                        fprintf(stderr, "Unexpected state %d\n", c->state);
                    conn_set_state(c, conn_closing);
                }
                break;

            case TRANSMIT_INCOMPLETE:
            case TRANSMIT_HARD_ERROR:
                break;                   /* Continue in state machine. */

            case TRANSMIT_SOFT_ERROR:
                stop = true;
                break;
            }
            break;

        case conn_closing:
            if (c->udp)
                conn_cleanup(c);
            else
                conn_close(c);
            stop = true;
            break;
        }
    }

    return;
}

void event_handler(const int fd, const short which, void *arg) {
    conn *c;

    c = (conn *)arg;
    assert(c != NULL);

    c->which = which;

    /* sanity */
    if (fd != c->sfd) {
        if (settings.verbose > 0)
            fprintf(stderr, "Catastrophic: event fd doesn't match conn fd!\n");
        conn_close(c);
        return;
    }

    drive_machine(c);

    /* wait for next event */
    return;
}

static int new_socket(struct addrinfo *ai) {
    int sfd;
    int flags;

    if ((sfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol)) == -1) {
        perror("socket()");
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("setting O_NONBLOCK");
        close(sfd);
        return -1;
    }
    return sfd;
}


/*
 * Sets a socket's send buffer size to the maximum allowed by the system.
 */
static void maximize_sndbuf(const int sfd) {
    socklen_t intsize = sizeof(int);
    int last_good = 0;
    int min, max, avg;
    int old_size;

    /* Start with the default size. */
    if (getsockopt(sfd, SOL_SOCKET, SO_SNDBUF, &old_size, &intsize) != 0) {
        if (settings.verbose > 0)
            perror("getsockopt(SO_SNDBUF)");
        return;
    }

    /* Binary-search for the real maximum. */
    min = old_size;
    max = MAX_SENDBUF_SIZE;

    while (min <= max) {
        avg = ((unsigned int)(min + max)) / 2;
        if (setsockopt(sfd, SOL_SOCKET, SO_SNDBUF, (void *)&avg, intsize) == 0) {
            last_good = avg;
            min = avg + 1;
        } else {
            max = avg - 1;
        }
    }

    if (settings.verbose > 1)
        fprintf(stderr, "<%d send buffer was %d, now %d\n", sfd, old_size, last_good);
}

static int server_socket(const int port, const bool is_udp) {
    int sfd;
    struct linger ling = {0, 0};
    struct addrinfo *ai;
    struct addrinfo *next;
    struct addrinfo hints;
    char port_buf[NI_MAXSERV];
    int error;
    int success = 0;

    int flags =1;

    /*
     * the memset call clears nonstandard fields in some impementations
     * that otherwise mess things up.
     */
    memset(&hints, 0, sizeof (hints));
    hints.ai_flags = AI_PASSIVE|AI_ADDRCONFIG;
    if (is_udp)
    {
        hints.ai_protocol = IPPROTO_UDP;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_family = AF_INET; /* This left here because of issues with OSX 10.5 */
    } else {
        hints.ai_family = AF_UNSPEC;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_socktype = SOCK_STREAM;
    }

    snprintf(port_buf, NI_MAXSERV, "%d", port);
    error= getaddrinfo(settings.inter, port_buf, &hints, &ai);
    if (error != 0) {
      if (error != EAI_SYSTEM)
        fprintf(stderr, "getaddrinfo(): %s\n", gai_strerror(error));
      else
        perror("getaddrinfo()");

      return 1;
    }

    for (next= ai; next; next= next->ai_next) {
        conn *listen_conn_add;
        if ((sfd = new_socket(next)) == -1) {
            freeaddrinfo(ai);
            return 1;
        }

        setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
        if (is_udp) {
            maximize_sndbuf(sfd);
        } else {
            setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
            setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));
            setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));
        }

        if (bind(sfd, next->ai_addr, next->ai_addrlen) == -1) {
            if (errno != EADDRINUSE) {
                perror("bind()");
                close(sfd);
                freeaddrinfo(ai);
                return 1;
            }
            close(sfd);
            continue;
        } else {
          success++;
          if (!is_udp && listen(sfd, 1024) == -1) {
              perror("listen()");
              close(sfd);
              freeaddrinfo(ai);
              return 1;
          }
      }

      if (is_udp)
      {
        int c;

        for (c = 0; c < settings.num_threads; c++) {
            /* this is guaranteed to hit all threads because we round-robin */
            dispatch_conn_new(sfd, conn_read, EV_READ | EV_PERSIST,
                              UDP_READ_BUFFER_SIZE, 1);
        }
      } else {
        if (!(listen_conn_add = conn_new(sfd, conn_listening,
                                         EV_READ | EV_PERSIST, 1, false, main_base))) {
            fprintf(stderr, "failed to create listening connection\n");
            exit(EXIT_FAILURE);
        }

        listen_conn_add->next = listen_conn;
        listen_conn = listen_conn_add;
      }
    }

    freeaddrinfo(ai);

    /* Return zero iff we detected no errors in starting up connections */
    return success == 0;
}

static int new_socket_unix(void) {
    int sfd;
    int flags;

    if ((sfd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        perror("socket()");
        return -1;
    }

    if ((flags = fcntl(sfd, F_GETFL, 0)) < 0 ||
        fcntl(sfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("setting O_NONBLOCK");
        close(sfd);
        return -1;
    }
    return sfd;
}

#ifndef WIN32
static int server_socket_unix(const char *path, int access_mask) {
    int sfd;
    struct linger ling = {0, 0};
    struct sockaddr_un addr;
    struct stat tstat;
    int flags =1;
    int old_umask;

    if (!path) {
        return 1;
    }

    if ((sfd = new_socket_unix()) == -1) {
        return 1;
    }

    /*
     * Clean up a previous socket file if we left it around
     */
    if (lstat(path, &tstat) == 0) {
        if (S_ISSOCK(tstat.st_mode))
            unlink(path);
    }

    setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (void *)&flags, sizeof(flags));
    setsockopt(sfd, SOL_SOCKET, SO_KEEPALIVE, (void *)&flags, sizeof(flags));
    setsockopt(sfd, SOL_SOCKET, SO_LINGER, (void *)&ling, sizeof(ling));

    /*
     * the memset call clears nonstandard fields in some impementations
     * that otherwise mess things up.
     */
    memset(&addr, 0, sizeof(addr));

    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, path);
    old_umask=umask( ~(access_mask&0777));
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        perror("bind()");
        close(sfd);
        umask(old_umask);
        return 1;
    }
    umask(old_umask);
    if (listen(sfd, 1024) == -1) {
        perror("listen()");
        close(sfd);
        return 1;
    }
    if (!(listen_conn = conn_new(sfd, conn_listening,
                                     EV_READ | EV_PERSIST, 1, false, main_base))) {
        fprintf(stderr, "failed to create listening connection\n");
        exit(EXIT_FAILURE);
    }

    return 0;
}
#endif

static void usage(void) {
    printf(PACKAGE " " VERSION "\n");
    printf("-p <num>      TCP port number to listen on (default: 21201)\n"
           "-U <num>      UDP port number to listen on (default: 0, off)\n"
           "-s <file>     unix socket path to listen on (disables network support)\n"
           "-a <mask>     access mask for unix socket, in octal (default 0700)\n"
           "-l <ip_addr>  interface to listen on, default is INDRR_ANY\n"
           "-d            run as a daemon\n"
		   "-d start          tell memcached to start\n"
		   "-d restart        tell running memcached to do a graceful restart\n"
		   "-d stop|shutdown  tell running memcached to shutdown\n"
		   "-d install        install memcached service\n"
		   "-d uninstall      uninstall memcached service\n"
           "-r            maximize core file limit\n"
           "-u <username> assume identity of <username> (only when run as root)\n"
           "-c <num>      max simultaneous connections, default is 4096\n"
           "-b <num>      item size smaller than <num> will use fast memory alloc, default is 2048 bytes\n"
           "-v            verbose (print errors/warnings while in event loop)\n"
           "-vv           very verbose (also print client commands/reponses)\n"
           "-h            print this help and exit\n"
           "-i            print license info\n"
           "-P <file>     save PID in <file>, only used with -d option\n"
           );
#ifdef USE_THREADS
    printf("-t <num>      number of threads to use, default 4\n");
#endif
    printf("--------------------BerkeleyDB Options-------------------------------\n");
    printf("-m <num>      in-memmory cache size of BerkeleyDB in megabytes, default is 256MB\n");
    printf("-A <num>      underlying page size in bytes, default is 4096, (512B ~ 64KB, power-of-two)\n");
    printf("-f <file>     filename of database, default is 'data.db'\n");
    printf("-H <dir>      env home of database, default is '/data1/memcachedb'\n");
    printf("-B <db_type>  type of database, 'btree' or 'hash'. default is 'btree'\n");
    printf("-L <num>      log buffer size in kbytes, default is 4MB\n");
    printf("-C <num>      do checkpoint every <num> seconds, 0 for disable, default is 5 minutes\n");
    printf("-T <num>      do memp_trickle every <num> seconds, 0 for disable, default is 30 seconds\n");
    printf("-e <num>      percent of the pages in the cache that should be clean, default is 60%%\n");
    printf("-D <num>      do deadlock detecting every <num> millisecond, 0 for disable, default is 100ms\n");
    printf("-N            enable DB_TXN_NOSYNC to gain big performance improved, default is off\n");
    printf("-X            allocate region memory from the heap, default is off\n");
    printf("--------------------Replication Options-------------------------------\n");
    printf("-R            identifies the host and port used by this site (required).\n");
    printf("-O            identifies another site participating in this replication group\n");
    printf("-M/-S         start as a master or slave\n");
    printf("-n            number of sites that participat in replication, default is 2\n");
    printf("-----------------------------------------------------------------------\n");

    return;
}

static void usage_license(void) {
    printf(PACKAGE " " VERSION "\n\n");
    printf(
    "Copyright (c) 2008, Steve Chu. <stvchu@gmail.com>\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions are\n"
    "met:\n"
    "\n"
    "    * Redistributions of source code must retain the above copyright\n"
    "notice, this list of conditions and the following disclaimer.\n"
    "\n"
    "    * Redistributions in binary form must reproduce the above\n"
    "copyright notice, this list of conditions and the following disclaimer\n"
    "in the documentation and/or other materials provided with the\n"
    "distribution.\n"
    "\n"
    "    * Neither the name of the Danga Interactive nor the names of its\n"
    "contributors may be used to endorse or promote products derived from\n"
    "this software without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
    "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\n"
    "LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\n"
    "A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\n"
    "OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\n"
    "SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\n"
    "LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
    "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
    "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
    "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
    "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    "\n"
    "\n"
    "This product includes software developed by Danga Interactive, Inc.\n"
    "\n"
    "[ memcached ]\n"
    "\n"
    "Copyright (c) 2003, Danga Interactive, Inc. <http://www.danga.com/>\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions are\n"
    "met:\n"
    "\n"
    "    * Redistributions of source code must retain the above copyright\n"
    "notice, this list of conditions and the following disclaimer.\n"
    "\n"
    "    * Redistributions in binary form must reproduce the above\n"
    "copyright notice, this list of conditions and the following disclaimer\n"
    "in the documentation and/or other materials provided with the\n"
    "distribution.\n"
    "\n"
    "    * Neither the name of the Danga Interactive nor the names of its\n"
    "contributors may be used to endorse or promote products derived from\n"
    "this software without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS\n"
    "\"AS IS\" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT\n"
    "LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR\n"
    "A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT\n"
    "OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,\n"
    "SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT\n"
    "LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
    "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
    "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
    "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE\n"
    "OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    "\n"
    "\n"
    "This product includes software developed by Niels Provos.\n"
    "\n"
    "[ libevent ]\n"
    "\n"
    "Copyright 2000-2003 Niels Provos <provos@citi.umich.edu>\n"
    "All rights reserved.\n"
    "\n"
    "Redistribution and use in source and binary forms, with or without\n"
    "modification, are permitted provided that the following conditions\n"
    "are met:\n"
    "1. Redistributions of source code must retain the above copyright\n"
    "   notice, this list of conditions and the following disclaimer.\n"
    "2. Redistributions in binary form must reproduce the above copyright\n"
    "   notice, this list of conditions and the following disclaimer in the\n"
    "   documentation and/or other materials provided with the distribution.\n"
    "3. All advertising materials mentioning features or use of this software\n"
    "   must display the following acknowledgement:\n"
    "      This product includes software developed by Niels Provos.\n"
    "4. The name of the author may not be used to endorse or promote products\n"
    "   derived from this software without specific prior written permission.\n"
    "\n"
    "THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR\n"
    "IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES\n"
    "OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.\n"
    "IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,\n"
    "INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT\n"
    "NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,\n"
    "DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY\n"
    "THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT\n"
    "(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF\n"
    "THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.\n"
    "\n"
    "\n"
    "This product includes software developed by Oracle Inc.\n"
    "\n"
    "[ BerkeleyDB ]\n"
    "\n"
    "See LICENSE file in BerkeleyDB distribution to get more Copyright info.\n"
    "\n"
    );

    return;
}

static void save_pid(const pid_t pid, const char *pid_file) {
    FILE *fp;
    if (pid_file == NULL)
        return;

    if ((fp = fopen(pid_file, "w")) == NULL) {
        fprintf(stderr, "Could not open the pid file %s for writing\n", pid_file);
        return;
    }

    fprintf(fp,"%ld\n", (long)pid);
    if (fclose(fp) == -1) {
        fprintf(stderr, "Could not close the pid file %s.\n", pid_file);
        return;
    }
}

static void remove_pidfile(const char *pid_file) {
  if (pid_file == NULL)
      return;

  if (unlink(pid_file) != 0) {
      fprintf(stderr, "Could not remove the pid file %s.\n", pid_file);
  }

}

void run_server()
{
	/* enter the loop */
	start_chkpoint_thread();
	start_memp_trickle_thread();
	start_dl_detect_thread();

	/* enter the event loop */
	event_base_loop(main_base, 0);
}

void stop_server()
{
	int ret;

	ret = event_base_loopexit(main_base, 0);
	if (ret == 0)
		fprintf(stderr, "done.\n");
	else
		fprintf(stderr, "error.\n");
	daemon_quit = 1;
	sleep(2);
	bdb_db_close();
	bdb_env_close();
	exit(0);

}
void pause_server()
{
	/* not implemented yet */
}

void continue_server()
{
	/* not implemented yet */
}


/* for safely exit, make sure to do checkpoint*/
static void sig_handler(const int sig)
{
    int ret;
#ifdef WIN32
	if (sig != CTRL_C_EVENT && sig !=  CTRL_CLOSE_EVENT && sig != CTRL_BREAK_EVENT
		&& sig != CTRL_LOGOFF_EVENT && sig != CTRL_SHUTDOWN_EVENT) {
#else
    if (sig != SIGTERM && sig != SIGQUIT && sig != SIGINT) {
#endif
        return;
    }
    if (daemon_quit == 1){
        return;
    }
    daemon_quit = 1;
    fprintf(stderr, "Signal(%d) received, try to exit daemon gracefully..\n", sig);

    /* exit event loop first */
    fprintf(stderr, "exit event base...");    
    ret = event_base_loopexit(main_base, 0);
    if (ret == 0)
      fprintf(stderr, "done.\n");
    else
      fprintf(stderr, "error.\n");

    /* make sure deadlock detect loop is quit*/
    sleep(2);
}

int main (int argc, char **argv) {
    int c;
    struct in_addr addr;
    bool daemonize = false;
    int maxcore = 0;
    char *username = NULL;
    char *pid_file = NULL;

#ifndef WIN32
    struct passwd *pw;
    struct sigaction sa;
    struct rlimit rlim;
#else
    WSADATA wsaData;
#endif
    /* listening socket */
    static int *l_socket = NULL;

    /* udp socket */
    static int *u_socket = NULL;
    static int u_socket_count = 0;

    char *portstr = NULL;

#ifndef WIN32
    /* register signal callback */
    if (signal(SIGTERM, sig_handler) == SIG_ERR)
        fprintf(stderr, "can not catch SIGTERM\n");
    if (signal(SIGQUIT, sig_handler) == SIG_ERR)
        fprintf(stderr, "can not catch SIGQUIT\n");
    if (signal(SIGINT,  sig_handler) == SIG_ERR)
        fprintf(stderr, "can not catch SIGINT\n");
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sig_handler, TRUE );
	if(WSAStartup(MAKEWORD(2,0), &wsaData) != 0) {
		fprintf(stderr, "Socket Initialization Error. Program  aborted\n");
		return;
	}
#endif	

    /* init settings */
    settings_init();
    bdb_settings_init();

    /* get Berkeley DB version*/
    db_version(&(bdb_version.majver), &(bdb_version.minver), &(bdb_version.patch));

    /* set stderr non-buffering (for running under, say, daemontools) */
    setbuf(stderr, NULL);

    /* process arguments */
#ifndef WIN32
    while ((c = getopt(argc, argv, "a:U:p:s:c:hivl:dru:P:t:b:f:H:B:m:A:L:C:T:e:D:NXMSR:O:n:")) != -1) {
#else
	while ((c = getopt(argc, argv, "a:U:p:s:c:hivl:d:ru:P:t:b:f:H:B:m:A:L:C:T:e:D:NXMSR:O:n:")) != -1) {
#endif
        switch (c) {
        case 'a':
            /* access for unix domain socket, as octal mask (like chmod)*/
            settings.access= strtol(optarg,NULL,8);
            break;

        case 'U':
            settings.udpport = atoi(optarg);
            break;
        case 'p':
            settings.port = atoi(optarg);
            break;
        case 's':
            settings.socketpath = optarg;
            break;
        case 'c':
            settings.maxconns = atoi(optarg);
            break;
        case 'h':
            usage();
            exit(EXIT_SUCCESS);
        case 'i':
            usage_license();
            exit(EXIT_SUCCESS);
        case 'v':
            settings.verbose++;
            break;
        case 'l':
            settings.inter= strdup(optarg);
            break;
        case 'd':
            daemonize = true;
#ifdef WIN32
			if(!optarg || !strcmpi(optarg, "runservice")) daemonize = 1;
			else if(!strcmpi(optarg, "start")) daemonize = 2;
			else if(!strcmpi(optarg, "restart")) daemonize = 3;
			else if(!strcmpi(optarg, "stop")) daemonize = 4;
			else if(!strcmpi(optarg, "shutdown")) daemonize = 5;
			else if(!strcmpi(optarg, "install")) daemonize = 6;
			else if(!strcmpi(optarg, "uninstall")) daemonize = 7;
			else fprintf(stderr, "Illegal argument: \"%s\"\n", optarg);
#endif /* WIN32 */

            break;
        case 'r':
            maxcore = 1;
            break;
        case 'u':
            username = optarg;
            break;
        case 'P':
            pid_file = optarg;
            break;
#ifdef USE_THREADS
        case 't':
            settings.num_threads = atoi(optarg);
            if (settings.num_threads == 0) {
                fprintf(stderr, "Number of threads must be greater than 0\n");
                exit(EXIT_FAILURE);
            }
            break;
#endif
        case 'b':
            settings.item_buf_size = atoi(optarg);
            if(settings.item_buf_size < 512){
                fprintf(stderr, "item buf size must be larger than 512 bytes\n");
                exit(EXIT_FAILURE);
            } 
            if(settings.item_buf_size > 256 * 1024){
                fprintf(stderr, "Warning: item buffer size(-b) larger than 256KB may cause performance issue\n");
            } 
            break;
        case 'f':
            bdb_settings.db_file = optarg;
            break;
        case 'H':
            bdb_settings.env_home = optarg;
            break;
        case 'B':
            if (0 == strcmp(optarg, "btree")){
                bdb_settings.db_type = DB_BTREE;
            }else if (0 == strcmp(optarg, "hash")){
                bdb_settings.db_type = DB_HASH;
            }else{
                fprintf(stderr, "Unknown databasetype, only 'btree' or 'hash' is available.\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'm':
            bdb_settings.cache_size = atoi(optarg) * 1024 * 1024;
            break;
        case 'A':
            bdb_settings.page_size = atoi(optarg);
            break;
        case 'L':
            bdb_settings.txn_lg_bsize = atoi(optarg) * 1024;
            break;
        case 'C':
            bdb_settings.chkpoint_val = atoi(optarg);
            break;
        case 'T':
            bdb_settings.memp_trickle_val = atoi(optarg);
            break;
        case 'e':
            bdb_settings.memp_trickle_percent = atoi(optarg);
            if (bdb_settings.memp_trickle_percent < 0 || 
                bdb_settings.memp_trickle_percent > 100){
                fprintf(stderr, "memp_trickle_percent should be 0 ~ 100.\n");
                exit(EXIT_FAILURE);
            }
            break;
        case 'D':
            bdb_settings.dldetect_val = atoi(optarg) * 1000;
            break;
        case 'N':
            bdb_settings.txn_nosync = 1;
            break;
        case 'X':
            bdb_settings.env_flags |= DB_PRIVATE;
            break;
        case 'M':
            if (bdb_settings.rep_start_policy == DB_REP_CLIENT){
                fprintf(stderr, "Can't not be a Master and Slave at same time.\n");
                exit(EXIT_FAILURE);
            }else{
               bdb_settings.rep_start_policy = DB_REP_MASTER;
            }
            break;
       case 'S':
            if (bdb_settings.rep_start_policy == DB_REP_MASTER){
                fprintf(stderr, "Can't not be a Master and Slave at same time.\n");
                exit(EXIT_FAILURE);
            }else{
               bdb_settings.rep_start_policy = DB_REP_CLIENT;
            }
            break;
        case 'R':
           bdb_settings.is_replicated = 1;
           bdb_settings.rep_localhost = strtok(optarg, ":");
            if ((portstr = strtok(NULL, ":")) == NULL) {
                fprintf(stderr, "Bad host specification.\n");
                exit(EXIT_FAILURE);
            }
           bdb_settings.rep_localport = (unsigned short)atoi(portstr);
            break;
        case 'O':
            bdb_settings.rep_remotehost = strtok(optarg, ":");
            if ((portstr = strtok(NULL, ":")) == NULL) {
                fprintf(stderr, "Bad host specification.\n");
                exit(EXIT_FAILURE);
            }
            bdb_settings.rep_remoteport = (unsigned short)atoi(portstr);
            break;
        case 'n':
            bdb_settings.rep_nsites = atoi(optarg);
            break;

        default:
            fprintf(stderr, "Illegal argument \"%c\"\n", c);
            exit(EXIT_FAILURE);
        }
    }

#ifndef WIN32
    if (maxcore != 0) {
        struct rlimit rlim_new;
        /*
         * First try raising to infinity; if that fails, try bringing
         * the soft limit to the hard.
         */
        if (getrlimit(RLIMIT_CORE, &rlim) == 0) {
            rlim_new.rlim_cur = rlim_new.rlim_max = RLIM_INFINITY;
            if (setrlimit(RLIMIT_CORE, &rlim_new)!= 0) {
                /* failed. try raising just to the old max */
                rlim_new.rlim_cur = rlim_new.rlim_max = rlim.rlim_max;
                (void)setrlimit(RLIMIT_CORE, &rlim_new);
            }
        }
        /*
         * getrlimit again to see what we ended up with. Only fail if
         * the soft limit ends up 0, because then no core files will be
         * created at all.
         */

        if ((getrlimit(RLIMIT_CORE, &rlim) != 0) || rlim.rlim_cur == 0) {
            fprintf(stderr, "failed to ensure corefile creation\n");
            exit(EXIT_FAILURE);
        }
    }

    /*
     * If needed, increase rlimits to allow as many connections
     * as needed.
     */

    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        fprintf(stderr, "failed to getrlimit number of files\n");
        exit(EXIT_FAILURE);
    } else {
        int maxfiles = settings.maxconns;
        if (rlim.rlim_cur < maxfiles)
            rlim.rlim_cur = maxfiles + 3;
        if (rlim.rlim_max < rlim.rlim_cur)
            rlim.rlim_max = rlim.rlim_cur;
        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
            fprintf(stderr, "failed to set rlimit for open files. Try running as root or requesting smaller maxconns value.\n");
            exit(EXIT_FAILURE);
        }
    }
#endif /* !WIN32 */

#ifndef WIN32
    /* daemonize if requested */
    /* if we want to ensure our ability to dump core, don't chdir to / */
    if (daemonize) {
        int res;
        res = daemon(maxcore, settings.verbose);
        if (res == -1) {
            fprintf(stderr, "failed to daemon() in order to daemonize\n");
            return 1;
        }
    }
#else
	switch(daemonize) {
		case 2:
			if(!ServiceStart()) {
				fprintf(stderr, "failed to start service\n");
				return 1;
			}
			exit(0);
		case 3:
			if(!ServiceRestart()) {
				fprintf(stderr, "failed to restart service\n");
				return 1;
			}
			exit(0);
		case 4:
		case 5:
			if(!ServiceStop()) {
				fprintf(stderr, "failed to stop service\n");
				return 1;
			}
			exit(0);
		case 6:
			if(!ServiceInstall()) {
				fprintf(stderr, "failed to install service or service already installed\n");
				return 1;
			}
			exit(0);
		case 7:
			if(!ServiceUninstall()) {
				fprintf(stderr, "failed to uninstall service or service not installed\n");
				return 1;
			}
			exit(0);
	}
#endif

#ifndef WIN32
    /* lose root privileges if we have them */
    if (getuid() == 0 || geteuid() == 0) {
        if (username == 0 || *username == '\0') {
            fprintf(stderr, "can't run as root without the -u switch\n");
            return 1;
        }
        if ((pw = getpwnam(username)) == 0) {
            fprintf(stderr, "can't find the user %s to switch to\n", username);
            return 1;
        }
        if (setgid(pw->pw_gid) < 0 || setuid(pw->pw_uid) < 0) {
            fprintf(stderr, "failed to assume identity of user %s\n", username);
            return 1;
        }
    }
#endif

    /* initialize main thread libevent instance */
    main_base = event_init();

    /* initialize other stuff */
    item_init();
    stats_init();
    conn_init();

#ifndef WIN32
    /*
     * ignore SIGPIPE signals; we can use errno==EPIPE if we
     * need that information
     */
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    if (sigemptyset(&sa.sa_mask) == -1 ||
        sigaction(SIGPIPE, &sa, 0) == -1) {
        perror("failed to ignore SIGPIPE; sigaction");
        exit(EXIT_FAILURE);
    }
#endif 
    /* start up worker threads if MT mode */
    thread_init(settings.num_threads, main_base);
    /* save the PID in if we're a daemon, do this after thread_init due to
       a file descriptor handling bug somewhere in libevent */
    if (daemonize)
        save_pid(getpid(), pid_file);
#ifndef WIN32
    /* create unix mode sockets after dropping privileges */
    if (settings.socketpath != NULL) {
        if (server_socket_unix(settings.socketpath,settings.access)) {
          fprintf(stderr, "failed to listen\n");
          exit(EXIT_FAILURE);
        }
    }
#endif

    /* create the listening socket, bind it, and init */
    if (settings.socketpath == NULL) {
        int udp_port;

        if (server_socket(settings.port, 0)) {
            fprintf(stderr, "failed to listen\n");
            exit(EXIT_FAILURE);
        }
        /*
         * initialization order: first create the listening sockets
         * (may need root on low ports), then drop root if needed,
         * then daemonise if needed, then init libevent (in some cases
         * descriptors created by libevent wouldn't survive forking).
         */
        udp_port = settings.udpport ? settings.udpport : settings.port;

        /* create the UDP listening socket and bind it */
        if (server_socket(udp_port, 1)) {
            fprintf(stderr, "failed to listen on UDP port %d\n", settings.udpport);
            exit(EXIT_FAILURE);
        }
    }
    
    /* here we init bdb env and open db */
    bdb_env_init();
    bdb_db_open();

    /* start checkpoint and deadlock detect thread */
#ifdef WIN32
	if (daemonize) {
		ServiceSetFunc(run_server, pause_server, continue_server, stop_server);
		ServiceRun();

	}
	else
#endif /* WIN32 */
	{
		start_chkpoint_thread();
		start_memp_trickle_thread();
		start_dl_detect_thread();

		/* enter the event loop */
		event_base_loop(main_base, 0);

		/* cleanup bdb staff */
		fprintf(stderr, "try to clean up bdb resource...\n");
		bdb_chkpoint();
		bdb_db_close();
		bdb_env_close();

	}
	/* remove the PID file if we're a daemon */
	if (daemonize)
		remove_pidfile(pid_file);

	/* Clean up strdup() call for bind() address */
	if (settings.inter)
		free(settings.inter);
	if (l_socket)
		free(l_socket);
	if (u_socket)
		free(u_socket);

    return 0;
}
