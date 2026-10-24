#include "netplay/net_socket.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>

#include <cstdio>
#include <cstring>
#include <chrono>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace Netplay {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalid = INVALID_SOCKET;
void CloseNative(NativeSocket s) { closesocket(s); }

bool EnsureWinsock() {
    static const bool ok = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
}
#else
using NativeSocket = int;
constexpr NativeSocket kInvalid = -1;
void CloseNative(NativeSocket s) { ::close(s); }
bool EnsureWinsock() { return true; }
#endif

NativeSocket Native(intptr_t value) { return static_cast<NativeSocket>(value); }

sockaddr_in ToSockaddr(const Address& address) {
    sockaddr_in out{};
    out.sin_family = AF_INET;
    out.sin_addr.s_addr = htonl(address.ip);
    out.sin_port = htons(address.port);
    return out;
}

std::mutex g_nameMutex;
std::string g_deviceName;

} // namespace

std::string IpToString(uint32_t ip) {
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u", (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return text;
}

std::string ToString(const Address& address) {
    return IpToString(address.ip) + ":" + std::to_string(address.port);
}

bool ParseAddress(std::string_view text, uint16_t defaultPort, Address& out) {
    uint32_t parts[4] = {};
    size_t index = 0;
    size_t pos = 0;
    for (; index < 4; ++index) {
        if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') {
            return false;
        }
        uint32_t value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            value = value * 10 + static_cast<uint32_t>(text[pos++] - '0');
            if (value > 255) {
                return false;
            }
        }
        parts[index] = value;
        if (index < 3) {
            if (pos >= text.size() || text[pos] != '.') {
                return false;
            }
            ++pos;
        }
    }
    uint32_t port = defaultPort;
    if (pos < text.size()) {
        if (text[pos] != ':') {
            return false;
        }
        ++pos;
        port = 0;
        if (pos >= text.size()) {
            return false;
        }
        for (; pos < text.size(); ++pos) {
            if (text[pos] < '0' || text[pos] > '9') {
                return false;
            }
            port = port * 10 + static_cast<uint32_t>(text[pos] - '0');
            if (port > 65535) {
                return false;
            }
        }
    }
    out.ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    out.port = static_cast<uint16_t>(port);
    return out.ip != 0 && out.port != 0;
}

bool ResolveAddress(const std::string& text, uint16_t defaultPort, Address& out) {
    if (ParseAddress(text, defaultPort, out)) {
        return true;
    }
    if (!EnsureWinsock()) {
        return false;
    }
    std::string host = text;
    uint16_t port = defaultPort;
    if (const size_t colon = text.rfind(':'); colon != std::string::npos) {
        host = text.substr(0, colon);
        port = static_cast<uint16_t>(std::strtoul(text.c_str() + colon + 1, nullptr, 10));
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        return false;
    }
    const auto* in = reinterpret_cast<const sockaddr_in*>(result->ai_addr);
    out.ip = ntohl(in->sin_addr.s_addr);
    out.port = port;
    freeaddrinfo(result);
    return out.Valid();
}

bool IsTailscaleIp(uint32_t ip) {
    return (ip & 0xFFC00000u) == 0x64400000u;  // 100.64.0.0/10
}

UdpSocket::~UdpSocket() {
    Close();
}

bool UdpSocket::Open(uint16_t port) {
    Close();
    if (!EnsureWinsock()) {
        return false;
    }
    const NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalid) {
        return false;
    }
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef _WIN32
    // A datagram to a port nobody listens on would otherwise fail the next recvfrom with
    // WSAECONNRESET; a peer that quit is not an error for us.
    BOOL reportReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, _WSAIOW(IOC_VENDOR, 12), &reportReset, sizeof(reportReset), nullptr, 0, &bytes, nullptr, nullptr);
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
#ifdef _WIN32
    // Any port: leave the socket unbound and let the first send pick one. An explicit bind makes
    // Windows Firewall ask about letting the game accept connections, which only a host needs.
    if (port == 0) {
        m_socket = static_cast<intptr_t>(s);
        m_port = 0;
        m_bound = false;
        return true;
    }
#endif
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(port);
    if (::bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
        CloseNative(s);
        return false;
    }
    sockaddr_in bound{};
    socklen_t length = sizeof(bound);
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &length);
    m_socket = static_cast<intptr_t>(s);
    m_port = ntohs(bound.sin_port);
    m_bound = true;
    return true;
}

void UdpSocket::Close() {
    if (m_socket != -1) {
        CloseNative(Native(m_socket));
        m_socket = -1;
        m_port = 0;
        m_bound = false;
    }
}

bool UdpSocket::IsOpen() const {
    return m_socket != -1;
}

