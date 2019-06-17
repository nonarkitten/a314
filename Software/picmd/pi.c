/*
 * Copyright (c) 2018 Niklas Ekström
 */

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <exec/memory.h>

#include <libraries/dos.h>
#include <libraries/dosextens.h>

#include <proto/dos.h>
#include <proto/exec.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "../a314device/a314.h"
#include "../a314device/proto_a314.h"

#define PICMD_SERVICE_NAME "picmd"

#define ID_314_DISK (('3' << 24) | ('1' << 16) | ('4' << 8))

struct MsgPort *sync_mp;
struct MsgPort *async_mp;

struct A314_IORequest *read_ior;
struct A314_IORequest *sync_ior;

struct Library *A314Base;

struct FileHandle *con;

ULONG socket;

UBYTE arbuf[256];

struct StandardPacket sync_sp;
struct StandardPacket wait_sp;

BOOL pending_a314_read = FALSE;
BOOL pending_con_wait = FALSE;
BOOL stream_closed = FALSE;

void put_con_sp(struct MsgPort *reply_port, struct StandardPacket *sp, LONG action, LONG arg1, LONG arg2, LONG arg3)
{
	sp->sp_Msg.mn_Node.ln_Type = NT_MESSAGE;
	sp->sp_Msg.mn_Node.ln_Pri = 0;
	sp->sp_Msg.mn_Node.ln_Name = (char *)&(sp->sp_Pkt);
	sp->sp_Msg.mn_Length = sizeof(struct StandardPacket);
	sp->sp_Msg.mn_ReplyPort = reply_port;
	sp->sp_Pkt.dp_Link = &(sp->sp_Msg);
	sp->sp_Pkt.dp_Port = reply_port;
	sp->sp_Pkt.dp_Type = action;
	sp->sp_Pkt.dp_Arg1 = arg1;
	sp->sp_Pkt.dp_Arg2 = arg2;
	sp->sp_Pkt.dp_Arg3 = arg3;
	PutMsg(con->fh_Type, &(sp->sp_Msg));
}

LONG set_screen_mode(LONG mode)
{
	put_con_sp(sync_mp, &sync_sp, ACTION_SCREEN_MODE, mode, 0, 0);
	Wait(1L << sync_mp->mp_SigBit);
	GetMsg(sync_mp);
	return sync_sp.sp_Pkt.dp_Res1;
}

LONG con_write(char *s, int length)
{
	put_con_sp(sync_mp, &sync_sp, ACTION_WRITE, con->fh_Arg1, (LONG)s, length);
	Wait(1L << sync_mp->mp_SigBit);
	GetMsg(sync_mp);
	return sync_sp.sp_Pkt.dp_Res1;
}

LONG con_read(char *s, int length)
{
	put_con_sp(sync_mp, &sync_sp, ACTION_READ, con->fh_Arg1, (LONG)s, length);
	Wait(1L << sync_mp->mp_SigBit);
	GetMsg(sync_mp);
	return sync_sp.sp_Pkt.dp_Res1;
}

void start_con_wait()
{
	put_con_sp(async_mp, &wait_sp, ACTION_WAIT_CHAR, 100000, 0, 0);
	pending_con_wait = TRUE;
}

void start_a314_cmd(struct MsgPort *reply_port, struct A314_IORequest *ior, UWORD cmd, char *buffer, int length)
{
	ior->a314_Request.io_Message.mn_ReplyPort = reply_port;
	ior->a314_Request.io_Command = cmd;
	ior->a314_Request.io_Error = 0;
	ior->a314_Socket = socket;
	ior->a314_Buffer = buffer;
	ior->a314_Length = length;
	SendIO((struct IORequest *)ior);
}

BYTE a314_connect(char *name)
{
	socket = time(NULL);
	start_a314_cmd(sync_mp, sync_ior, A314_CONNECT, name, strlen(name));
	Wait(1L << sync_mp->mp_SigBit);
	GetMsg(sync_mp);
	return sync_ior->a314_Request.io_Error;
}

BYTE a314_write(char *buffer, int length)
{
	start_a314_cmd(sync_mp, sync_ior, A314_WRITE, buffer, length);
	Wait(1L << sync_mp->mp_SigBit);
	GetMsg(sync_mp);
	return sync_ior->a314_Request.io_Error;
}

