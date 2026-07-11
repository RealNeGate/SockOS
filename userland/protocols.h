// Series of common protocols
#include <beans.h>

enum {
    // Name server protocol
    NAME_REGISTER_DRIVER,
    NAME_FIND_DRIVER,

    // USB protocol
    USB_CTRL_XFER,
    USB_BULK_XFER,
};

static int name_register_driver(UTCB* utcb, KHandle root, int name, KHandle handle) {
    utcb->mr[0] = name;
    utcb->hr[0] = handle;
    return mailbox_call(root, msg_tag(1, 1, 0, NAME_REGISTER_DRIVER)).cmd;
}

