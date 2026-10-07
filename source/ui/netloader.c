#include "netloader.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>

#include <zlib.h>
#include <dirent.h>
#include <strings.h>

#define ZLIB_CHUNK (16 * 1024)

static int listenfd = -1;
static int datafd   = -1;
static int udpfd    = -1;
/* Background mode only: commands over HTTP on NETWORK_PORT+1 --
 *   GET /ping, GET /list (the .3dsx in sdmc:/3ds), GET /launch?path=/3ds/x.3dsx */
static int cmdfd    = -1;
#define COMMAND_PORT (NETWORK_PORT + 1)
static volatile size_t filelen, filetotal;
static volatile bool wantExit = false;

/* Background mode: a thread listens for 3dslink from start-up on, so a build
 * can be sent and started without anyone pressing Y. Y only shows the screen.
 * Before the console sleeps the network is let go (sockets open across sleep
 * can keep it from waking) and taken again after. */
static Thread s_bgThread;
static volatile bool s_background, s_bgStop, s_sleeping, s_bgIdle = true, s_inBackground, s_screenShown;
static aptHookCookie s_aptHook;

static bool shouldStop(void)
{
	return wantExit || (s_inBackground && (s_bgStop || s_sleeping));
}

static void netloaderError(const char* func, int err);

static bool set_socket_nonblocking(int sock)
{
	int flags = fcntl(sock, F_GETFL);
	if (flags == -1) return false;
	return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
}

static int recvall(int sock, void* buffer, int size, int flags)
{
	int len, sizeleft = size;

	while (sizeleft)
	{
		len = recv(sock, buffer, sizeleft, flags);
		if (!len)
		{
			size = 0;
			break;
		} else if (len < 0)
		{
			if (shouldStop())
			{
				netloaderError("stopped", 0);
				size = 0;
				break;
			}
			if (errno != EAGAIN && errno != EWOULDBLOCK)
			{
				netloaderError("recv", errno);
				break;
			}
		} else
		{
			sizeleft -= len;
			buffer += len;
		}
	}
	return size;
}

static bool netloaderInit(void)
{
	return networkInit();
}