BYTE a314_eos()
{
	start_a314_cmd(sync_mp, sync_ior, A314_EOS, NULL, 0);
	Wait(1L << sync_mp->mp_SigBit);
	GetMsg(sync_mp);
	return sync_ior->a314_Request.io_Error;
}

BYTE a314_reset()
{
	start_a314_cmd(sync_mp, sync_ior, A314_RESET, NULL, 0);
	Wait(1L << sync_mp->mp_SigBit);
	GetMsg(sync_mp);
	return sync_ior->a314_Request.io_Error;
}

void start_a314_read()
{
	start_a314_cmd(async_mp, read_ior, A314_READ, arbuf, 255);
	pending_a314_read = TRUE;
}

void handle_con_wait_completed()
{
	pending_con_wait = FALSE;

	if (stream_closed)
		return;

	if (wait_sp.sp_Pkt.dp_Res1 == DOSFALSE)
	{
		start_con_wait();
	}
	else
	{
		unsigned char buf[64];
		int len = con_read(buf, sizeof(buf));

		if (len == 0 || len == -1)
		{
			a314_reset();
			stream_closed = TRUE;
		}
		else
		{
			a314_write(buf, len);
			start_con_wait();
		}
	}
}

void handle_a314_read_completed()
{
	pending_a314_read = FALSE;

	if (stream_closed)
		return;

	int res = read_ior->a314_Request.io_Error;
	if (res == A314_READ_OK)
	{
		UBYTE *p = read_ior->a314_Buffer;
		int len = read_ior->a314_Length;

		con_write(p, len);
		start_a314_read();
	}
	else if (res == A314_READ_EOS)
	{
		a314_eos();
		stream_closed = TRUE;
	}
	else if (res == A314_READ_RESET)
	{
		stream_closed = TRUE;
	}
}

UBYTE *create_and_send_start_msg(int *buffer_len, BPTR current_dir, int argc, char **argv, short rows, short cols)
{
	int buf_len = 6;

	int component_count = 0;
	UBYTE *components[20];

	if (current_dir != 0)
	{
		struct FileLock *fl = (struct FileLock *)BADDR(current_dir);
		struct DeviceList *dl = (struct DeviceList *)BADDR(fl->fl_Volume);

		if (dl->dl_DiskType == ID_314_DISK)
		{
			struct FileInfoBlock *fib = AllocMem(sizeof(struct FileInfoBlock), 0);

			BPTR lock = DupLock(current_dir);

			while (lock != 0)
			{
				if (Examine(lock, fib) == 0)
				{
					UnLock(lock);
					break;
				}

				int n = strlen(fib->fib_FileName);
				UBYTE *p = AllocMem(n + 1, 0);
				p[0] = (UBYTE)n;
				memcpy(&p[1], fib->fib_FileName, n);
				components[component_count++] = p;

				buf_len += n + 1;

				BPTR child = lock;
				lock = ParentDir(child);
				UnLock(child);
			}

			FreeMem(fib, sizeof(struct FileInfoBlock));
		}
	}

	for (int i = 1; i < argc; i++)
		buf_len += strlen(argv[i]) + 1;

	UBYTE *buffer = AllocMem(buf_len, MEMF_A314);

	UBYTE *p = buffer;

	*(short *)p = rows;
	p += 2;
	*(short *)p = cols;
	p += 2;

	*p++ = (UBYTE)component_count;
	for (int i = 0; i < component_count; i++)
	{
		UBYTE *q = components[component_count - 1 - i];
		int n = *q;
		memcpy(p, q, n + 1);
		p += n + 1;
		FreeMem(q, n + 1);
	}

	*p++ = (UBYTE)(argc - 1);
	for (int i = 1; i < argc; i++)
	{
		UBYTE *q = (UBYTE *)argv[i];
		int n = strlen(q);
		*p++ = (UBYTE)n;
		memcpy(p, q, n);
		p += n;
	}

	ULONG buf_desc[2] = {TranslateAddressA314(buffer), buf_len};
	a314_write((char *)buf_desc, sizeof(buf_desc));

	*buffer_len = buf_len;
	return buffer;
}

