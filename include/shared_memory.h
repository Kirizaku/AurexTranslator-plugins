#ifndef SHARED_MEMORY_H
#define SHARED_MEMORY_H

#include <cstring>
#include <string>
#include <cstdint>
#include <optional>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

enum class MsgType : uint8_t {
    Status  = 0,
    Text    = 1,
    Info    = 2,
};

enum class StatusCode : uint8_t {
    Success = 0,
    Failure = 1,
};

struct SharedData {
    int ready;
    char message[1024];
    MsgType msg_type;
    StatusCode status_code;
};

class SharedMemory {
private:
    std::string name;
    size_t size;
    SharedData* data;
    bool is_valid;

#ifdef _WIN32
    HANDLE hMapFile;
#else
    int shm_fd;
#endif

public:
    SharedMemory(const std::string& name, size_t size, bool create)
        : name(name), size(size), data(nullptr), is_valid(false)
    {
#ifdef _WIN32
        hMapFile = NULL;
#else
        shm_fd = -1;
#endif

        if (!init(create)) {
            cleanup();
        }
    }

    bool init(bool create) {
#ifdef _WIN32
        if (create) {
            hMapFile = CreateFileMappingA(
                INVALID_HANDLE_VALUE,
                NULL,
                PAGE_READWRITE,
                0,
                static_cast<DWORD>(size),
                name.c_str()
                );
        } else {
            hMapFile = OpenFileMappingA(
                FILE_MAP_ALL_ACCESS,
                FALSE,
                name.c_str()
                );
        }

        if (hMapFile == NULL) {
            return false;
        }

        data = reinterpret_cast<SharedData*>(MapViewOfFile(
            hMapFile,
            FILE_MAP_ALL_ACCESS,
            0, 0,
            size
            ));

        if (data == NULL) {
            return false;
        }

        if (create) {
            data->ready = 0;
            std::memset(data->message, 0, sizeof(data->message));
        }

#else
        std::string linuxName = "/" + name;

        int flags = O_RDWR;
        if (create) flags |= O_CREAT;

        shm_fd = shm_open(linuxName.c_str(), flags, 0666);
        if (shm_fd == -1) {
            return false;
        }

        if (create) {
            if (ftruncate(shm_fd, size) == -1) {
                return false;
            }
        }

        data = reinterpret_cast<SharedData*>(mmap(
            nullptr,
            size,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            shm_fd,
            0
            ));

        if (data == MAP_FAILED) {
            data = nullptr;
            return false;
        }

        if (create) {
            data->ready = 0;
            std::memset(data->message, 0, sizeof(data->message));
        }
#endif

        is_valid = true;
        return true;
    }

    bool valid() const {
        return is_valid && data != nullptr;
    }

    bool send(MsgType type, const std::string& msg, StatusCode code = StatusCode::Success) {
        if (!valid()) return false;

        data->msg_type = type;
        data->status_code = code;
        std::strncpy(data->message, msg.c_str(), sizeof(data->message) - 1); 
        data->message[sizeof(data->message) - 1] = '\0';
        data->ready = 1;
        return true;
    }

    struct Message {
        MsgType type;
        StatusCode status_code;
        std::string text;
    };

    std::optional<Message> receive() {
        if (!valid()) return std::nullopt;
        if (data->ready != 1) return std::nullopt;

        Message msg;
        msg.type = data->msg_type;
        msg.status_code = data->status_code;
        msg.text = std::string(data->message);

        data->ready = 0;
        return msg;
    }

    void cleanup() {
#ifdef _WIN32
        if (data) {
            UnmapViewOfFile(data);
            data = nullptr;
        }
        if (hMapFile) {
            CloseHandle(hMapFile);
            hMapFile = NULL;
        }
#else
        if (data && data != MAP_FAILED) {
            munmap(data, size);
            data = nullptr;
        }
        if (shm_fd != -1) {
            close(shm_fd);
            shm_fd = -1;
        }
#endif
        is_valid = false;
    }

    ~SharedMemory() {
        cleanup();
    }

#if defined(__linux__)
    void unlink() {
        std::string linuxName = "/" + name;
        shm_unlink(linuxName.c_str());
    }
#endif
};

#endif