static bool netloaderActivate(void)
{
	struct sockaddr_in serv_addr;
	// create udp socket for broadcast ping
	for (;;)
	{
		udpfd = socket(AF_INET, SOCK_DGRAM, 0);
		if (!(udpfd < 0 && errno == -ENETDOWN))
			break;
		svcSleepThread(16666666ULL);
		if (shouldStop())
			return false;
	}
	if (udpfd < 0)
	{
		netloaderError("udp socket", errno);
		return false;
	}

	memset(&serv_addr, '0', sizeof(serv_addr));
	serv_addr.sin_family = AF_INET;
	serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	serv_addr.sin_port = htons(NETWORK_PORT);

	if (bind(udpfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0)
	{
		netloaderError("bind udp socket", errno);
		return false;
	}

	if (!set_socket_nonblocking(udpfd))
	{
		netloaderError("listen fcntl", errno);
		return false;
	}

	// create listening socket on all addresses on NETWORK_PORT

	listenfd = socket(AF_INET, SOCK_STREAM, 0);
	if (listenfd < 0)
	{
		netloaderError("socket", errno);
		return false;
	}

	int rc = bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
	if (rc != 0)
	{
		netloaderError("bind", errno);
		return false;
	}

	if (!set_socket_nonblocking(listenfd))
	{
		netloaderError("listen fcntl", errno);
		return false;
	}

	rc = listen(listenfd, 10);
	if(rc != 0)
	{
		netloaderError("listen", errno);
		return false;
	}

	if (s_inBackground)
	{
		cmdfd = socket(AF_INET, SOCK_STREAM, 0);
		serv_addr.sin_port = htons(COMMAND_PORT);
		if (cmdfd < 0 || bind(cmdfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) != 0 ||
			!set_socket_nonblocking(cmdfd) || listen(cmdfd, 4) != 0)
		{
			/* Without commands the netloader still works. */
			if (cmdfd >= 0) close(cmdfd);
			cmdfd = -1;
		}
	}

	return true;
}

static void netloaderDeactivate(void)
{
	if (cmdfd >= 0)
	{
		close(cmdfd);
		cmdfd = -1;
	}

	if (listenfd >= 0)
	{
		close(listenfd);
		listenfd = -1;
	}

	if (datafd >= 0)
	{
		close(datafd);
		datafd = -1;
	}

	if (udpfd >= 0)
	{
		close(udpfd);
		udpfd = -1;
	}

	networkDeactivate();
}

void netloaderError(const char* func, int err)
{
	netloaderDeactivate();
	if (s_inBackground)
	{
		/* No error screen: the listener just starts over. */
		if (s_screenShown && uiGetStateInfo()->update == netloaderUpdate)
			uiExitState();
		s_screenShown = false;
		return;
	}
	networkError(netloaderUpdate, StrId_NetLoader, func, err);
}

static int receiveAndDecompress(int sock, FILE* fh, size_t filesize)
{
	static unsigned char in[ZLIB_CHUNK];
	static unsigned char out[ZLIB_CHUNK];

	int ret;
	unsigned have;
	z_stream strm;
	size_t chunksize;

	/* allocate inflate state */
	strm.zalloc = Z_NULL;
	strm.zfree = Z_NULL;
	strm.opaque = Z_NULL;
	strm.avail_in = 0;
	strm.next_in = Z_NULL;
	ret = inflateInit(&strm);
	if (ret != Z_OK)
	{
		netloaderError("inflateInit", ret);
		return ret;
	}

	size_t total = 0;
	// decompress until deflate stream ends or end of file
	do
	{
		int len = recvall(sock, &chunksize, 4, 0);

		if (len != 4)
		{
			inflateEnd(&strm);
			netloaderError("chunksize", len);
			return Z_DATA_ERROR;
		}

		strm.avail_in = recvall(sock,in,chunksize,0);

		if (strm.avail_in == 0)
		{
			inflateEnd(&strm);
			netloaderError("closed", 0);
			return Z_DATA_ERROR;
		}

		strm.next_in = in;

		// run inflate() on input until output buffer not full
		do
		{
			strm.avail_out = ZLIB_CHUNK;
			strm.next_out = out;
			ret = inflate(&strm, Z_NO_FLUSH);

			switch (ret)
			{
				case Z_NEED_DICT:
					ret = Z_DATA_ERROR; // and fall through
				case Z_DATA_ERROR:
				case Z_MEM_ERROR:
				case Z_STREAM_ERROR:
					inflateEnd(&strm);
					netloaderError("inflate", ret);
					return ret;
			}

			have = ZLIB_CHUNK - strm.avail_out;

			if (fwrite(out, 1, have, fh) != have || ferror(fh))
			{
				inflateEnd(&strm);
				netloaderError("fwrite",0);
				return Z_ERRNO;
			}

			total += have;
			filetotal = total;
			//sprintf(progress,"%zu (%d%%)",total, (100 * total) / filesize);
			//netloader_draw_progress();
		} while (strm.avail_out == 0);

		// done when inflate() says it's done
	} while (ret != Z_STREAM_END);

	// clean up and return
	inflateEnd(&strm);
	return ret == Z_STREAM_END ? Z_OK : Z_DATA_ERROR;
}

static void sendText(int fd, int status, const char* text)
{
	char head[128];
	int n = snprintf(head, sizeof(head), "HTTP/1.0 %d %s\r\nContent-Type: text/plain\r\nAccess-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
		status, status == 200 ? "OK" : status == 404 ? "Not Found" : "Bad Request");
	send(fd, head, n, 0);
	send(fd, text, strlen(text), 0);
}

static int fromHex(char c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* One command connection; true (and `me` filled) when it asks to launch. */
static bool handleCommand(int fd, menuEntry_s* me)
{
	char req[512];
	int got = 0;
	u64 start = osGetTime();
	while (got < (int)sizeof(req) - 1 && osGetTime() - start < 2000)
	{
		int n = recv(fd, req + got, sizeof(req) - 1 - got, 0);
		if (n > 0) { got += n; req[got] = 0; if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) break; }
		else if (n == 0) break;
		else svcSleepThread(5000000ULL);
	}
	req[got] = 0;
	char target[400] = { 0 };
	if (sscanf(req, "%*7s %399s", target) != 1)
	{
		sendText(fd, 400, "bad request\n");
		return false;
	}
	if (strcmp(target, "/ping") == 0)
	{
		sendText(fd, 200, "hbmenu " VERSION "\n");
		return false;
	}
	if (strcmp(target, "/list") == 0)
	{
		static char list[4096];
		int n = 0;
		list[0] = 0;
		DIR* dir = opendir("sdmc:/3ds");
		if (dir)
		{
			struct dirent* de;
			while ((de = readdir(dir)) && n < (int)sizeof(list) - 300)
			{
				size_t len = strlen(de->d_name);
				if (len > 5 && strcasecmp(de->d_name + len - 5, ".3dsx") == 0)
					n += snprintf(list + n, sizeof(list) - n, "/3ds/%s\n", de->d_name);
			}
			closedir(dir);
		}
		sendText(fd, 200, list);
		return false;
	}
	const char* q = strstr(target, "/launch?path=");
	if (!q)
	{
		sendText(fd, 404, "try /ping, /list, /launch?path=/3ds/x.3dsx\n");
		return false;
	}
	/* The path, %-decoded; "/3ds/x.3dsx" or "sdmc:/3ds/x.3dsx". */
	char path[300];
	int o = 0;
	for (const char* c = q + 13; *c && *c != '&' && o < (int)sizeof(path) - 1; c ++)
	{
		if (*c == '%' && fromHex(c[1]) >= 0 && fromHex(c[2]) >= 0) { path[o++] = (char)(fromHex(c[1]) * 16 + fromHex(c[2])); c += 2; }
		else if (*c == '+') path[o++] = ' ';
		else path[o++] = *c;
	}
	path[o] = 0;
	menuEntryInit(me, ENTRY_TYPE_FILE);
	snprintf(me->path, sizeof(me->path), "%s%s", strncmp(path, "sdmc:", 5) == 0 ? "" : "sdmc:", path);
	FILE* f = fopen(me->path, "rb");
	if (!f)
	{
		sendText(fd, 404, "no such file\n");
		return false;
	}
	fclose(f);
	/* argv[0] is the file, as when it is picked in the menu: romfs needs it. */
	argData_s* ad = &me->args;
	ad->buf[0] = 0;
	ad->dst = (char*)&ad->buf[1];
	launchAddArg(ad, me->path);
	sendText(fd, 200, "launching\n");
	return true;
}

/* One listen-receive-launch round; true when a file was launched. */
static bool netloaderRun(bool background)
{
	struct sockaddr_in sa_udp_remote;
	char recvbuf[256];
	if (!background)
		wantExit = false;
	filelen = 0;
	filetotal = 0;
	s_inBackground = background;

	if (!netloaderInit())
	{
		if (!background)
			errorScreen(textGetString(StrId_NetLoader), textGetString(StrId_NetLoaderUnavailable));
		return false;
	}

	if (!background)
		uiEnterState(UI_STATE_NETLOADER);

	if (!netloaderActivate())
	{
		netloaderDeactivate();
		if (!background)
			uiExitState();
		return false;
	}

	while (datafd < 0)
	{
		if (shouldStop())
		{
			netloaderDeactivate();
			if (!background)
				uiExitState();
			else if (s_screenShown && uiGetStateInfo()->update == netloaderUpdate)
			{
				uiExitState();
				s_screenShown = false;
			}
			return false;
		}

		socklen_t fromlen = sizeof(sa_udp_remote);

		int len = recvfrom(udpfd, recvbuf, sizeof(recvbuf), 0, (struct sockaddr*) &sa_udp_remote, &fromlen);
		if (len != -1 && strncmp(recvbuf, "3dsboot", 7) == 0)
		{
			sa_udp_remote.sin_family = AF_INET;
			sa_udp_remote.sin_port = htons(NETWORK_PORT);
			sendto(udpfd, "boot3ds", 7, 0, (struct sockaddr*) &sa_udp_remote,sizeof(sa_udp_remote));
		}

		if (cmdfd >= 0)
		{
			struct sockaddr_in from;
			socklen_t fromlen2 = sizeof(from);
			int fd = accept(cmdfd, (struct sockaddr*)&from, &fromlen2);
			if (fd >= 0)
			{
				static menuEntry_s cme;
				const bool launch = handleCommand(fd, &cme);
				svcSleepThread(50000000ULL); /* let the answer leave */
				close(fd);
				if (launch)
				{
					netloaderDeactivate();
					if (s_screenShown && uiGetStateInfo()->update == netloaderUpdate)
						uiExitState();
					s_screenShown = false;
					launchMenuEntry(&cme);
					return true;
				}
			}
		}

		socklen_t addrlen = sizeof(struct sockaddr_in);
		datafd = accept(listenfd, (struct sockaddr*)&sa_udp_remote, &addrlen);
		if (datafd < 0)
		{
			if (errno != -EWOULDBLOCK && errno != EWOULDBLOCK)
			{
				netloaderError("accept", errno);
				return false;
			}
		} else
		{
			close(listenfd);
			listenfd = -1;
			/* Someone is sending: show the progress. */
			if (background && !s_screenShown)
			{
				s_screenShown = true;
				uiEnterState(UI_STATE_NETLOADER);
			}
		}

		svcSleepThread(16666666ULL);
	}

	int namelen;
	int len = recvall(datafd, &namelen, 4, 0);
	if (len != 4 || namelen >= (sizeof(recvbuf)+1))
	{
		netloaderError("namelen", errno);
		return false;
	}

	len = recvall(datafd, recvbuf, namelen, 0);
	if (len != namelen)
	{
		netloaderError("name", errno);
		return false;
	}

	recvbuf[namelen] = 0;
	len = recvall(datafd, (int*)&filelen, 4, 0);
	if (len != 4)
	{
		netloaderError("filelen", errno);
		return false;
	}

	int response = 0;

	static menuEntry_s me;
	menuEntryInit(&me, ENTRY_TYPE_FILE);
	strncpy(me.path, "sdmc:/3ds/", sizeof(me.path)-1);
	strncat(me.path, recvbuf, sizeof(me.path)-1);
	me.path[sizeof(me.path)-1] = 0;

	FILE* outf = fopen(me.path, "wb");
	if (!outf)
		response = -1;
	send(datafd, &response, sizeof(response), 0);

	if (response < 0)
	{
		netloaderError("fopen", errno);
		return false;
	}

	static char fbuf[64*1024];
	setvbuf(outf, fbuf, _IOFBF, sizeof(fbuf));
	len = receiveAndDecompress(datafd, outf, filelen);
	fclose(outf);

	if (len != Z_OK)
		return false;

	send(datafd, &response, sizeof(response), 0);

	int cmdlen;
	len = recvall(datafd, &cmdlen, 4, 0);
	if (len == 4 && cmdlen <= sizeof(fbuf))
	{
		len = recvall(datafd, fbuf, cmdlen, 0);
		if (len == cmdlen)
		{
			argData_s* ad = &me.args;
			ad->buf[0] = 0;
			ad->dst = (char*)&ad->buf[1];

			char* ptr = fbuf;
			char* ptrend = fbuf + cmdlen;
			while (ptr < ptrend)
				ptr += launchAddArg(ad, ptr);
		}
	}

	uint32_t remote = sa_udp_remote.sin_addr.s_addr;
	if (remote) {
		char netlinked[18];
		sprintf(netlinked,"%08" PRIx32 "_3DSLINK_",remote);
		launchAddArg(&me.args, netlinked);
	}

	netloaderDeactivate();
	if (!background || s_screenShown)
		uiExitState();
	s_screenShown = false;
	launchMenuEntry(&me);
	return true;
}

void netloaderTask(void* arg)
{
	if (s_background)
	{
		/* The listener is already up: just show its screen. */
		s_screenShown = true;
		uiEnterState(UI_STATE_NETLOADER);
		return;
	}
	netloaderRun(false);
}

static void netloaderBackgroundProc(void* arg)
{
	while (!s_bgStop)
	{
		if (s_sleeping)
		{
			s_bgIdle = true;
			svcSleepThread(100000000ULL);
			continue;
		}
		s_bgIdle = false;
		const bool launched = netloaderRun(true);
		s_bgIdle = true;
		if (launched)
			break; /* the menu is on its way out */
		if (!s_bgStop && !s_sleeping)
			svcSleepThread(1000000000ULL);
	}
	s_bgIdle = true;
}

static void netloaderAptHook(APT_HookType hook, void* param)
{
	if (hook == APTHOOK_ONSLEEP)
	{
		s_sleeping = true;
		/* Let the listener close its sockets before the console sleeps. */
		for (int i = 0; i < 150 && !s_bgIdle; i ++)
			svcSleepThread(10000000ULL);
	} else if (hook == APTHOOK_ONWAKEUP)
	{
		s_sleeping = false;
	}
}

void netloaderStartBackground(void)
{
	if (s_background) return;
	s_background = true;
	s_bgStop = false;
	aptHook(&s_aptHook, netloaderAptHook, NULL);
	s_bgThread = threadCreate(netloaderBackgroundProc, NULL, 32*1024, 0x30, -2, false);
	if (!s_bgThread)
	{
		s_background = false;
		aptUnhook(&s_aptHook);
	}
}



void netloaderUpdate(void)
{
	if (wantExit || datafd >= 0) return;

	if (hidKeysDown() & KEY_B)
	{
		if (s_background)
		{
			/* Hide the screen; the listener stays. */
			s_screenShown = false;
			uiExitState();
		} else
			wantExit = true;
	}
}

void netloaderExit(void)
{
	wantExit = true;
	if (s_background)
	{
		s_bgStop = true;
		threadJoin(s_bgThread, 3000000000ULL);
		threadFree(s_bgThread);
		aptUnhook(&s_aptHook);
		s_background = false;
	}
}

void netloaderDrawBot(void)
{
	char buf[256];
	const char* text = NULL;
	if (datafd < 0)
	{
		u32 ip = gethostid();
		snprintf(buf, sizeof(buf), textGetString(StrId_NetLoaderActive), ip&0xFF, (ip>>8)&0xFF, (ip>>16)&0xFF, (ip>>24)&0xFF, NETWORK_PORT);
		text = buf;
	}

	networkDrawBot(StrId_NetLoader, text, (datafd >= 0 && filelen), filelen, filetotal);
}