int main(int argc, char **argv)
{
	sync_mp = CreatePort(NULL, 0);
	if (sync_mp == NULL)
	{
		printf("Unable to create sync reply message port\n");
		return 0;
	}

	async_mp = CreatePort(NULL, 0);
	if (async_mp == NULL)
	{
		printf("Unable to create async reply message port\n");
		DeletePort(sync_mp);
		return 0;
	}

	sync_ior = (struct A314_IORequest *)CreateExtIO(sync_mp, sizeof(struct A314_IORequest));
	if (sync_ior == NULL)
	{
		printf("Unable to create io request for synchronous commands\n");
		DeletePort(async_mp);
		DeletePort(sync_mp);
		return 0;
	}

	read_ior = (struct A314_IORequest *)CreateExtIO(sync_mp, sizeof(struct A314_IORequest));
	if (read_ior == NULL)
	{
		printf("Unable to create io request for reads\n");
		DeleteExtIO((struct IORequest *)sync_ior);
		DeletePort(async_mp);
		DeletePort(sync_mp);
		return 0;
	}

	if (OpenDevice(A314_NAME, 0, (struct IORequest *)sync_ior, 0) != 0)
	{
		printf("Unable to open a314.device\n");
		DeleteExtIO((struct IORequest *)read_ior);
		DeleteExtIO((struct IORequest *)sync_ior);
		DeletePort(async_mp);
		DeletePort(sync_mp);
		return 0;
	}

	memcpy(read_ior, sync_ior, sizeof(struct A314_IORequest));

	A314Base = &(sync_ior->a314_Request.io_Device->dd_Library);

	if (a314_connect(PICMD_SERVICE_NAME) != A314_CONNECT_OK)
	{
		printf("Unable to connect to picmd service\n");
		CloseDevice((struct IORequest *)sync_ior);
		DeleteExtIO((struct IORequest *)read_ior);
		DeleteExtIO((struct IORequest *)sync_ior);
		DeletePort(async_mp);
		DeletePort(sync_mp);
		return 0;
	}

	struct Process *proc = (struct Process *)FindTask(NULL);
	con = (struct FileHandle *)BADDR(proc->pr_CIS);

	set_screen_mode(DOSTRUE);

	con_write("\x9b" "0 q", 4);

	int len = con_read(arbuf, 32);	// "\x9b" "1;1;33;77 r"
	if (len < 10 || arbuf[len - 1] != 'r')
	{
		printf("Failure to receive window bounds report\n");
		set_screen_mode(DOSFALSE);
		a314_reset();
		CloseDevice((struct IORequest *)sync_ior);
		DeleteExtIO((struct IORequest *)read_ior);
		DeleteExtIO((struct IORequest *)sync_ior);
		DeletePort(async_mp);
		DeletePort(sync_mp);
		return 0;
	}

	con_write("\x9b" "12{", 4);

	int start = 5;
	int ind = start;
	while (arbuf[ind] != ';')
		ind++;
	arbuf[ind] = 0;
	int rows = atoi(arbuf + start);
	ind++;
	start = ind;
	while (arbuf[ind] != ' ')
		ind++;
	arbuf[ind] = 0;
	int cols = atoi(arbuf + start);

	int start_msg_len;
	UBYTE *start_msg = create_and_send_start_msg(&start_msg_len, proc->pr_CurrentDir, argc, argv, (short)rows, (short)cols);

	start_con_wait();
	start_a314_read();

	ULONG portsig = 1L << async_mp->mp_SigBit;

	while (TRUE)
	{
		ULONG signal = Wait(portsig | SIGBREAKF_CTRL_C);

		if (signal & portsig)
		{
			struct Message *msg;
			while (msg = GetMsg(async_mp))
			{
				if (msg == (struct Message *)&wait_sp)
					handle_con_wait_completed();
				else if (msg == (struct Message *)read_ior)
					handle_a314_read_completed();
			}
		}

		if (stream_closed && !pending_a314_read && !pending_con_wait)
			break;
	}

	set_screen_mode(DOSFALSE);

	FreeMem(start_msg, start_msg_len);

	CloseDevice((struct IORequest *)sync_ior);
	DeleteExtIO((struct IORequest *)read_ior);
	DeleteExtIO((struct IORequest *)sync_ior);
	DeletePort(async_mp);
	DeletePort(sync_mp);
	return 0;
}
