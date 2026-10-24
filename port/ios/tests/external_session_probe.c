#include "host_external_session.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
    struct host_external_session session={0};
    host_external_session_connect(&session,1);
    assert(session.owner==1 && session.negotiating && session.pending);
    host_external_session_accept(&session);
    assert(!session.negotiating && session.pending);
    /* A renderer refusal leaves the transaction pending. */
    assert(session.pending);
    host_external_session_complete(&session);
    assert(!session.pending);
    host_external_session_connect(&session,2);
    assert(!host_external_session_disconnect(&session,1));
    assert(session.owner==2 && session.negotiating);
    assert(host_external_session_disconnect(&session,2));
    assert(!session.owner && !session.negotiating && session.pending);
    /* Reconnection may arrive before the return-to-device commit. */
    host_external_session_connect(&session,3);
    assert(session.pending && session.negotiating && session.owner==3);
    assert(!host_external_session_disconnect(&session,2));
    assert(host_external_session_disconnect(&session,3));
    host_external_session_complete(&session);
    assert(!session.pending);
    puts("PASS: scene ownership, replacement, disconnect before output, and reconnect before commit");
}
