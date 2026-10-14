#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <iostream>
#include <string>
#include <new>
#include <climits>

const DWORD PROCESS_TIMEOUT = 5000;
const DWORD LINE_TIMEOUT = 120000;

bool closeHandle(HANDLE& handle)
{
    if (!handle || handle == INVALID_HANDLE_VALUE) return true;
    bool ok = CloseHandle(handle) != FALSE;
    if (!ok) std::fprintf(stderr, "CloseHandle error %lu\n", GetLastError());
    handle = nullptr;
    return ok;
}

bool writeAll(HANDLE output, const void* data, size_t size)
{
    const char* bytes = static_cast<const char*>(data);
    while (size > 0) {
        DWORD part = static_cast<DWORD>(size > 4096 ? 4096 : size);
        DWORD written = 0;
        if (!WriteFile(output, bytes, part, &written, nullptr)) return false;
        if (written == 0) { SetLastError(ERROR_WRITE_FAULT); return false; }
        bytes += written;
        size -= written;
    }
    return true;
}

bool startChild(const std::wstring& path, HANDLE input, HANDLE output,
                HANDLE errorOutput, PROCESS_INFORMATION& process)
{
    std::wstring command = L"\"" + path + L"\"";
    STARTUPINFOEXW startup = {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = output;
    startup.StartupInfo.hStdError = errorOutput;
    HANDLE inherited[] = { input, output, errorOutput };

    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return false;
    startup.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, size));
    if (!startup.lpAttributeList) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    bool initialized = InitializeProcThreadAttributeList(
        startup.lpAttributeList, 1, 0, &size) != FALSE;
    bool ok = false;
    if (initialized && UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
        // Передаём только stdin, stdout и stderr конкретного ребёнка.
        ok = CreateProcessW(path.c_str(), &command[0], nullptr, nullptr, TRUE,
            EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
            &startup.StartupInfo, &process) != FALSE;
    }
    DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    if (initialized) DeleteProcThreadAttributeList(startup.lpAttributeList);
    HeapFree(GetProcessHeap(), 0, startup.lpAttributeList);
    SetLastError(error);
    return ok;
}

// Результат: 1 — строка, 0 — конец ввода, -1 — ошибка.
int readLine(HANDLE input, bool console, std::wstring& line)
{
    line.clear();
    if (console) {
        wchar_t buffer[256];
        for (;;) {
            DWORD count = 0;
            if (!ReadConsoleW(input, buffer, 256, &count, nullptr)) return -1;
            if (count == 0) return line.empty() ? 0 : 1;
            line.append(buffer, count);
            if (line.back() == L'\n') break;
        }
        line.pop_back();
        if (!line.empty() && line.back() == L'\r') line.pop_back();
    } else {
        std::string utf8;
        if (!std::getline(std::cin, utf8)) {
            if (std::cin.bad()) { SetLastError(ERROR_READ_FAULT); return -1; }
            return 0;
        }
        if (!utf8.empty() && utf8.back() == '\r') utf8.pop_back();
        if (utf8.size() > INT_MAX) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return -1; }
        if (!utf8.empty()) {
            int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
            if (length == 0) return -1;
            line.resize(length);
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                utf8.data(), static_cast<int>(utf8.size()), &line[0], length)) return -1;
        }
    }
    return line == L"/exit" ? 0 : 1;
}

bool printLine(HANDLE output, bool console, const std::wstring& line)
{
    if (console) {
        size_t position = 0;
        while (position < line.size()) {
            DWORD count = static_cast<DWORD>((line.size() - position > 4096)
                ? 4096 : line.size() - position);
            DWORD written = 0;
            if (!WriteConsoleW(output, line.data() + position, count, &written, nullptr))
                return false;
            if (written == 0) { SetLastError(ERROR_WRITE_FAULT); return false; }
            position += written;
        }
        return true;
    }
    if (line.size() > INT_MAX) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(),
        static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0) return false;
    std::string utf8(size, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(),
        static_cast<int>(line.size()), &utf8[0], size, nullptr, nullptr)) return false;
    return writeAll(output, utf8.data(), utf8.size());
}

struct Exchange {
    HANDLE pipe;
    std::wstring text;
    DWORD error;
};

DWORD WINAPI sendLine(void* parameter)
{
    Exchange& exchange = *static_cast<Exchange*>(parameter);
    if (!writeAll(exchange.pipe, exchange.text.data(), exchange.text.size() * sizeof(wchar_t)))
        exchange.error = GetLastError();
    return 0;
}

DWORD WINAPI receiveLine(void* parameter)
{
    Exchange& exchange = *static_cast<Exchange*>(parameter);
    wchar_t buffer[2048];
    DWORD pending = 0;
    try {
        for (;;) {
            DWORD count = 0;
            char* bytes = reinterpret_cast<char*>(buffer);
            if (!ReadFile(exchange.pipe, bytes + pending, sizeof(buffer) - pending,
                          &count, nullptr)) {
                exchange.error = GetLastError();
                return 1;
            }
            if (count == 0) { exchange.error = ERROR_BROKEN_PIPE; return 1; }
            DWORD total = count + pending;
            DWORD symbols = total / sizeof(wchar_t);
            exchange.text.append(buffer, symbols);
            pending = total % sizeof(wchar_t);
            if (pending) bytes[0] = bytes[total - 1];
            if (!exchange.text.empty() && exchange.text.back() == L'\n') {
                if (pending) { exchange.error = ERROR_INVALID_DATA; return 1; }
                return 0;
            }
        }
    } catch (const std::exception&) {
        exchange.error = ERROR_NOT_ENOUGH_MEMORY;
        return 1;
    }
}

