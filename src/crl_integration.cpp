#include <crl/crl_on_main.h>
#include <rpl/never.h>

// The application will replace this optional integration stream with its
// main-loop update producer. The bounded smoke has no update source, so this
// owned parent integration truthfully exposes a producer that never emits.
rpl::producer<> crl::on_main_update_requests() {
    return rpl::never<>();
}
