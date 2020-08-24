// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifdef HAVE_CONFIG_H
#include <config/bitcoin-config.h>
#endif

#include <hash.h>
#include <netaddress.h>
#include <random.h>
#include <tinyformat.h>
#include <util/asmap.h>
#include <util/bit_cast.h>
#include <util/strencodings.h>

#include <cstring>
#include <limits>

static const uint8_t pchIPv4[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
static const uint8_t pchOnionCat[] = {0xFD, 0x87, 0xD8, 0x7E, 0xEB, 0x43};

// 0xFD + sha256("bitcoin")[0:5]
static const uint8_t g_internal_prefix[] = {0xFD, 0x6B, 0x88, 0xC0, 0x87, 0x24};

static const uint8_t pchSingleAddressNetmask[16] =
    {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

void CNetAddr::SetIP(const CNetAddr &ipIn) {
    m_net = ipIn.m_net;
    std::memcpy(ip, ipIn.ip, sizeof(ip));
}

void CNetAddr::SetLegacyIPv6(const uint8_t ipv6[16])
{
    if (std::memcmp(ipv6, pchIPv4, sizeof(pchIPv4)) == 0) {
        m_net = NET_IPV4;
    } else if (std::memcmp(ipv6, pchOnionCat, sizeof(pchOnionCat)) == 0) {
        m_net = NET_ONION;
    } else if (std::memcmp(ipv6, g_internal_prefix, sizeof(g_internal_prefix)) == 0) {
        m_net = NET_INTERNAL;
    } else {
        m_net = NET_IPV6;
    }
    std::memcpy(ip, ipv6, 16);
}

void CNetAddr::SetRaw(Network network, const uint8_t *ip_in) {
    switch (network) {
        case NET_IPV4:
            m_net = NET_IPV4;
            std::memcpy(ip, pchIPv4, 12);
            std::memcpy(ip + 12, ip_in, 4);
            break;
        case NET_IPV6:
            SetLegacyIPv6(ip_in);
            break;
        default:
            assert(!"invalid network");
    }
}

/**
 * Try to make this a dummy address that maps the specified name into IPv6 like
 * so: (0xFD + %sha256("bitcoin")[0:5]) + %sha256(name)[0:10]. Such dummy
 * addresses have a prefix of fd6b:88c0:8724::/48 and are guaranteed to not be
 * publicly routable as it falls under RFC4193's fc00::/7 subnet allocated to
 * unique-local addresses.
 *
 * CAddrMan uses these fake addresses to keep track of which DNS seeds were
 * used.
 *
 * @returns Whether or not the operation was successful.
 *
 * @see CNetAddr::IsInternal(), CNetAddr::IsRFC4193()
 */
bool CNetAddr::SetInternal(const std::string &name) {
    if (name.empty()) {
        return false;
    }
    m_net = NET_INTERNAL;
    uint8_t hash[32] = {};
    CSHA256().Write(reinterpret_cast<const uint8_t *>(name.data()), name.size()).Finalize(hash);
    std::memcpy(ip, g_internal_prefix, sizeof(g_internal_prefix));
    std::memcpy(ip + sizeof(g_internal_prefix), hash, sizeof(ip) - sizeof(g_internal_prefix));
    return true;
}

/**
 * Try to make this a dummy address that maps the specified onion address into
 * IPv6 using OnionCat's range and encoding. Such dummy addresses have a prefix
 * of fd87:d87e:eb43::/48 and are guaranteed to not be publicly routable as they
 * fall under RFC4193's fc00::/7 subnet allocated to unique-local addresses.
 *
 * @returns Whether or not the operation was successful.
 *
 * @see CNetAddr::IsTor(), CNetAddr::IsRFC4193()
 */
bool CNetAddr::SetSpecial(const std::string &strName) {
    if (strName.size() > 6 &&
        strName.substr(strName.size() - 6, 6) == ".onion") {
        std::vector<uint8_t> vchAddr =
            DecodeBase32(strName.substr(0, strName.size() - 6).c_str());
        if (vchAddr.size() != 16 - sizeof(pchOnionCat)) {
            return false;
        }
        m_net = NET_ONION;
        std::memcpy(ip, pchOnionCat, sizeof(pchOnionCat));
        for (unsigned int i = 0; i < 16 - sizeof(pchOnionCat); i++) {
            ip[i + sizeof(pchOnionCat)] = vchAddr[i];
        }
        return true;
    }
    return false;
}

CNetAddr::CNetAddr(const struct in_addr &ipv4Addr) {
    SetRaw(NET_IPV4, (const uint8_t *)&ipv4Addr);
}

CNetAddr::CNetAddr(const struct in6_addr &ipv6Addr, const uint32_t scope) {
    SetRaw(NET_IPV6, (const uint8_t *)&ipv6Addr);
    scopeId = scope;
}

unsigned int CNetAddr::GetByte(int n) const {
    return ip[15 - n];
}

bool CNetAddr::IsIPv4() const {
    return m_net == NET_IPV4;
}

bool CNetAddr::IsIPv6() const {
    return m_net == NET_IPV6;
}

bool CNetAddr::IsRFC1918() const {
    return IsIPv4() &&
           (GetByte(3) == 10 || (GetByte(3) == 192 && GetByte(2) == 168) ||
            (GetByte(3) == 172 && (GetByte(2) >= 16 && GetByte(2) <= 31)));
}

bool CNetAddr::IsRFC2544() const {
    return IsIPv4() && GetByte(3) == 198 &&
           (GetByte(2) == 18 || GetByte(2) == 19);
}

bool CNetAddr::IsRFC3927() const {
    return IsIPv4() && (GetByte(3) == 169 && GetByte(2) == 254);
}

bool CNetAddr::IsRFC6598() const {
    return IsIPv4() && GetByte(3) == 100 && GetByte(2) >= 64 &&
           GetByte(2) <= 127;
}

bool CNetAddr::IsRFC5737() const {
    return IsIPv4() &&
           ((GetByte(3) == 192 && GetByte(2) == 0 && GetByte(1) == 2) ||
            (GetByte(3) == 198 && GetByte(2) == 51 && GetByte(1) == 100) ||
            (GetByte(3) == 203 && GetByte(2) == 0 && GetByte(1) == 113));
}

bool CNetAddr::IsRFC3849() const {
    return IsIPv6() && GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x0D && GetByte(12) == 0xB8;
}

bool CNetAddr::IsRFC3964() const {
    return IsIPv6() && GetByte(15) == 0x20 && GetByte(14) == 0x02;
}

bool CNetAddr::IsRFC6052() const {
    static const uint8_t pchRFC6052[] = {0, 0x64, 0xFF, 0x9B, 0, 0, 0, 0, 0, 0, 0, 0};
    return IsIPv6() && std::memcmp(ip, pchRFC6052, sizeof(pchRFC6052)) == 0;
}

bool CNetAddr::IsRFC4380() const {
    return IsIPv6() && GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0 && GetByte(12) == 0;
}

bool CNetAddr::IsRFC4862() const {
    static const uint8_t pchRFC4862[] = {0xFE, 0x80, 0, 0, 0, 0, 0, 0};
    return IsIPv6() && std::memcmp(ip, pchRFC4862, sizeof(pchRFC4862)) == 0;
}

bool CNetAddr::IsRFC4193() const {
    return IsIPv6() && (GetByte(15) & 0xFE) == 0xFC;
}

bool CNetAddr::IsRFC6145() const {
    static const uint8_t pchRFC6145[] = {0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0, 0};
    return IsIPv6() && std::memcmp(ip, pchRFC6145, sizeof(pchRFC6145)) == 0;
}

bool CNetAddr::IsRFC4843() const {
    return IsIPv6() && GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x00 &&
           (GetByte(12) & 0xF0) == 0x10;
}

bool CNetAddr::IsRFC7343() const {
    return IsIPv6() && GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x00 &&
           (GetByte(12) & 0xF0) == 0x20;
}

bool CNetAddr::IsHeNet() const {
    return (GetByte(15) == 0x20 && GetByte(14) == 0x01 && GetByte(13) == 0x04 && GetByte(12) == 0x70);
}

/**
 * @returns Whether or not this is a dummy address that maps an onion address
 *          into IPv6.
 *
 * @see CNetAddr::SetSpecial(const std::string &)
 */
bool CNetAddr::IsTor() const {
    return m_net == NET_ONION;
}

bool CNetAddr::IsLocal() const {
    // IPv4 loopback (127.0.0.0/8 or 0.0.0.0/8)
    if (IsIPv4() && (GetByte(3) == 127 || GetByte(3) == 0)) {
        return true;
    }

    // IPv6 loopback (::1/128)
    static const uint8_t pchLocal[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (IsIPv6() && std::memcmp(ip, pchLocal, 16) == 0) {
        return true;
    }

    return false;
}

/**
 * @returns Whether or not this network address is a valid address that @a could
 *          be used to refer to an actual host.
 *
 * @note A valid address may or may not be publicly routable on the global
 *       internet. As in, the set of valid addresses is a superset of the set of
 *       publicly routable addresses.
 *
 * @see CNetAddr::IsRoutable()
 */
bool CNetAddr::IsValid() const
{
    // unspecified IPv6 address (::/128)
    uint8_t ipNone6[16] = {};
    if (IsIPv6() && std::memcmp(ip, ipNone6, 16) == 0) {
        return false;
    }

    // documentation IPv6 address
    if (IsRFC3849()) {
        return false;
    }

    if (IsInternal()) {
        return false;
    }

    if (IsIPv4()) {
        // INADDR_NONE
        uint32_t ipNone = INADDR_NONE;
        if (std::memcmp(ip + 12, &ipNone, 4) == 0) {
            return false;
        }

        // 0
        ipNone = 0;
        if (std::memcmp(ip + 12, &ipNone, 4) == 0) {
            return false;
        }
    }

    return true;
}

/**
 * @returns Whether or not this network address is publicly routable on the
 *          global internet.
 *
 * @note A routable address is always valid. As in, the set of routable addreses
 *       is a subset of the set of valid addresses.
 *
 * @see CNetAddr::IsValid()
 */
bool CNetAddr::IsRoutable() const {
    return IsValid() &&
           !(IsRFC1918() || IsRFC2544() || IsRFC3927() || IsRFC4862() ||
             IsRFC6598() || IsRFC5737() || (IsRFC4193() && !IsTor()) ||
             IsRFC4843() || IsRFC7343() || IsLocal() || IsInternal());
}

/**
 * @returns Whether or not this is a dummy address that maps a name into IPv6.
 *
 * @see CNetAddr::SetInternal(const std::string &)
 */
bool CNetAddr::IsInternal() const {
    return m_net == NET_INTERNAL;
}

enum Network CNetAddr::GetNetwork() const {
    if (IsInternal()) {
        return NET_INTERNAL;
    }

    if (!IsRoutable()) {
        return NET_UNROUTABLE;
    }

    return m_net;
}

std::string CNetAddr::ToStringIP() const {
    if (IsTor()) {
        return EncodeBase32(&ip[6], 10) + ".onion";
    }
    if (IsInternal()) {
        return EncodeBase32(ip + sizeof(g_internal_prefix),
                            sizeof(ip) - sizeof(g_internal_prefix)) +
               ".internal";
    }
    CService serv(*this, 0);

    if (const auto optPair = serv.GetSockAddr()) {
        auto & [sockaddr, socklen] = *optPair;
        char name[1025] = "";
        if (!getnameinfo((const struct sockaddr *)&sockaddr, socklen, name,
                         sizeof(name), nullptr, 0, NI_NUMERICHOST)) {
            return std::string(name);
        }
    }
    if (IsIPv4()) {
        return strprintf("%u.%u.%u.%u", GetByte(3), GetByte(2), GetByte(1),
                         GetByte(0));
    }

    return strprintf("%x:%x:%x:%x:%x:%x:%x:%x", GetByte(15) << 8 | GetByte(14),
                     GetByte(13) << 8 | GetByte(12),
                     GetByte(11) << 8 | GetByte(10),
                     GetByte(9) << 8 | GetByte(8), GetByte(7) << 8 | GetByte(6),
                     GetByte(5) << 8 | GetByte(4), GetByte(3) << 8 | GetByte(2),
                     GetByte(1) << 8 | GetByte(0));
}

std::string CNetAddr::ToString() const {
    return ToStringIP();
}

bool operator==(const CNetAddr &a, const CNetAddr &b) {
    return a.m_net == b.m_net && std::memcmp(a.ip, b.ip, 16) == 0;
}

bool operator<(const CNetAddr &a, const CNetAddr &b) {
    return a.m_net < b.m_net || (a.m_net == b.m_net && std::memcmp(a.ip, b.ip, 16) < 0);
}

/**
 * Try to get our IPv4 address.
 *
 * @param[out] pipv4Addr The in_addr struct to which to copy.
 *
 * @returns Whether or not the operation was successful, in particular, whether
 *          or not our address was an IPv4 address.
 *
 * @see CNetAddr::IsIPv4()
 */
bool CNetAddr::GetInAddr(struct in_addr *pipv4Addr) const {
    if (!IsIPv4()) {
        return false;
    }
    std::memcpy(pipv4Addr, ip + 12, 4);
    return true;
}

/**
 * Try to get our IPv6 address.
 *
 * @param[out] pipv6Addr The in6_addr struct to which to copy.
 *
 * @returns Whether or not the operation was successful, in particular, whether
 *          or not our address was an IPv6 address.
 *
 * @see CNetAddr::IsIPv6()
 */
bool CNetAddr::GetIn6Addr(struct in6_addr *pipv6Addr) const {
    if (!IsIPv6()) {
        return false;
    }
    std::memcpy(pipv6Addr, ip, 16);
    return true;
}

bool CNetAddr::HasLinkedIPv4() const {
    return IsRoutable() && (IsIPv4() || IsRFC6145() || IsRFC6052() || IsRFC3964() || IsRFC4380());
}

uint32_t CNetAddr::GetLinkedIPv4() const {
    if (IsIPv4() || IsRFC6145() || IsRFC6052()) {
        // IPv4, mapped IPv4, SIIT translated IPv4: the IPv4 address is the last 4 bytes of the address
        return ReadBE32(ip + 12);
    } else if (IsRFC3964()) {
        // 6to4 tunneled IPv4: the IPv4 address is in bytes 2-6
        return ReadBE32(ip + 2);
    } else if (IsRFC4380()) {
        // Teredo tunneled IPv4: the IPv4 address is in the last 4 bytes of the address, but bitflipped
        return ~ReadBE32(ip + 12);
    }
    assert(false);
}

uint8_t CNetAddr::GetNetClass() const {
    uint8_t net_class = NET_IPV6;
    if (IsLocal()) {
        net_class = 255;
    }
    if (IsInternal()) {
        net_class = NET_INTERNAL;
    } else if (!IsRoutable()) {
        net_class = NET_UNROUTABLE;
    } else if (HasLinkedIPv4()) {
        net_class = NET_IPV4;
    } else if (IsTor()) {
        net_class = NET_ONION;
    }
    return net_class;
}

uint32_t CNetAddr::GetMappedAS(const std::vector<bool> &asmap) const {
    if (uint8_t net_class;
            asmap.size() == 0 || ((net_class = GetNetClass()) != NET_IPV4 && net_class != NET_IPV6)) {
        return 0; // Indicates not found, safe because AS0 is reserved per RFC7607.
    }
    std::vector<bool> ip_bits(128);
    if (HasLinkedIPv4()) {
        // For lookup, treat as if it was just an IPv4 address (pchIPv4 prefix + IPv4 bits)
        for (int8_t byte_i = 0; byte_i < 12; ++byte_i) {
            for (uint8_t bit_i = 0; bit_i < 8; ++bit_i) {
                ip_bits[byte_i * 8 + bit_i] = (pchIPv4[byte_i] >> (7 - bit_i)) & 1;
            }
        }
        uint32_t ipv4 = GetLinkedIPv4();
        for (int i = 0; i < 32; ++i) {
            ip_bits[96 + i] = (ipv4 >> (31 - i)) & 1;
        }
    } else {
        // Use all 128 bits of the IPv6 address otherwise
        for (int8_t byte_i = 0; byte_i < 16; ++byte_i) {
            uint8_t cur_byte = GetByte(15 - byte_i);
            for (uint8_t bit_i = 0; bit_i < 8; ++bit_i) {
                ip_bits[byte_i * 8 + bit_i] = (cur_byte >> (7 - bit_i)) & 1;
            }
        }
    }
    uint32_t mapped_as = Interpret(asmap, ip_bits);
    return mapped_as;
}

/**
 * Get the canonical identifier of our network group
 *
 * The groups are assigned in a way where it should be costly for an attacker to
 * obtain addresses with many different group identifiers, even if it is cheap
 * to obtain addresses with the same identifier.
 *
 * @note No two connections will be attempted to addresses with the same network
 *       group.
 */
std::vector<uint8_t> CNetAddr::GetGroup(const std::vector<bool> &asmap) const {
    std::vector<uint8_t> vchRet;
    // If non-empty asmap is supplied and the address is IPv4/IPv6,
    // return ASN to be used for bucketing.
    uint32_t asn = GetMappedAS(asmap);
    if (asn != 0) { // Either asmap was empty, or address has non-asmappable net class (e.g. TOR).
        vchRet.push_back(NET_IPV6); // IPv4 and IPv6 with same ASN should be in the same bucket
        for (int i = 0; i < 4; i++) {
            vchRet.push_back((asn >> (8 * i)) & 0xFF);
        }
        return vchRet;
    }

    vchRet.push_back(GetNetClass());
    int nStartByte = 0;
    int nBits = 16;

    if (IsLocal()) {
        // all local addresses belong to the same group
        nBits = 0;
    } else if (IsInternal()) {
        // all internal-usage addresses get their own group
        nStartByte = sizeof(g_internal_prefix);
        nBits = (sizeof(ip) - sizeof(g_internal_prefix)) * 8;
    } else if (!IsRoutable()) {
        // all other unroutable addresses belong to the same group
        nBits = 0;
    } else if (HasLinkedIPv4()) {
        // IPv4 addresses (and mapped IPv4 addresses) use /16 groups
        uint32_t ipv4 = GetLinkedIPv4();
        vchRet.push_back((ipv4 >> 24) & 0xFF);
        vchRet.push_back((ipv4 >> 16) & 0xFF);
        return vchRet;
    } else if (IsTor()) {
        nStartByte = 6;
        nBits = 4;
    } else if (IsHeNet()) {
        // for he.net, use /36 groups
        nBits = 36;
    } else {
        // for the rest of the IPv6 network, use /32 groups
        nBits = 32;
    }

    // push our ip onto vchRet byte by byte...
    while (nBits >= 8) {
        vchRet.push_back(GetByte(15 - nStartByte));
        nStartByte++;
        nBits -= 8;
    }
    // ...for the last byte, push nBits and for the rest of the byte push 1's
    if (nBits > 0) {
        vchRet.push_back(GetByte(15 - nStartByte) | ((1 << (8 - nBits)) - 1));
    }

    return vchRet;
}

uint64_t CNetAddr::GetHash() const {
    uint256 hash = Hash(&ip[0], &ip[16]);
    uint64_t nRet;
    std::memcpy(&nRet, &hash, sizeof(nRet));
    return nRet;
}

/** Calculates a metric for how reachable (*this) is from a given partner */
int CNetAddr::GetReachabilityFrom(const CNetAddr *paddrPartner) const {

    // private extensions to enum Network, only returned by GetExtNetwork, and only
    // used in GetReachabilityFrom
    static constexpr int NET_UNKNOWN = NET_MAX + 0;
    static constexpr int NET_TEREDO = NET_MAX + 1;
    static auto GetExtNetwork = [](const CNetAddr *addr) -> int {
        if (addr == nullptr) {
            return NET_UNKNOWN;
        }
        if (addr->IsRFC4380()) {
            return NET_TEREDO;
        }
        return addr->GetNetwork();
    };

    enum Reachability {
        REACH_UNREACHABLE,
        REACH_DEFAULT,
        REACH_TEREDO,
        REACH_IPV6_WEAK,
        REACH_IPV4,
        REACH_IPV6_STRONG,
        REACH_PRIVATE
    };

    if (!IsRoutable() || IsInternal()) {
        return REACH_UNREACHABLE;
    }

    int ourNet = GetExtNetwork(this);
    int theirNet = GetExtNetwork(paddrPartner);
    bool fTunnel = IsRFC3964() || IsRFC6052() || IsRFC6145();

    switch (theirNet) {
        case NET_IPV4:
            switch (ourNet) {
                default:
                    return REACH_DEFAULT;
                case NET_IPV4:
                    return REACH_IPV4;
            }
        case NET_IPV6:
            switch (ourNet) {
                default:
                    return REACH_DEFAULT;
                case NET_TEREDO:
                    return REACH_TEREDO;
                case NET_IPV4:
                    return REACH_IPV4;
                // only prefer giving our IPv6 address if it's not tunnelled
                case NET_IPV6:
                    return fTunnel ? REACH_IPV6_WEAK : REACH_IPV6_STRONG;
            }
        case NET_ONION:
            switch (ourNet) {
                default:
                    return REACH_DEFAULT;
                // Tor users can connect to IPv4 as well
                case NET_IPV4:
                    return REACH_IPV4;
                case NET_ONION:
                    return REACH_PRIVATE;
            }
        case NET_TEREDO:
            switch (ourNet) {
                default:
                    return REACH_DEFAULT;
                case NET_TEREDO:
                    return REACH_TEREDO;
                case NET_IPV6:
                    return REACH_IPV6_WEAK;
                case NET_IPV4:
                    return REACH_IPV4;
            }
        case NET_UNKNOWN:
        case NET_UNROUTABLE:
        default:
            switch (ourNet) {
                default:
                    return REACH_DEFAULT;
                case NET_TEREDO:
                    return REACH_TEREDO;
                case NET_IPV6:
                    return REACH_IPV6_WEAK;
                case NET_IPV4:
                    return REACH_IPV4;
                // either from Tor, or don't care about our address
                case NET_ONION:
                    return REACH_PRIVATE;
            }
    }
}

CService::CService(const struct sockaddr_in &addr)
    : CNetAddr(addr.sin_addr), port(ntohs(addr.sin_port)) {
    assert(addr.sin_family == AF_INET);
}

CService::CService(const struct sockaddr_in6 &addr)
    : CNetAddr(addr.sin6_addr, addr.sin6_scope_id),
      port(ntohs(addr.sin6_port)) {
    assert(addr.sin6_family == AF_INET6);
}

bool CService::SetSockAddr(const sockaddr_storage &addr) {
    switch (addr.ss_family) {
        case AF_INET:
            *this = CService(bit_cast<sockaddr_in>(addr));
            return true;
        case AF_INET6:
            *this = CService(bit_cast<sockaddr_in6>(addr));
            return true;
        default:
            return false;
    }
}

bool operator==(const CService &a, const CService &b) {
    return static_cast<CNetAddr>(a) == static_cast<CNetAddr>(b) &&
           a.port == b.port;
}

bool operator<(const CService &a, const CService &b) {
    return static_cast<CNetAddr>(a) < static_cast<CNetAddr>(b) ||
           (static_cast<CNetAddr>(a) == static_cast<CNetAddr>(b) &&
            a.port < b.port);
}

/**
 * Obtain the IPv4/6 socket address this represents.
 *
 * @returns An optional sockaddr / length pair when successful.
 */
std::optional<std::pair<sockaddr_storage, socklen_t>> CService::GetSockAddr() const {
    std::optional<std::pair<sockaddr_storage, socklen_t>> ret;
    if (IsIPv4()) {
        constexpr socklen_t addrlen = sizeof(sockaddr_in);
        static_assert(addrlen <= sizeof(sockaddr_storage));
        sockaddr_in addrin = {}; // 0-init
        if (!GetInAddr(&addrin.sin_addr)) {
            return ret;
        }
        addrin.sin_family = AF_INET;
        addrin.sin_port = htons(port);
        ret.emplace(sockaddr_storage{}, addrlen);
        std::memcpy(&ret->first, &addrin, addrlen);
    } else if (IsIPv6()) {
        constexpr socklen_t addrlen = sizeof(sockaddr_in6);
        static_assert(addrlen <= sizeof(sockaddr_storage));
        sockaddr_in6 addrin6 = {}; // 0-init
        if (!GetIn6Addr(&addrin6.sin6_addr)) {
            return ret;
        }
        addrin6.sin6_scope_id = scopeId;
        addrin6.sin6_family = AF_INET6;
        addrin6.sin6_port = htons(port);
        ret.emplace(sockaddr_storage{}, addrlen);
        std::memcpy(&ret->first, &addrin6, addrlen);
    }
    return ret;
}

/**
 * @returns An identifier unique to this service's address and port number.
 */
std::vector<uint8_t> CService::GetKey() const {
    auto addr = GetAddressBytes();
    std::vector<uint8_t> key(addr, addr + GetAddressLen());
    key.push_back(port >> 8); // most significant byte of our port
    key.push_back(port & 0x0FF); // least significant byte of our port
    return key;
}

std::string CService::ToStringPort() const {
    return strprintf("%u", port);
}

std::string CService::ToStringIPPort() const {
    if (IsIPv4() || IsTor() || IsInternal()) {
        return ToStringIP() + ":" + ToStringPort();
    } else {
        return "[" + ToStringIP() + "]:" + ToStringPort();
    }
}

std::string CService::ToString() const {
    return ToStringIPPort();
}

CSubNet::CSubNet(const CNetAddr &addr, int32_t mask)
    : network(addr), valid(true)
{
    // Default to /32 (IPv4) or /128 (IPv6), i.e. match single address
    std::memcpy(netmask, pchSingleAddressNetmask, sizeof(netmask));

    // IPv4 addresses start at offset 12, and first 12 bytes must match, so just
    // offset n
    const int astartofs = network.IsIPv4() ? 12 : 0;

    // Only valid if in range of bits of address
    int32_t n = mask;
    if (n >= 0 && n <= (128 - astartofs * 8)) {
        n += astartofs * 8;
        // Clear bits [n..127]
        for (; n < 128; ++n) {
            netmask[n >> 3] &= ~(1 << (7 - (n & 7)));
        }
    } else {
        valid = false;
    }

    // Normalize network according to netmask
    for (int x = 0; x < 16; ++x) {
        network.ip[x] &= netmask[x];
    }
}

/**
 * @returns The number of 1-bits in the prefix of the specified subnet mask. If
 *          the specified subnet mask is not a valid one, -1.
 */
static inline int NetmaskBits(uint8_t x) {
    switch (x) {
        case 0x00:
            return 0;
        case 0x80:
            return 1;
        case 0xc0:
            return 2;
        case 0xe0:
            return 3;
        case 0xf0:
            return 4;
        case 0xf8:
            return 5;
        case 0xfc:
            return 6;
        case 0xfe:
            return 7;
        case 0xff:
            return 8;
        default:
            return -1;
    }
}

CSubNet::CSubNet(const CNetAddr &addr, const CNetAddr &mask)
    : network(addr), valid(true)
{
    // Check if `mask` contains 1-bits after 0-bits (which is an invalid netmask).
    bool zeros_found = false;
    for (size_t i = mask.IsIPv4() ? 12 : 0; i < sizeof(mask.ip); ++i) {
        const int num_bits = NetmaskBits(mask.ip[i]);
        if (num_bits == -1 || (zeros_found && num_bits != 0)) {
            valid = false;
            return;
        }
        if (num_bits < 8) {
            zeros_found = true;
        }
    }

    // Default to /32 (IPv4) or /128 (IPv6), i.e. match single address
    std::memcpy(netmask, pchSingleAddressNetmask, sizeof(netmask));

    // IPv4 addresses start at offset 12, and first 12 bytes must match, so just
    // offset n
    const int astartofs = network.IsIPv4() ? 12 : 0;

    for (int x = astartofs; x < 16; ++x) {
        netmask[x] = mask.ip[x];
    }

    // Normalize network according to netmask
    for (int x = 0; x < 16; ++x) {
        network.ip[x] &= netmask[x];
    }
}

CSubNet::CSubNet(const CNetAddr &addr)
    : network(addr), valid(addr.IsValid())
{
    static_assert (sizeof(netmask) == sizeof(pchSingleAddressNetmask),
                   "netmask and pchSingleAddressNetmask must be the same size" );
    std::memcpy(netmask, pchSingleAddressNetmask, sizeof(netmask));
}

/**
 * @returns True if this subnet is valid, the specified address is valid, and
 *          the specified address belongs in this subnet.
 */
bool CSubNet::Match(const CNetAddr &addr) const {
    if (!valid || !addr.IsValid() || network.m_net != addr.m_net) {
        return false;
    }
    for (int x = 0; x < 16; ++x) {
        if ((addr.ip[x] & netmask[x]) != network.ip[x]) {
            return false;
        }
    }
    return true;
}

std::string CSubNet::ToString() const {
    uint8_t cidr = 0;

    for (size_t i = network.IsIPv4() ? 12 : 0; i < sizeof(netmask); ++i) {
        if (netmask[i] == 0x00) {
            break;
        }
        cidr += NetmaskBits(netmask[i]);
    }

    return network.ToString() + strprintf("/%u", cidr);
}

bool CSubNet::IsSingleIP() const {
    return 0 == std::memcmp(netmask, pchSingleAddressNetmask, sizeof(pchSingleAddressNetmask));
}

bool operator==(const CSubNet &a, const CSubNet &b) {
    return a.valid == b.valid && a.network == b.network &&
           !std::memcmp(a.netmask, b.netmask, 16);
}

bool operator<(const CSubNet &a, const CSubNet &b) {
    return a.network < b.network ||
           (a.network == b.network && std::memcmp(a.netmask, b.netmask, 16) < 0);
}


// std::unordered_map support --

size_t SaltedNetAddrHasher::operator()(const CNetAddr &addr) const
{
    return static_cast<size_t>(SerializeSipHash(addr, k0(), k1()));
}

size_t SaltedSubNetHasher::operator()(const CSubNet &subnet) const
{
    return static_cast<size_t>(SerializeSipHash(subnet, k0(), k1()));
}

bool SanityCheckASMap(const std::vector<bool> &asmap) {
    return SanityCheckASMap(asmap, 128); // For IP address lookups, the input is 128 bits
}