bool finishChild(PROCESS_INFORMATION& process, bool abort)
{
    if (!process.hProcess) return true;
    DWORD wait = WaitForSingleObject(process.hProcess, abort ? 0 : PROCESS_TIMEOUT);
    bool ok = wait == WAIT_OBJECT_0;
    if (!ok) {
        std::fprintf(stderr, "Stopping child (wait result %lu).\n", wait);
        if (!TerminateProcess(process.hProcess, 1))
            std::fprintf(stderr, "TerminateProcess error %lu\n", GetLastError());
        if (WaitForSingleObject(process.hProcess, PROCESS_TIMEOUT) != WAIT_OBJECT_0) {
            std::fprintf(stderr, "Cannot stop child.\n");
            return false;
        }
    }
    DWORD code = 1;
    if (!GetExitCodeProcess(process.hProcess, &code)) ok = false;
    else if (code != 0) { std::fprintf(stderr, "Child exit code %lu\n", code); ok = false; }
    ok = closeHandle(process.hThread) && ok;
    ok = closeHandle(process.hProcess) && ok;
    return ok;
}

int run()
{
    HANDLE pipes[6] = {};
    HANDLE errorOutput = nullptr;
    PROCESS_INFORMATION child1 = {}, child2 = {};
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    bool consoleInput = GetConsoleMode(input, &mode) != FALSE;
    bool consoleOutput = GetConsoleMode(output, &mode) != FALSE;
    bool ok = true;

    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (length == 0 || length >= 32768) return 1;
    std::wstring directory(path, length);
    directory.resize(directory.find_last_of(L"\\/") + 1);

    // Все каналы изначально ненаследуемые.
    for (int i = 0; i < 6 && ok; i += 2)
        ok = CreatePipe(&pipes[i], &pipes[i + 1], nullptr, 0) != FALSE;
    if (ok) {
        int childEnds[] = { 0, 2, 3, 5 };
        for (int index : childEnds)
            if (!SetHandleInformation(pipes[index], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                ok = false;
    }
    if (ok) ok = DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE),
        GetCurrentProcess(), &errorOutput, 0, TRUE, DUPLICATE_SAME_ACCESS) != FALSE;
    try {
        if (ok) ok = startChild(directory + L"child1.exe", pipes[0], pipes[3], errorOutput, child1);
        if (ok) ok = startChild(directory + L"child2.exe", pipes[2], pipes[5], errorOutput, child2);
    } catch (const std::exception&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        ok = false;
    }
    if (!ok) std::fprintf(stderr, "Setup/CreateProcess error %lu\n", GetLastError());

    // У родителя остаются только запись в первый канал и чтение последнего.
    for (int index : { 0, 2, 3, 5 }) ok = closeHandle(pipes[index]) && ok;
    ok = closeHandle(errorOutput) && ok;
    if (ok && consoleInput)
        std::fprintf(stderr, "Enter lines. Type /exit on a separate line to finish.\n");

    try {
        std::wstring line;
        while (ok) {
            // Пока пользователь вводит строку, рабочие потоки ещё не запущены.
            int status = readLine(input, consoleInput, line);
            if (status == 0) break;
            if (status < 0) { ok = false; break; }
            line += L'\n';
            Exchange sender = { pipes[1], line, ERROR_SUCCESS };
            Exchange receiver = { pipes[4], L"", ERROR_SUCCESS };
            HANDLE threads[2] = {};
            threads[0] = CreateThread(nullptr, 0, receiveLine, &receiver, 0, nullptr);
            if (threads[0]) threads[1] = CreateThread(nullptr, 0, sendLine, &sender, 0, nullptr);
            DWORD wait = WAIT_FAILED;
            if (threads[0] && threads[1])
                wait = WaitForMultipleObjects(2, threads, TRUE, LINE_TIMEOUT);
            if (wait != WAIT_OBJECT_0) {
                std::fprintf(stderr, "Exchange failed/timed out (wait %lu, error %lu).\n",
                             wait, GetLastError());
                // Остановка детей разрывает каналы и освобождает заблокированные потоки.
                finishChild(child1, true);
                finishChild(child2, true);
                for (HANDLE thread : threads) {
                    if (thread && WaitForSingleObject(thread, PROCESS_TIMEOUT) != WAIT_OBJECT_0) {
                        std::fprintf(stderr, "Cannot stop I/O thread; exiting parent.\n");
                        ExitProcess(1);
                    }
                }
                ok = false;
            } else if (sender.error != ERROR_SUCCESS || receiver.error != ERROR_SUCCESS) {
                std::fprintf(stderr, "Pipe errors: send %lu, receive %lu\n",
                             sender.error, receiver.error);
                ok = false;
            }
            for (HANDLE& thread : threads) ok = closeHandle(thread) && ok;
            if (ok) ok = printLine(output, consoleOutput, receiver.text);
        }
    } catch (const std::exception&) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        ok = false;
    }
    if (!ok) std::fprintf(stderr, "parent: operation failed (error %lu).\n", GetLastError());
    // Закрытие записи сообщает child1 об EOF; он передаётся дальше по цепочке.
    ok = closeHandle(pipes[1]) && ok;
    bool firstFinished = finishChild(child1, !ok);
    bool secondFinished = finishChild(child2, !ok || !firstFinished);
    ok = firstFinished && secondFinished && ok;
    for (HANDLE& pipe : pipes) ok = closeHandle(pipe) && ok;
    return ok ? 0 : 1;
}

int main()
{
    try { return run(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "parent: %s\n", error.what());
        // Последняя защита при исключении: ОС освобождает дескрипторы процесса.
        return 1;
    }
}
