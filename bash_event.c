/* bash_event.c -- termx 源码级 shell 事件通道。 */

/* Copyright (C) 2025 Free Software Foundation, Inc.

   This file is part of GNU Bash, the Bourne Again SHell.

   Bash is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   Bash is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with Bash.  If not, see <http://www.gnu.org/licenses/>.
*/

/* termx-bash 阶段 1：把 prompt / command 生命周期事件直接写宿主注入的
   Unix domain socket（TERMX_EVENT_ENDPOINT / TERMX_EVENT_TOKEN），不写
   stdout/OSC，不依赖 rcfile，也不覆盖用户的 DEBUG / PROMPT_COMMAND。

   线上协议与 crates/termx-shell-events 一致：连接后先发一行
   {"v":1,"token":"..."}，之后每行一个 JSON 事件（A/B/C/D/X）。

   通道失败、断开或缓冲写满时永久降级为 no-op：shell 不阻塞，也不伪造
   transcript 或退出码。token 只留在进程内存中，读入后立即从环境中清除。 */

#if !defined (_GNU_SOURCE)
#  define _GNU_SOURCE 1
#endif

#include "config.h"

#if defined (HAVE_UNISTD_H)
#  include <unistd.h>
#endif

#include "bashansi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "shell.h"
#include "bash_event.h"

#if defined (__linux__)

#include <fcntl.h>
#include <time.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>

/* 与服务端 crates/termx-shell-events 的 MAX_FRAME_BYTES 保持一致；
   超限帧会被对端断开，因此本地必须先截断或降级。 */
#define BE_MAX_FRAME		4096
/* 单条 C 命令文本的初始预算；放不下时按 1/2 递减，最后省略该字段。 */
#define BE_COMMAND_LIMIT	3500
/* EAGAIN 时的待发缓冲上限；超过即降级，绝不阻塞 shell。 */
#define BE_MAX_PENDING		16384
/* 非阻塞 connect 的等待上限。 */
#define BE_CONNECT_TIMEOUT_MS	200
#define BE_ID_SIZE		33

#define BE_ENDPOINT_ENV		"TERMX_EVENT_ENDPOINT"
#define BE_TOKEN_ENV		"TERMX_EVENT_TOKEN"
#define BE_ENDPOINT_PREFIX	"unix:"

/* 事件帧构造缓冲；overflow 表示放不进一帧。 */
struct be_buf
{
  char data[BE_MAX_FRAME + 1];
  size_t len;
  int overflow;
};

/* 0 = 未初始化, 1 = 可用, -1 = 永久降级。 */
static int be_state = 0;
static int be_fd = -1;
static pid_t be_pid;
static unsigned long be_rng;
static unsigned long be_id_counter;
static char be_active_id[BE_ID_SIZE];
static char be_pending[BE_MAX_PENDING];
static size_t be_pending_len;

static void be_init (void);
static void be_disable (void);
static void be_write (const char *, size_t);
static void be_emit (const struct be_buf *);
static void be_emit_command_start (const char *, const char *, long long);
static void be_emit_command_end (const char *, int, long long);

/* 追加一段原始字节；放不下时只置 overflow。 */
static void
be_add (struct be_buf *buffer, const char *text, size_t len)
{
  if (buffer->overflow)
    return;
  if (buffer->len + len > BE_MAX_FRAME)
    {
      buffer->overflow = 1;
      return;
    }
  memcpy (buffer->data + buffer->len, text, len);
  buffer->len += len;
}

static void
be_adds (struct be_buf *buffer, const char *text)
{
  be_add (buffer, text, strlen (text));
}

/* 追加十进制整数。 */
static void
be_add_long (struct be_buf *buffer, long long value)
{
  char digits[32];

  snprintf (digits, sizeof digits, "%lld", value);
  be_adds (buffer, digits);
}

