/* ================================================================
 *  Systrix OS — user/echo_client.c
 *  Sends a test message to the "echo" server, prints the reply.
 *  Run AFTER echo_server is already running.
 * ================================================================ */

#include "libc.h"
#include "ipc.h"

void _start(void) {
    write(1, "echo-client: looking up server\n", 31);

    long srv = ipc_lookup("echo");
    if (srv < 0) {
        write(1, "echo-client: server not found! start ECHO_SRV first\n", 52);
        exit(1);
    }

    write(1, "echo-client: found server, sending message\n", 43);

    IpcMsg req;
    req.type    = 0x1234;
    req.data[0] = 0xDEADBEEF;
    req.data[1] = 42;
    req.data[2] = 0;
    req.data[3] = 0;
    req.data[4] = 0;
    req.data[5] = 0;

    if (ipc_send(srv, &req) != 0) {
        write(1, "echo-client: send failed\n", 25);
        exit(1);
    }

    IpcMsg reply;
    ipc_recv(&reply);

    if (reply.type == MSG_OK && reply.data[0] == 0xDEADBEEF) {
        write(1, "echo-client: IPC round-trip OK! microkernel lives!\n", 51);
    } else {
        write(1, "echo-client: reply mismatch\n", 28);
    }

    exit(0);
}
