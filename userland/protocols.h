// Series of common protocols
#include <beans.h>

enum {
    // (name: String, val: Mailbox) -> ()
    NAME_REGISTER_DRIVER,
};

static int mailbox_send_2u_1t_0s(KHandle mailbox, int cmd, uintptr_t arg0, uintptr_t arg1, KHandle handle) {
    UTCB* utcb = get_utcb();
    utcb->hr[0] = handle;

    MSG_Tag tag = { .untyped = 2, .typed = 1, .cmd = cmd };
    return mailbox_send(mailbox, NULL_HANDLE, tag, NULL, arg0, arg1).cmd;
}

static void name_register_driver(KHandle root, int name, KHandle mailbox) {
    return mailbox_send_2u_1t_0s(root, NAME_REGISTER_DRIVER, name, 0, mailbox);
}

