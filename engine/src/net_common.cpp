#include "net_ws.h"

#include <mutex>

namespace tt {

void net_ensure_curl_init() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

} // namespace tt