// ---- the online server's relay (Relay in net_socket.h) ----
namespace {

constexpr uint8_t kRelayRegister = 0xB1, kRelayRegistered = 0xB2, kRelayForward = 0xB3, kRelayList = 0xB5,
                  kRelayListReply = 0xB6;
constexpr uint16_t kRelayVersion = 1;
constexpr uint16_t kRelayDefaultPort = 52200;
// The public server, once there is one; Config.toml [online] server or BRAWL_ONLINE_SERVER overrides it.
constexpr const char* kDefaultOnlineServer = "";
constexpr char kCodeLetters[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O or 1/I to mix up

struct RelayState {
    std::mutex mutex;
    bool loaded = false;
    std::string serverText;
    Address server;
    int64_t lastResolve = -100000;
    uint32_t id = 0;
    int64_t lastRegistered = -100000;
    uint8_t listMax = 0, listPlayers = 0;
    std::string listName, listBuild;
    bool listRequested = false;
    std::vector<Relay::ListedRoom> listed;
};

RelayState& R() {
    static RelayState state;
    return state;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// This game's id: made once and kept with its settings, so it is the same game after the restart into a match.
uint32_t LoadRelayId() {
    const auto path = RuntimeConfigFile::ApplicationDataDirectory() / "Netplay" / "online_id";
    std::error_code ec;
    if (std::ifstream in(path); in) {
        uint32_t id = 0;
        if (in >> std::hex >> id && (id & 0xFFFFFFu) != 0) return id & 0xFFFFFFu;
    }
    std::random_device random;
    uint32_t id = 0;
    while ((id & 0xFFFFFFu) == 0) id = random();
    id &= 0xFFFFFFu;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream(path) << std::hex << id << "\n";
    return id;
}

void LoadRelayLocked(RelayState& r) {
    if (r.loaded) return;
    r.loaded = true;
    const char* env = std::getenv("BRAWL_ONLINE_SERVER");
    r.serverText = env != nullptr ? env : RuntimeConfigFile::OnlineServer(kDefaultOnlineServer);
    r.id = LoadRelayId();
}

// The server's address, resolved now and then (a name may move); invalid when none is set or it can't be found.
Address RelayServerLocked(RelayState& r) {
    LoadRelayLocked(r);
    if (r.serverText.empty()) return {};
    const int64_t now = NowMs();
    if (!r.server.Valid() ? now - r.lastResolve > 10000 : now - r.lastResolve > 300000) {
        r.lastResolve = now;
        Address resolved;
        if (ResolveAddress(r.serverText, kRelayDefaultPort, resolved)) {
            r.server = resolved;
        } else {
            RT_LOGF(RT_TAG_RUNTIME, "online: can't find the server %s\n", r.serverText.c_str());
        }
    }
    return r.server;
}

void PutU24(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
}

uint32_t GetU24(const uint8_t* p) {
    return p[0] | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16);
}

void PutStr8(std::vector<uint8_t>& out, const std::string& text) {
    const size_t n = std::min<size_t>(text.size(), 63);
    out.push_back(static_cast<uint8_t>(n));
    out.insert(out.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(n));
}

bool GetStr8(const uint8_t* data, size_t size, size_t& pos, std::string& out) {
    if (pos >= size || pos + 1 + data[pos] > size) return false;
    out.assign(reinterpret_cast<const char*>(data + pos + 1), data[pos]);
    pos += 1 + data[pos];
    return true;
}

} // namespace

namespace Relay {

bool Enabled() {
    RelayState& r = R();
    std::lock_guard<std::mutex> lock(r.mutex);
    LoadRelayLocked(r);
    return !r.serverText.empty();
}

bool Connected() {
    RelayState& r = R();
    std::lock_guard<std::mutex> lock(r.mutex);
    return NowMs() - r.lastRegistered < 6000;
}

bool IsRelayAddress(const Address& address) {
    return (address.ip >> 24) == 240;
}

Address OwnAddress() {
    RelayState& r = R();
    std::lock_guard<std::mutex> lock(r.mutex);
    LoadRelayLocked(r);
    return Address{(240u << 24) | r.id, 1};
}

std::string Code() {
    uint32_t id = OwnAddress().ip & 0xFFFFFFu;
    std::string code(5, 'A');
    for (int i = 4; i >= 0; --i, id >>= 5) code[static_cast<size_t>(i)] = kCodeLetters[id & 31];
    return code;
}

bool CodeToAddress(std::string_view code, Address& out) {
    if (ParseAddress(code, 1, out) && IsRelayAddress(out)) return true;
    uint32_t id = 0;
    int letters = 0;
    for (char c : code) {
        if (c == ' ' || c == '-') continue;
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        const char* at = c != '\0' ? std::strchr(kCodeLetters, c) : nullptr;
        if (at == nullptr) return false;
        id = (id << 5) | static_cast<uint32_t>(at - kCodeLetters);
        ++letters;
    }
    if (letters != 5 || id == 0 || id > 0xFFFFFFu) return false;
    out = Address{(240u << 24) | id, 1};
    return true;
}

void SetListing(uint8_t maxPlayers, uint8_t players, const std::string& name, const std::string& build) {
    RelayState& r = R();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.listMax = maxPlayers;
    r.listPlayers = players;
    r.listName = name;
    r.listBuild = build;
}

std::vector<ListedRoom> Listed() {
    RelayState& r = R();
    std::lock_guard<std::mutex> lock(r.mutex);
    return r.listed;
}

void RequestList() {
    RelayState& r = R();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.listRequested = true;
}

} // namespace Relay

// Keeps this socket registered with the server (every 2 s) and asks for the room list when wanted.
void UdpSocket::RelayTick() {
    RelayState& r = R();
    Address server;
    std::vector<uint8_t> reg, list;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        server = RelayServerLocked(r);
        if (!server.Valid()) return;
        const int64_t now = NowMs();
        if (now - m_lastRelayRegister >= 2000) {
            m_lastRelayRegister = now;
            reg = {kRelayRegister, static_cast<uint8_t>(kRelayVersion), static_cast<uint8_t>(kRelayVersion >> 8)};
            PutU24(reg, r.id);
            reg.push_back(r.listMax != 0 ? 1 : 0);
            if (r.listMax != 0) {
                reg.push_back(r.listMax);
                reg.push_back(r.listPlayers);
                PutStr8(reg, r.listName);
                PutStr8(reg, r.listBuild);
            }
        }
        if (r.listRequested) {
            r.listRequested = false;
            list = {kRelayList};
        }
    }
    if (!reg.empty()) SendRaw(server, reg.data(), reg.size());
    if (!list.empty()) SendRaw(server, list.data(), list.size());
}

