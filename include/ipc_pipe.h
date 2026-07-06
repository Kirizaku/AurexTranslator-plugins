#ifndef IPC_PIPE_H
#define IPC_PIPE_H

#include <cstring>
#include <string>
#include <cstdint>
#include <optional>
#include <utility>
#include <mutex>
#include <atomic>
#include <cerrno>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>
#endif

enum class MsgType : uint8_t {
    Status = 0,
    Text   = 1,
    Info   = 2
};

enum class StatusCode : uint8_t {
    Success = 0,
    Failure = 1,
};

#pragma pack(push, 1)
struct PipeHeader {
    uint8_t  msg_type;
    uint8_t  status_code;
    uint8_t  variant;
    uint8_t  _pad;
    uint32_t addr;
    uint32_t text_len;
};
#pragma pack(pop)

class IpcPipe {
public:
    struct Message {
        MsgType     type;
        StatusCode  status_code;
        uint8_t     variant;
        uint32_t    addr;
        std::string text;
    };

private:
    bool              m_is_server;
    std::atomic<bool> m_valid{false};
    std::mutex        m_send_mutex;

#ifdef _WIN32
    HANDLE      m_handle = INVALID_HANDLE_VALUE;
    std::string m_path;
#else
    int         m_fd        = -1;
    int         m_server_fd = -1;
    std::string m_socket_path;
#endif

    bool write_all(const void* buf, size_t len) {
        const char* p = static_cast<const char*>(buf);
#ifdef _WIN32
        DWORD rem = static_cast<DWORD>(len);
        while (rem > 0) {
            DWORD n = 0;
            if (!WriteFile(m_handle, p, rem, &n, NULL) || n == 0) return false;
            p += n; rem -= n;
        }
#else
        size_t rem = len;
        while (rem > 0) {
            ssize_t n = ::send(m_fd, p, rem, MSG_NOSIGNAL);
            if (n <= 0) return false;
            p += n; rem -= static_cast<size_t>(n);
        }
#endif
        return true;
    }

    bool read_all(void* buf, size_t len) {
        char* p = static_cast<char*>(buf);
#ifdef _WIN32
        DWORD rem = static_cast<DWORD>(len);
        while (rem > 0) {
            DWORD n = 0;
            if (!ReadFile(m_handle, p, rem, &n, NULL) || n == 0) return false;
            p += n; rem -= n;
        }
#else
        size_t rem = len;
        while (rem > 0) {
            ssize_t n = ::read(m_fd, p, rem);
            if (n <= 0) return false;
            p += n; rem -= static_cast<size_t>(n);
        }
#endif
        return true;
    }

public:
    IpcPipe(const std::string& name, bool is_server)
        : m_is_server(is_server)
    {
#ifdef _WIN32
        m_path = "\\\\.\\pipe\\" + name;
        const std::string& path = m_path;
        if (is_server) {
            m_handle = CreateNamedPipeA(
                path.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1, 4096, 4096, 0, NULL);
            m_valid = (m_handle != INVALID_HANDLE_VALUE);
        } else {
            for (int i = 0; i < 10 && !m_valid; ++i) {
                m_handle = CreateFileA(path.c_str(), GENERIC_WRITE,
                                       0, NULL, OPEN_EXISTING, 0, NULL);
                if (m_handle != INVALID_HANDLE_VALUE) {
                    m_valid = true;
                } else {
                    Sleep(10);
                }
            }
        }
#else
        m_socket_path = "/tmp/" + name + ".sock";
        if (is_server) {
            m_server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (m_server_fd == -1) return;
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);
            ::unlink(m_socket_path.c_str());
            if (::bind(m_server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) return;
            if (::listen(m_server_fd, 1) == -1) return;
            m_valid = true;
        } else {
            m_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (m_fd == -1) return;
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, m_socket_path.c_str(), sizeof(addr.sun_path) - 1);
            for (int i = 0; i < 10 && !m_valid; ++i) {
                if (::connect(m_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                    m_valid = true;
                } else {
                    usleep(10000);
                }
            }
        }
#endif
    }

    bool waitForConnection() {
        if (!m_is_server || !m_valid) return false;
#ifdef _WIN32
        return ConnectNamedPipe(m_handle, NULL) ||
               GetLastError() == ERROR_PIPE_CONNECTED;
#else
        while (m_valid) {
            struct pollfd pfd{};
            pfd.fd = m_server_fd;
            pfd.events = POLLIN;
            int r = ::poll(&pfd, 1, 100);
            if (r < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (r == 0) continue;
            if (pfd.revents & POLLIN) {
                m_fd = ::accept(m_server_fd, NULL, NULL);
                return m_fd != -1;
            }
            return false;
        }
        return false;
#endif
    }

    bool valid() const { return m_valid; }

    bool send(MsgType type, const std::string& msg,
              StatusCode code = StatusCode::Success,
              uint8_t variant = 0, uint32_t addr = 0)
    {
#ifdef _WIN32
        if (!m_valid || m_handle == INVALID_HANDLE_VALUE) return false;
#else
        if (!m_valid || m_fd == -1) return false;
#endif
        PipeHeader hdr{};
        hdr.msg_type    = static_cast<uint8_t>(type);
        hdr.status_code = static_cast<uint8_t>(code);
        hdr.variant     = variant;
        hdr.addr        = addr;
        hdr.text_len    = static_cast<uint32_t>(msg.size());
        std::lock_guard<std::mutex> lk(m_send_mutex);
        return write_all(&hdr, sizeof(hdr)) && write_all(msg.data(), msg.size());
    }

    std::optional<Message> receive() {
#ifdef _WIN32
        if (m_handle == INVALID_HANDLE_VALUE) return std::nullopt;
#else
        if (m_fd == -1) return std::nullopt;
#endif
        PipeHeader hdr{};
        if (!read_all(&hdr, sizeof(hdr))) return std::nullopt;
        Message msg;
        msg.type        = static_cast<MsgType>(hdr.msg_type);
        msg.status_code = static_cast<StatusCode>(hdr.status_code);
        msg.variant     = hdr.variant;
        msg.addr        = hdr.addr;
        if (hdr.text_len > 0) {
            msg.text.resize(hdr.text_len);
            if (!read_all(msg.text.data(), hdr.text_len)) return std::nullopt;
        }
        return msg;
    }

    void close() {
        m_valid = false;
#ifdef _WIN32
        HANDLE h = std::exchange(m_handle, INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) {
            CancelIoEx(h, NULL);
            CloseHandle(h);
        }
#else
        int fd = std::exchange(m_fd, -1);
        if (fd != -1) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        int sfd = std::exchange(m_server_fd, -1);
        if (sfd != -1) ::close(sfd);
#endif
    }

    void unlink() {
#ifndef _WIN32
        if (!m_socket_path.empty())
            ::unlink(m_socket_path.c_str());
#endif
    }

    ~IpcPipe() { close(); }
};

#endif // IPC_PIPE_H