/* 按 JSON 字符串规则转义 text 的前 limit 个字节（调用方保证 UTF-8 边界）。 */
static void
be_add_escaped (struct be_buf *buffer, const char *text, size_t limit)
{
  static const char hex[] = "0123456789abcdef";
  size_t index;
  unsigned char c;

  for (index = 0; index < limit && text[index] && !buffer->overflow; index++)
    {
      c = (unsigned char) text[index];
      switch (c)
	{
	case '"':
	  be_add (buffer, "\\\"", 2);
	  break;
	case '\\':
	  be_add (buffer, "\\\\", 2);
	  break;
	case '\n':
	  be_add (buffer, "\\n", 2);
	  break;
	case '\r':
	  be_add (buffer, "\\r", 2);
	  break;
	case '\t':
	  be_add (buffer, "\\t", 2);
	  break;
	default:
	  if (c < 0x20)
	    {
	      char escape[6];

	      escape[0] = '\\';
	      escape[1] = 'u';
	      escape[2] = '0';
	      escape[3] = '0';
	      escape[4] = hex[(c >> 4) & 0xf];
	      escape[5] = hex[c & 0xf];
	      be_add (buffer, escape, sizeof escape);
	    }
	  else
	    be_add (buffer, text + index, 1);
	  break;
	}
    }
}

/* 返回不超过 limit 且不切断 UTF-8 多字节字符的前缀长度。 */
static size_t
be_utf8_prefix (const char *text, size_t limit)
{
  size_t len;

  len = strlen (text);
  if (len > limit)
    len = limit;
  while (len > 0 && ((unsigned char) text[len] & 0xc0) == 0x80)
    len--;
  return len;
}

/* 当前 UTC 毫秒时间戳。 */
static long long
be_now_ms (void)
{
  struct timespec ts;

  if (clock_gettime (CLOCK_REALTIME, &ts) == 0)
    return (long long) ts.tv_sec * 1000LL + (long long) (ts.tv_nsec / 1000000L);
  return (long long) time ((time_t *) 0) * 1000LL;
}

/* xorshift64：只用于生成进程内唯一的 command-id，不用于安全用途。 */
static unsigned long
be_next_random (void)
{
  unsigned long x;

  x = be_rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  be_rng = x;
  return x;
}

static void
be_seed (void)
{
  struct timespec ts;
  unsigned long seed;

  seed = (unsigned long) getpid () * 2654435761UL;
  if (clock_gettime (CLOCK_REALTIME, &ts) == 0)
    seed ^= (unsigned long) ts.tv_nsec + ((unsigned long) ts.tv_sec << 20);
  if (seed == 0)
    seed = 0x9e3779b9UL;
  be_rng = seed;
}

/* 32 位十六进制 command-id（与 pwsh 的 Guid "N" 长度一致）。 */
static void
be_new_id (char *out)
{
  struct timespec ts;
  unsigned long stamp;

  stamp = 0;
  if (clock_gettime (CLOCK_REALTIME, &ts) == 0)
    stamp = (unsigned long) ts.tv_nsec;
  snprintf (out, BE_ID_SIZE, "%08lx%08lx%08lx%08lx",
	    (unsigned long) getpid () & 0xffffffffUL,
	    ++be_id_counter & 0xffffffffUL,
	    stamp & 0xffffffffUL,
	    be_next_random () & 0xffffffffUL);
}

static ssize_t
be_send_bytes (const char *data, size_t len)
{
#if defined (MSG_NOSIGNAL)
  return send (be_fd, data, len, MSG_NOSIGNAL);
#else
  return send (be_fd, data, len, 0);
#endif
}

/* 永久降级：关闭 fd，之后所有事件都是 no-op。 */
static void
be_disable (void)
{
  if (be_fd >= 0)
    close (be_fd);
  be_fd = -1;
  be_state = -1;
  be_pending_len = 0;
  be_active_id[0] = '\0';
}

/* 尽力把待发缓冲写空；EAGAIN 保留，其他错误降级。 */
static void
be_flush_pending (void)
{
  size_t offset;

  offset = 0;
  while (offset < be_pending_len)
    {
      ssize_t written;

      written = be_send_bytes (be_pending + offset, be_pending_len - offset);
      if (written > 0)
	{
	  offset += (size_t) written;
	  continue;
	}
      if (written < 0 && errno == EINTR)
	continue;
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	break;
      be_disable ();
      return;
    }
  if (offset)
    {
      memmove (be_pending, be_pending + offset, be_pending_len - offset);
      be_pending_len -= offset;
    }
}

