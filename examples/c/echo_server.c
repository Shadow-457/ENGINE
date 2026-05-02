/* ================================================================
 *  Systrix OS — user/echo_server.c
 *  Minimal demo server: registers as "echo", receives messages,
 *  sends them straight back.  Proves IPC round-trip works.
 *
 *  Build:  add to Makefile like other user programs
 *  Run:    elf ECHO_SRV   (in Systrix shell, starts background server)
 *          elf ECHO_CLI   (sends a test message, prints reply)
 * ================================================================ */

#include "libc.h"
#include "ipc.h"

void _start(void) {
    write(1, "echo-server: starting\n", 22);

    if (ipc_register("echo") != 0) {
        write(1, "echo-server: register failed\n", 29);
        exit(1);
    }

    write(1, "echo-server: registered as 'echo'\n", 34);

    IpcMsg msg;
    while (1) {
        long from = ipc_recv(&msg);
        if (from < 0) continue;

        /* just send it straight back */
        msg.type = MSG_OK;
        ipc_send(from, &msg);
    }
}