bool UdpSocket::SendTo(const Address& to, const void* data, size_t size) {
    if (!IsOpen() || !to.Valid()) {
        return false;
    }
    if (Relay::IsRelayAddress(to)) {
        RelayTick();
        Address server;
        {
            std::lock_guard<std::mutex> lock(R().mutex);
            server = RelayServerLocked(R());
        }
        if (!server.Valid() || size > 1400) return false;
        std::vector<uint8_t> packet{kRelayForward};
        PutU24(packet, to.ip & 0xFFFFFFu);
        packet.insert(packet.end(), static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
        return SendRaw(server, packet.data(), packet.size());
    }
    return SendRaw(to, data, size);
}

bool UdpSocket::SendRaw(const Address& to, const void* data, size_t size) {
    const sockaddr_in target = ToSockaddr(to);
    const auto sent = ::sendto(Native(m_socket), static_cast<const char*>(data), static_cast<int>(size), 0,
                               reinterpret_cast<const sockaddr*>(&target), sizeof(target));
    if (!m_bound && sent >= 0) {
        // The first send bound the socket to a port of the system's choosing.
        sockaddr_in bound{};
        socklen_t length = sizeof(bound);
        getsockname(Native(m_socket), reinterpret_cast<sockaddr*>(&bound), &length);
        m_port = ntohs(bound.sin_port);
        m_bound = true;
    }
    return sent == static_cast<decltype(sent)>(size);
}

int UdpSocket::ReceiveFrom(Address& from, void* buffer, size_t capacity, int timeoutMs) {
    if (!IsOpen()) {
        return -1;
    }
    RelayTick();
    if (!m_bound) {
        // Nothing can arrive before the first send gives the socket a port.
        if (timeoutMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        }
        return 0;
    }
    if (timeoutMs > 0) {
#ifdef _WIN32
        WSAPOLLFD pfd{};
        pfd.fd = Native(m_socket);
        pfd.events = POLLRDNORM;
        if (WSAPoll(&pfd, 1, timeoutMs) <= 0) {
            return 0;
        }
#else
        pollfd pfd{};
        pfd.fd = Native(m_socket);
        pfd.events = POLLIN;
        if (::poll(&pfd, 1, timeoutMs) <= 0) {
            return 0;
        }
#endif
    }
    sockaddr_in source{};
    socklen_t length = sizeof(source);
    const auto received = ::recvfrom(Native(m_socket), static_cast<char*>(buffer), static_cast<int>(capacity), 0,
                                     reinterpret_cast<sockaddr*>(&source), &length);
    if (received < 0) {
#ifdef _WIN32
        const int error = WSAGetLastError();
        return error == WSAEWOULDBLOCK || error == WSAECONNRESET || error == WSAEMSGSIZE ? 0 : -1;
#else
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
#endif
    }
    from.ip = ntohl(source.sin_addr.s_addr);
    from.port = ntohs(source.sin_port);
    // From the online server: another game's packet (handed on as from its relay address), or the server's own.
    RelayState& r = R();
    Address server;
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        server = r.server;
    }
    auto* bytes = static_cast<uint8_t*>(buffer);
    if (!server.Valid() || from != server || received < 1) {
        return static_cast<int>(received);
    }
    if (bytes[0] == kRelayForward && received >= 4) {
        from = Address{(240u << 24) | GetU24(bytes + 1), 1};
        std::memmove(bytes, bytes + 4, static_cast<size_t>(received - 4));
        return static_cast<int>(received - 4);
    }
    std::lock_guard<std::mutex> lock(r.mutex);
    if (bytes[0] == kRelayRegistered) {
        if (NowMs() - r.lastRegistered > 6000) RT_LOGF(RT_TAG_RUNTIME, "online: connected to %s\n", r.serverText.c_str());
        r.lastRegistered = NowMs();
    } else if (bytes[0] == kRelayListReply && received >= 2) {
        const size_t size = static_cast<size_t>(received);
        std::vector<Relay::ListedRoom> rooms;
        size_t pos = 2;
        for (uint8_t i = 0; i < bytes[1]; ++i) {
            Relay::ListedRoom room;
            if (pos + 5 > size) break;
            room.host = Address{(240u << 24) | GetU24(bytes + pos), 1};
            room.maxPlayers = bytes[pos + 3];
            room.players = bytes[pos + 4];
            pos += 5;
            if (!GetStr8(bytes, size, pos, room.name) || !GetStr8(bytes, size, pos, room.build)) break;
            if ((room.host.ip & 0xFFFFFFu) != r.id) rooms.push_back(std::move(room));
        }
        r.listed = std::move(rooms);
    }
    return 0;
}

