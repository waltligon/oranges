/*
 * (C) 2001 Clemson University and The University of Chicago
 *
 * See COPYING in top-level directory.
 */

#include "pvfs2-config.h"

#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#include <sys/poll.h>
#include <sys/uio.h>

#include "sockio.h"

/* if the platform provides a MSG_NOSIGNAL option (which disables the
 * generation of signals on broken pipe), then use it
 */
#ifdef MSG_NOSIGNAL
#define DEFAULT_MSG_FLAGS MSG_NOSIGNAL
#else
#define DEFAULT_MSG_FLAGS 0
#endif

int BMI_sockio_new_sock()
{
    return(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
}

int BMI_sockio_bind_sock(int sockd,
	      int service)
{
    struct sockaddr_in saddr;

    bzero((char *) &saddr, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons((u_short) service);
    saddr.sin_addr.s_addr = INADDR_ANY;
  bind_sock_restart:
    if (bind(sockd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0)
    {
	if (errno == EINTR)
	    goto bind_sock_restart;
	return (-1);
    }
    return (sockd);
}

int BMI_sockio_connect_sock(int sockd,
		 const char *name,
		 int service)
{
    struct sockaddr saddr;

    if (BMI_sockio_init_sock(&saddr, name, service) != 0)
	return (-1);
  connect_sock_restart:
    if (connect(sockd, (struct sockaddr *) &saddr, sizeof(saddr)) < 0)
    {
	if (errno == EINTR)
	    goto connect_sock_restart;
	return (-1);
    }
    return (sockd);
}

int BMI_sockio_init_sock(struct sockaddr *saddrp,
	      const char *name,
	      int service)
{
    struct hostent *hep;

    bzero((char *) saddrp, sizeof(struct sockaddr_in));
    if (name == NULL)
    {
	if ((hep = gethostbyname("localhost")) == NULL)
	{
	    return (-1);
	}
    }
    else if ((hep = gethostbyname(name)) == NULL)
    {
	return (-1);
    }
    ((struct sockaddr_in *) saddrp)->sin_family = AF_INET;
    ((struct sockaddr_in *) saddrp)->sin_port = htons((u_short) service);
    bcopy(hep->h_addr, (char *) &(((struct sockaddr_in *) saddrp)->sin_addr),
	  hep->h_length);
    return (0);
}


/* blocking receive */
/* Returns -1 if it cannot get all len bytes
 * and the # of bytes received otherwise
 */
int BMI_sockio_brecv(int s,
	  void *buf,
	  int len)
{
    int oldfl, ret, comp = len;
    int olderrno;
    oldfl = fcntl(s, F_GETFL, 0);
    if (oldfl & O_NONBLOCK)
	fcntl(s, F_SETFL, oldfl & (~O_NONBLOCK));

    while (comp)
    {
      brecv_restart:
	if ((ret = recv(s, (char *) buf, comp, DEFAULT_MSG_FLAGS)) < 0)
	{
	    if (errno == EINTR)
		goto brecv_restart;
	    olderrno = errno;
	    fcntl(s, F_SETFL, oldfl|O_NONBLOCK);
	    errno = olderrno;
	    return (-1);
	}
	if (!ret)
	{
	    /* Note: this indicates a closed socket.  However, I don't
	     * like this behavior, so we're going to return -1 w/an EPIPE
	     * instead.
	     */
	    fcntl(s, F_SETFL, oldfl|O_NONBLOCK);
	    errno = EPIPE;
	    return (-1);
	}
	comp -= ret;
	buf += ret;
    }
    fcntl(s, F_SETFL, oldfl|O_NONBLOCK);
    return (len - comp);
}

/* nonblocking receive */
int BMI_sockio_nbrecv(int s,
	   void *buf,
	   int len)
{
    int ret, comp = len;

    while (comp)
    {
      nbrecv_restart:
	ret = recv(s, buf, comp, DEFAULT_MSG_FLAGS);
	if (!ret)	/* socket closed */
	{
	    errno = EPIPE;
	    return (-1);
	}
	if (ret == -1 && errno == EWOULDBLOCK)
	{
	    return (len - comp);	/* return amount completed */
	}
	if (ret == -1 && errno == EINTR)
	{
	    goto nbrecv_restart;
	}
	else if (ret == -1)
	{
	    return (-1);
	}
	comp -= ret;
	buf += ret;
    }
    return (len - comp);
}

/* BMI_sockio_nbpeek()
 *
 * performs a nonblocking check to see if the amount of data requested
 * is actually available in a socket.  Does not actually read the data
 * out.
 *
 * returns number of bytes available on succes, -1 on failure.
 */
int BMI_sockio_nbpeek(int s, void* buf, int len)
{
    int ret, comp = len;

    while (comp)
    {
      nbpeek_restart:
	ret = recv(s, buf, comp, (MSG_PEEK|DEFAULT_MSG_FLAGS));
	if (!ret)	/* socket closed */
	{
	    errno = EPIPE;
	    return (-1);
	}
	if (ret == -1 && errno == EWOULDBLOCK)
	{
	    return (len - comp);	/* return amount completed */
	}
	if (ret == -1 && errno == EINTR)
	{
	    goto nbpeek_restart;
	}
	else if (ret == -1)
	{
	    return (-1);
	}
	comp -= ret;
    }
    return (len - comp);
}


/* blocking send */
int BMI_sockio_bsend(int s,
	  void *buf,
	  int len)
{
    int oldfl, ret, comp = len;
    int olderrno;
    oldfl = fcntl(s, F_GETFL, 0);
    if (oldfl & O_NONBLOCK)
	fcntl(s, F_SETFL, oldfl & (~O_NONBLOCK));

    while (comp)
    {
      bsend_restart:
	if ((ret = send(s, (char *) buf, comp, DEFAULT_MSG_FLAGS)) < 0)
	{
	    if (errno == EINTR)
		goto bsend_restart;
	    olderrno = errno;
	    fcntl(s, F_SETFL, oldfl | O_NONBLOCK);
	    errno = olderrno;
	    return (-1);
	}
	comp -= ret;
	buf += ret;
    }
    fcntl(s, F_SETFL, oldfl | O_NONBLOCK);
    return (len - comp);
}


/* nonblocking send */
/* should always return 0 when nothing gets done! */
int BMI_sockio_nbsend(int s,
	   void *buf,
	   int len)
{
    int ret, comp = len;

    while (comp)
    {
      nbsend_restart:
	ret = send(s, (char *) buf, comp, DEFAULT_MSG_FLAGS);
	if (ret == 0 || (ret == -1 && errno == EWOULDBLOCK))
	    return (len - comp);	/* return amount completed */
	if (ret == -1 && errno == EINTR)
	{
	    goto nbsend_restart;
	}
	else if (ret == -1)
	    return (-1);
	comp -= ret;
	buf += ret;
    }
    return (len - comp);
}

/* nonblocking vector send */
int BMI_sockio_nbvector(int s,
	    struct iovec* vector,
	    int count, 
	    int recv_flag)
{
    int ret;

    /* NOTE: this function is different from the others that will
     * keep making the I/O system call until EWOULDBLOCK is encountered; we 
     * give up after one call
     */

    /* loop over if interrupted */
    do
    {
	if(recv_flag)
	{
	    ret = readv(s, vector, count);
	}
	else
	{
	    ret = writev(s, vector, count);
	}
    }while(ret == -1 && errno == EINTR);

    /* return zero if can't do any work at all */
    if(ret == -1 && errno == EWOULDBLOCK)
	return(0);

    /* if data transferred or an error */
    return(ret);
}

#ifdef __USE_SENDFILE__
/* NBSENDFILE() - nonblocking (on the socket) send from file
 *
 * Here we are going to take advantage of the sendfile() call provided
 * in the linux 2.2 kernel to send from an open file directly (ie. w/out
 * explicitly reading into user space memory or memory mapping).
 *
 * We are going to set the non-block flag on the socket, but leave the
 * file as is.
 *
 * Boy, that type on the offset for sockfile() sure is lame, isn't it?
 * That's going to cause us some headaches when we want to do 64-bit
 * I/O...
 *
 * Returns -1 on error, amount of data written to socket on success.
 */
int BMI_sockio_nbsendfile(int s,
	       int f,
	       int off,
	       int len)
{
    int ret, comp = len, myoff;

    while (comp)
    {
      nbsendfile_restart:
	myoff = off;
	ret = sendfile(s, f, &myoff, comp);
	if (ret == 0 || (ret == -1 && errno == EWOULDBLOCK))
	    return (len - comp);	/* return amount completed */
	if (ret == -1 && errno == EINTR)
	{
	    goto nbsendfile_restart;
	}
	else if (ret == -1)
	    return (-1);
	comp -= ret;
	off += ret;
    }
    return (len - comp);
}
#endif

/* routines to get and set socket options */
int BMI_sockio_get_sockopt(int s,
		int optname)
{
    int val, len = sizeof(val);
    if (getsockopt(s, SOL_SOCKET, optname, &val, &len) == -1)
	return (-1);
    else
	return (val);
}

int BMI_sockio_set_tcpopt(int s,
	       int optname,
	       int val)
{
    if (setsockopt(s, IPPROTO_TCP, optname, &val, sizeof(val)) == -1)
	return (-1);
    else
	return (val);
}

int BMI_sockio_set_sockopt(int s,
		int optname,
		int val)
{
    if (setsockopt(s, SOL_SOCKET, optname, &val, sizeof(val)) == -1)
	return (-1);
    else
	return (val);
}

/*
 * Local variables:
 *  c-indent-level: 4
 *  c-basic-offset: 4
 * End:
 *
 * vim: ts=8 sts=4 sw=4 expandtab
 */
