#include "bottie/text_mask.hpp"
#include "bottie/network_policy.hpp"

#ifdef ESP_PLATFORM
#include "psa/crypto.h"
#else
#include <openssl/evp.h>
#endif

namespace bottie::network::policy {
bool text_digest_valid(const char* path, const uint8_t* bytes, size_t length) {
    if (!text_mask_path(path) || !bytes || length < 16 || length > text_mask_capacity) return false;
    uint8_t digest[32];
#ifdef ESP_PLATFORM
    size_t written = 0;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, bytes, length, digest, sizeof digest, &written) != PSA_SUCCESS ||
        written != sizeof digest) return false;
#else
    unsigned written = 0;
    if (EVP_Digest(bytes, length, digest, &written, EVP_sha256(), nullptr) != 1 || written != sizeof digest) return false;
#endif
    constexpr char hex[] = "0123456789abcdef";
    for (size_t n = 0; n < sizeof digest; ++n)
        if (path[9 + 2 * n] != hex[digest[n] >> 4] || path[10 + 2 * n] != hex[digest[n] & 15]) return false;
    return true;
}
} // namespace bottie::network::policy