std::vector<LocalInterface> LocalInterfaces() {
    std::vector<LocalInterface> result;
#ifdef _WIN32
    if (!EnsureWinsock()) {
        return result;
    }
    ULONG size = 16 * 1024;
    std::vector<unsigned char> storage;
    for (int attempt = 0; attempt < 3; ++attempt) {
        storage.resize(size);
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
        const ULONG status = GetAdaptersAddresses(
            AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, adapters,
            &size);
        if (status == ERROR_BUFFER_OVERFLOW) {
            continue;
        }
        if (status != NO_ERROR) {
            return result;
        }
        for (auto* adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }
            for (auto* unicast = adapter->FirstUnicastAddress; unicast != nullptr; unicast = unicast->Next) {
                const auto* in = reinterpret_cast<const sockaddr_in*>(unicast->Address.lpSockaddr);
                if (in == nullptr || in->sin_family != AF_INET) {
                    continue;
                }
                LocalInterface entry;
                entry.ip = ntohl(in->sin_addr.s_addr);
                const uint8_t prefix = unicast->OnLinkPrefixLength;
                if (prefix > 0 && prefix < 31 && !IsTailscaleIp(entry.ip)) {
                    const uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
                    entry.broadcast = (entry.ip & mask) | ~mask;
                }
                result.push_back(entry);
            }
        }
        break;
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) {
        return result;
    }
    for (ifaddrs* item = list; item != nullptr; item = item->ifa_next) {
        if (item->ifa_addr == nullptr || item->ifa_addr->sa_family != AF_INET || (item->ifa_flags & IFF_UP) == 0 ||
            (item->ifa_flags & IFF_LOOPBACK) != 0) {
            continue;
        }
        LocalInterface entry;
        entry.ip = ntohl(reinterpret_cast<const sockaddr_in*>(item->ifa_addr)->sin_addr.s_addr);
        if ((item->ifa_flags & IFF_BROADCAST) != 0 && item->ifa_broadaddr != nullptr && !IsTailscaleIp(entry.ip)) {
            entry.broadcast = ntohl(reinterpret_cast<const sockaddr_in*>(item->ifa_broadaddr)->sin_addr.s_addr);
        }
        result.push_back(entry);
    }
    freeifaddrs(list);
#endif
    return result;
}

std::string DeviceName() {
    {
        std::lock_guard<std::mutex> lock(g_nameMutex);
        if (!g_deviceName.empty()) {
            return g_deviceName;
        }
    }
#ifdef _WIN32
    char name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD length = sizeof(name);
    if (GetComputerNameA(name, &length) && length > 0) {
        return std::string(name, length);
    }
#else
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) == 0 && name[0] != '\0' && std::strcmp(name, "localhost") != 0) {
        return name;
    }
#endif
    return "Player";
}

void SetDeviceName(std::string name) {
    std::lock_guard<std::mutex> lock(g_nameMutex);
    g_deviceName = std::move(name);
}

} // namespace Netplay