/* 非阻塞写：EAGAIN 时追加到有界待发缓冲，绝不等待。 */
static void
be_write (const char *data, size_t len)
{
  size_t offset;

  if (be_state != 1 || be_fd < 0)
    return;

  if (be_pending_len)
    {
      be_flush_pending ();
      if (be_state != 1)
	return;
    }

  offset = 0;
  while (offset < len)
    {
      ssize_t written;

      written = be_send_bytes (data + offset, len - offset);
      if (written > 0)
	{
	  offset += (size_t) written;
	  continue;
	}
      if (written < 0 && errno == EINTR)
	continue;
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
	{
	  size_t rest;

	  rest = len - offset;
	  if (be_pending_len + rest > sizeof be_pending)
	    {
	      be_disable ();
	      return;
	    }
	  memcpy (be_pending + be_pending_len, data + offset, rest);
	  be_pending_len += rest;
	  return;
	}
      be_disable ();
      return;
    }
}

/* 写一整帧（含换行）。 */
static void
be_emit (const struct be_buf *buffer)
{
  if (be_state != 1 || buffer->overflow)
    return;
  be_write (buffer->data, buffer->len);
  be_write ("\n", 1);
}

/* 非阻塞连接 UDS；任何失败返回 -1，由调用方降级。 */
static int
be_connect (const char *path)
{
  struct sockaddr_un address;
  struct timeval timeout;
  fd_set writable;
  socklen_t length;
  int flags, fd, error, ready;

  if (path == 0 || *path == '\0' || strlen (path) >= sizeof address.sun_path)
    return -1;

  memset (&address, 0, sizeof address);
  address.sun_family = AF_UNIX;
  strcpy (address.sun_path, path);

  fd = socket (AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  flags = fcntl (fd, F_GETFL, 0);
  if (flags < 0 || fcntl (fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
      close (fd);
      return -1;
    }
  fcntl (fd, F_SETFD, FD_CLOEXEC);

  if (connect (fd, (struct sockaddr *) &address, sizeof address) < 0)
    {
      if (errno != EINPROGRESS)
	{
	  close (fd);
	  return -1;
	}

      FD_ZERO (&writable);
      FD_SET (fd, &writable);
      timeout.tv_sec = 0;
      timeout.tv_usec = BE_CONNECT_TIMEOUT_MS * 1000;
      ready = select (fd + 1, (fd_set *) 0, &writable, (fd_set *) 0, &timeout);
      if (ready <= 0)
	{
	  close (fd);
	  return -1;
	}

      length = sizeof error;
      error = 0;
      if (getsockopt (fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0 || error)
	{
	  close (fd);
	  return -1;
	}
    }

  return fd;
}

/* 握手帧：{"v":1,"token":"..."}。 */
static void
be_send_hello (const char *token)
{
  struct be_buf buffer;

  buffer.len = 0;
  buffer.overflow = 0;
  be_adds (&buffer, "{\"v\":1,\"token\":\"");
  be_add_escaped (&buffer, token, strlen (token));
  be_adds (&buffer, "\"}");
  be_emit (&buffer);
}

/* 仅在交互式主 shell 中初始化；读取环境后立即清除 token。 */
static void
be_init (void)
{
  const char *endpoint, *token, *path;
  int fd;

  if (be_state != 0)
    return;

  be_pid = getpid ();

  endpoint = getenv (BE_ENDPOINT_ENV);
  token = getenv (BE_TOKEN_ENV);

  if (interactive == 0 || subshell_environment)
    return;
  if (endpoint == 0 || token == 0 || *token == '\0')
    return;
  if (strncmp (endpoint, BE_ENDPOINT_PREFIX, sizeof (BE_ENDPOINT_PREFIX) - 1) != 0)
    return;

  path = endpoint + sizeof (BE_ENDPOINT_PREFIX) - 1;
  fd = be_connect (path);
  if (fd < 0)
    return;

  be_fd = fd;
  be_state = 1;
  be_seed ();
  be_send_hello (token);

#if defined (HAVE_UNSETENV)
  /* token/path 已不再使用，子 shell 不应继承事件凭据。 */
  unsetenv (BE_ENDPOINT_ENV);
  unsetenv (BE_TOKEN_ENV);
#endif
}

/* 是否可发送事件；子 shell（fork）不继承通道语义。 */
static int
be_ready (void)
{
  if (be_state == 0)
    be_init ();
  return (be_state == 1 && be_pid == getpid ());
}

/* C：命令开始，带命令文本与开始时间；放不下时截断文本。 */
static void
be_emit_command_start (const char *id, const char *command, long long started_at)
{
  struct be_buf buffer;
  size_t limit;

  limit = BE_COMMAND_LIMIT;
  for (;;)
    {
      buffer.len = 0;
      buffer.overflow = 0;
      be_adds (&buffer, "{\"v\":1,\"event\":\"C\",\"id\":\"");
      be_adds (&buffer, id);
      be_adds (&buffer, "\"");
      if (command && *command && limit > 0)
	{
	  be_adds (&buffer, ",\"command\":\"");
	  be_add_escaped (&buffer, command, be_utf8_prefix (command, limit));
	  be_adds (&buffer, "\"");
	}
      be_adds (&buffer, ",\"started_at_ms\":");
      be_add_long (&buffer, started_at);
      be_adds (&buffer, "}");
      if (buffer.overflow == 0)
	break;
      if (limit == 0)
	return;			/* 连基础帧都放不下：丢弃该事件 */
      limit /= 2;
    }
  be_emit (&buffer);
}

/* D：命令结束，带退出码与结束时间。 */
static void
be_emit_command_end (const char *id, int code, long long ended_at)
{
  struct be_buf buffer;

  buffer.len = 0;
  buffer.overflow = 0;
  be_adds (&buffer, "{\"v\":1,\"event\":\"D\",\"code\":");
  be_add_long (&buffer, (long long) code);
  be_adds (&buffer, ",\"id\":\"");
  be_adds (&buffer, id);
  be_adds (&buffer, "\",\"ended_at_ms\":");
  be_add_long (&buffer, ended_at);
  be_adds (&buffer, "}");
  be_emit (&buffer);
}

/* A/B/X 都是无附加字段的单行帧。 */
static void
be_emit_simple (const char *kind)
{
  struct be_buf buffer;

  buffer.len = 0;
  buffer.overflow = 0;
  be_adds (&buffer, "{\"v\":1,\"event\":\"");
  be_adds (&buffer, kind);
  be_adds (&buffer, "\"}");
  be_emit (&buffer);
}

void
bash_event_prompt_start (void)
{
  if (be_ready ())
    be_emit_simple ("A");
}

void
bash_event_prompt_end (void)
{
  if (be_ready ())
    be_emit_simple ("B");
}

void
bash_event_command_start (char *text)
{
  char id[BE_ID_SIZE];

  if (text == 0 || *text == '\0')
    return;
  if (be_ready () == 0)
    return;

  be_new_id (id);
  strcpy (be_active_id, id);
  be_emit_command_start (id, text, be_now_ms ());
}

void
bash_event_command_end (int code)
{
  if (be_ready () == 0)
    return;
  if (be_active_id[0] == '\0')
    return;

  be_emit_command_end (be_active_id, code, be_now_ms ());
  be_active_id[0] = '\0';
}

/* X 只撤销尚未开始的命令；正在执行的命令由 D 收尾。 */
void
bash_event_command_abort (void)
{
  if (be_ready () == 0)
    return;
  if (be_active_id[0] != '\0')
    return;

  be_emit_simple ("X");
}

/* 顶层跳转：有活动命令就发 D，否则 DISCARD 发 X。 */
void
bash_event_reader_jump (int code, int exit_value)
{
  if (be_ready () == 0)
    return;

  switch (code)
    {
    case DISCARD:
      if (be_active_id[0] != '\0')
	bash_event_command_end (exit_value);
      else
	bash_event_command_abort ();
      break;
    case EXITBLTIN:
    case EXITPROG:
    case FORCE_EOF:
    case ERREXIT:
      bash_event_command_end (exit_value);
      break;
    default:
      break;
    }
}

void
bash_event_shutdown (void)
{
  if (be_state == 1)
    be_disable ();
}

#else /* !__linux__ */

void
bash_event_prompt_start (void)
{
}

void
bash_event_prompt_end (void)
{
}

void
bash_event_command_start (char *text)
{
  (void) text;
}

void
bash_event_command_end (int code)
{
  (void) code;
}

void
bash_event_command_abort (void)
{
}

void
bash_event_reader_jump (int code, int exit_value)
{
  (void) code;
  (void) exit_value;
}

void
bash_event_shutdown (void)
{
}

#endif /* __linux__ */
