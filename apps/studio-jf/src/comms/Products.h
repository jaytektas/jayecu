#pragma once
// Identity products this studio speaks the jayecu protocol with. The identity reply is
// "<product> <board> <version> <build> <layout_hash> <uid> t<size>"; the product is its first token.
//   jayecu — the engine ECU;  j8hp — the J8HP ZF 8HP transmission controller.
#include <cstdint>
#include <string>
#include <vector>

inline bool isKnownProduct(const std::string &sig)
{
    for (const char *p : { "jayecu", "j8hp" }) {
        const std::string tok = p;
        if (sig.rfind(tok, 0) == 0 && (sig.size() == tok.size() || sig[tok.size()] == ' '))
            return true;
    }
    return false;
}
inline bool isKnownProduct(const std::vector<uint8_t> &data)
{
    return isKnownProduct(std::string(data.begin(), data.end()));
}
