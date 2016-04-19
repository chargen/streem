#ifndef _WIN32
# include <sys/fcntl.h>
# include <sys/types.h>
# include <sys/socket.h>
# include <netinet/in.h>
# include <netdb.h>
# include <stdio.h>
# define closesocket(fd) close(fd)
#else
# include <ws2tcpip.h>
# include <mswsock.h>
# include <fcntl.h>
#endif

#include "strm.h"

struct socket_data {
  int sock;
};

static int
accept_cb(strm_stream* task, strm_value data)
{
  struct socket_data *sd = task->data;
  struct sockaddr_in writer_addr;
  socklen_t writer_len;
  int sock;

  memset(&writer_addr, 0, sizeof(writer_addr));
  writer_len = sizeof(writer_addr);
  sock = accept(sd->sock, (struct sockaddr *)&writer_addr, &writer_len);
  if (sock < 0) {
    closesocket(sock);
    strm_raise(task, "socket error: listen");
    return STRM_NG;
  }

  strm_io_emit(task, strm_io_new(sock, STRM_IO_READ|STRM_IO_WRITE|STRM_IO_FLUSH),
               sd->sock, accept_cb);
  return STRM_OK;
}

static int
server_accept(strm_stream* task, strm_value data)
{
  struct socket_data *sd = task->data;

  strm_io_start_read(task, sd->sock, accept_cb);
  return STRM_OK;
}

static int
server_close(strm_stream* task, strm_value d)
{
  struct socket_data *sd = task->data;

  closesocket(sd->sock);
  free(sd);
  return STRM_OK;
}

static int
tcp_server(strm_stream* task, int argc, strm_value* args, strm_value* ret)
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int sock, s;
  const char *service;
  char buf[12];
  struct socket_data *sd;
  strm_stream *t;

#ifdef _WIN32
  int sockopt = SO_SYNCHRONOUS_NONALERT;
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 0), &wsa);
  setsockopt(INVALID_SOCKET, SOL_SOCKET, SO_OPENTYPE, (char *)&sockopt, sizeof(sockopt));
#endif

  if (argc != 1) {
    strm_raise(task, "tcp_server: wrong number of arguments");
    return STRM_NG;
  }
  if (strm_num_p(args[0])) {
    sprintf(buf, "%d", (int)strm_value_int(args[0]));
    service = buf;
  }
  else {
    strm_string str = strm_value_str(args[0]);
    service = strm_str_cstr(str, buf);
  }

  memset(&hints, 0, sizeof(struct addrinfo));
#ifdef _WIN32
  hints.ai_family = AF_INET;      /* TODO: IPv6 doesn't work on windows */
#else
  hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
#endif
  hints.ai_socktype = SOCK_STREAM;/* Datagram socket */
  hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
  hints.ai_protocol = 0;          /* Any protocol */

  for (;;) {
    s = getaddrinfo(NULL, service, &hints, &result);
    if (s != 0) {
      if (s == EAI_AGAIN) continue;
      strm_raise(task, gai_strerror(s));
      return STRM_NG;
    }
    break;
  }

  for (rp = result; rp != NULL; rp = rp->ai_next) {
    sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock == -1) continue;

    if (bind(sock, rp->ai_addr, rp->ai_addrlen) == 0)
      break;                    /* Success */
    closesocket(sock);
  }

  freeaddrinfo(result);
  if (rp == NULL) {
    strm_raise(task, "socket error: bind");
    return STRM_NG;
  }

  if (listen(sock, 5) < 0) {
    closesocket(sock);
    strm_raise(task, "socket error: listen");
    return STRM_NG;
  }

  sd = malloc(sizeof(struct socket_data));
  sd->sock = sock;
  t = strm_stream_new(strm_producer, server_accept, server_close, (void*)sd);
  *ret = strm_stream_value(t);
  return STRM_OK;
}

static int
tcp_socket(strm_stream* task, int argc, strm_value* args, strm_value* ret)
{
  struct addrinfo hints;
  struct addrinfo *result, *rp;
  int sock, s;
  const char *service;
  char sbuf[12], hbuf[7];
  strm_string host;

#ifdef _WIN32
  int sockopt = SO_SYNCHRONOUS_NONALERT;
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 0), &wsa);
  setsockopt(INVALID_SOCKET, SOL_SOCKET, SO_OPENTYPE, (char *)&sockopt, sizeof(sockopt));
#endif

  if (argc != 2) {
    strm_raise(task, "tcp_socket: wrong number of arguments");
    return STRM_NG;
  }
  host = strm_value_str(args[0]);
  if (strm_num_p(args[1])) {
    sprintf(sbuf, "%d", (int)strm_value_int(args[1]));
    service = sbuf;
  }
  else {
    strm_string str = strm_value_str(args[1]);
    service = strm_str_cstr(str, sbuf);
  }

  memset(&hints, 0, sizeof(struct addrinfo));
#ifdef _WIN32
  hints.ai_family = AF_INET;      /* TODO: IPv6 doesn't work on windows */
#else
  hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
#endif
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = 0;          /* Any protocol */
  s = getaddrinfo(strm_str_cstr(host, hbuf), service, &hints, &result);

  if (s != 0) {
    strm_raise(task, gai_strerror(s));
    return STRM_NG;
  }

  for (rp = result; rp != NULL; rp = rp->ai_next) {
    sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
    if (sock == -1) continue;

    if (connect(sock, rp->ai_addr, rp->ai_addrlen) != -1)
      break;                    /* Success */
    closesocket(sock);
  }

  freeaddrinfo(result);
  if (rp == NULL) {
    strm_raise(task, "socket error: connect");
    return STRM_NG;
  }
  *ret = strm_ptr_value((void*)strm_io_new(sock,
		STRM_IO_READ|STRM_IO_WRITE|STRM_IO_FLUSH));
  return STRM_OK;
}

void
strm_socket_init(strm_state* state)
{
  strm_var_def(state, "tcp_server", strm_cfunc_value(tcp_server));
  strm_var_def(state, "tcp_socket", strm_cfunc_value(tcp_socket));
}
