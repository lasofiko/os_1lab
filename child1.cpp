#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#pragma comment(lib, "User32.lib")

bool writeAll(HANDLE output, const void* data, DWORD size)
{
    const char* bytes = static_cast<const char*>(data);
    while (size > 0) {
        DWORD written = 0;
        if (!WriteFile(output, bytes, size, &written, nullptr)) {
            std::fprintf(stderr, "child1: WriteFile error %lu\n", GetLastError());
            return false;
        }
        if (written == 0) return false;
        bytes += written;
        size -= written;
    }
    return true;
}

int main()
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!input || input == INVALID_HANDLE_VALUE ||
        !output || output == INVALID_HANDLE_VALUE) return 1;

    wchar_t buffer[2048];
    DWORD pending = 0;
    for (;;) {
        DWORD count = 0;
        char* bytes = reinterpret_cast<char*>(buffer);
        if (!ReadFile(input, bytes + pending, sizeof(buffer) - pending,
                      &count, nullptr)) {
            DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) break;
            std::fprintf(stderr, "child1: ReadFile error %lu\n", error);
            return 1;
        }
        if (count == 0) break;

        DWORD total = pending + count;
        DWORD complete = total - total % sizeof(wchar_t);
        char lastByte = bytes[total - 1];

        if (CharLowerBuffW(buffer, complete / sizeof(wchar_t)) == 0 && complete != 0) {
            std::fprintf(stderr, "child1: CharLowerBuffW failed\n");
            return 1;
        }
        if (!writeAll(output, buffer, complete)) return 1;
        pending = total - complete;
        if (pending) bytes[0] = lastByte;
    }

    if (pending) {
        std::fprintf(stderr, "child1: incomplete UTF-16 character\n");
        return 1;
    }

    bool inputClosed = CloseHandle(input) != FALSE;
    bool outputClosed = CloseHandle(output) != FALSE;
    return inputClosed && outputClosed ? 0 : 1;
}
