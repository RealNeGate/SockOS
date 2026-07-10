// Series of common protocols
#include <beans.h>

enum {
    // (name: String, val: Mailbox) -> ()
    NAME_REGISTER_DRIVER,
};

static int name_register_driver(UTCB* utcb, KHandle root, int name, KHandle handle) {
    utcb->mr[0] = name;
    utcb->hr[0] = handle;
    return mailbox_call(root, msg_tag(1, 1, 0, NAME_REGISTER_DRIVER)).cmd;
}

