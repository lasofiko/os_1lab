#define _WIN32_WINNT 0x0601
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <string>
#include <cstdio>
#include <cstdint>
#include <io.h>
#include <fcntl.h>

const DWORD PROCESS_TIMEOUT = 5000;

bool closeHandle(HANDLE& handle)
{
    if (!handle || handle == INVALID_HANDLE_VALUE) return true;
    if (!CloseHandle(handle)) {
        std::cerr << "CloseHandle error " << GetLastError() << '\n';
        return false;
    }
    handle = nullptr;
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
    // Каждому ребёнку передаём только его ввод, вывод и поток ошибок.
    if (initialized && UpdateProcThreadAttribute(startup.lpAttributeList, 0,
        PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr)) {
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

bool writeLine(HANDLE pipe, const std::string& line)
{
    size_t position = 0;
    while (position < line.size()) {
        size_t remaining = line.size() - position;
        DWORD count = static_cast<DWORD>(remaining > MAXDWORD ? MAXDWORD : remaining);
        DWORD written = 0;
        if (!WriteFile(pipe, line.data() + position, count, &written, nullptr)) {
            std::cerr << "WriteFile error " << GetLastError() << '\n';
            return false;
        }
        if (written == 0) {
            std::cerr << "WriteFile: no data written\n";
            return false;
        }
        position += written;
    }
    return true;
}

bool readAnswer(FILE* pipe, std::string& line)
{
    line.clear();
    int symbol;
    while ((symbol = std::fgetc(pipe)) != EOF) {
        if (symbol == '\n') return true;
        line += static_cast<char>(symbol);
    }
    std::cerr << (std::ferror(pipe) ? "Pipe read error\n" : "Child closed pipe before reply\n");
    return false;
}

bool finishChild(PROCESS_INFORMATION& process, bool abort)
{
    if (!process.hProcess) return true;
    DWORD wait = WaitForSingleObject(process.hProcess, abort ? 0 : PROCESS_TIMEOUT);
    bool ok = wait == WAIT_OBJECT_0;
    if (!ok) {
        if (wait == WAIT_FAILED)
            std::cerr << "WaitForSingleObject error " << GetLastError() << '\n';
        if (!TerminateProcess(process.hProcess, 1))
            std::cerr << "TerminateProcess error " << GetLastError() << '\n';
        wait = WaitForSingleObject(process.hProcess, PROCESS_TIMEOUT);
    }
    if (wait == WAIT_OBJECT_0) {
        DWORD code = 1;
        if (!GetExitCodeProcess(process.hProcess, &code)) {
            std::cerr << "GetExitCodeProcess error " << GetLastError() << '\n';
            ok = false;
        } else if (code != 0) {
            std::cerr << "Child exit code " << code << '\n';
            ok = false;
        }
    } else {
        std::cerr << "Child did not stop\n";
        ok = false;
    }
    ok = closeHandle(process.hThread) && ok;
    ok = closeHandle(process.hProcess) && ok;
    return ok;
}

int run()
{
    wchar_t path[32768];
    DWORD length = GetModuleFileNameW(nullptr, path, 32768);
    if (length == 0 || length >= 32768) {
        std::cerr << "Cannot get executable path\n";
        return 1;
    }
    std::wstring directory(path, length);
    directory.resize(directory.find_last_of(L"\\/") + 1);

    HANDLE pipes[6] = {};
    HANDLE errorOutput = nullptr;
    PROCESS_INFORMATION child1 = {}, child2 = {};
    FILE* answer = nullptr;
    bool ok = true;

    // Три канала: parent -> child1 -> child2 -> parent.
    for (int i = 0; i < 6 && ok; i += 2)
        ok = CreatePipe(&pipes[i], &pipes[i + 1], nullptr, 0) != FALSE;
    if (ok) {
        for (int index : { 0, 2, 3, 5 })
            if (!SetHandleInformation(pipes[index], HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
                ok = false;
    }
    if (ok) ok = DuplicateHandle(GetCurrentProcess(), GetStdHandle(STD_ERROR_HANDLE),
        GetCurrentProcess(), &errorOutput, 0, TRUE, DUPLICATE_SAME_ACCESS) != FALSE;
    try {
        if (ok) ok = startChild(directory + L"child1.exe", pipes[0], pipes[3], errorOutput, child1);
        if (ok) ok = startChild(directory + L"child2.exe", pipes[2], pipes[5], errorOutput, child2);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        ok = false;
    }
    if (!ok) std::cerr << "Setup/CreateProcess error " << GetLastError() << '\n';

    for (int index : { 0, 2, 3, 5 }) ok = closeHandle(pipes[index]) && ok;
    ok = closeHandle(errorOutput) && ok;

    if (ok) {
        // FILE читает канал с обычной буферизацией стандартной библиотеки.
        int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(pipes[4]), _O_RDONLY | _O_TEXT);
        if (descriptor == -1) {
            std::perror("_open_osfhandle");
            ok = false;
        } else {
            pipes[4] = nullptr; // Теперь дескриптором владеет стандартная библиотека.
            answer = _fdopen(descriptor, "r");
            if (!answer) {
                std::perror("_fdopen");
                if (_close(descriptor) != 0) std::perror("_close");
                ok = false;
            }
        }
    }

    if (ok) std::cerr << "Enter lines. Type /exit to finish.\n";
    try {
        std::string line;
        std::string result;
        while (ok && std::getline(std::cin, line)) {
            if (line == "/exit") break;
            line += '\n';
            // Дети сначала читают строку целиком, поэтому взаимного ожидания нет.
            if (!writeLine(pipes[1], line) || !readAnswer(answer, result)) {
                ok = false;
                break;
            }
            std::cout << result << std::endl;
            if (!std::cout) {
                std::cerr << "Console/output error\n";
                ok = false;
            }
        }
        if (std::cin.bad() || (std::cin.fail() && !std::cin.eof())) {
            std::cerr << "Input error\n";
            ok = false;
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        ok = false;
    }

    // EOF проходит по цепочке после закрытия записи первого канала.
    ok = closeHandle(pipes[1]) && ok;
    bool firstFinished = finishChild(child1, !ok);
    bool secondFinished = finishChild(child2, !ok || !firstFinished);
    ok = firstFinished && secondFinished && ok;
    if (answer && std::fclose(answer) != 0) {
        std::perror("fclose");
        ok = false;
    }
    for (HANDLE& pipe : pipes) ok = closeHandle(pipe) && ok;
    return ok ? 0 : 1;
}

int main()
{
    // CP1251 позволяет работать с русскими буквами в обычной std::string.
    UINT inputCodePage = GetConsoleCP();
    UINT outputCodePage = GetConsoleOutputCP();
    if ((inputCodePage && !SetConsoleCP(1251)) ||
        (outputCodePage && !SetConsoleOutputCP(1251))) {
        std::cerr << "Cannot set console code page\n";
        if (inputCodePage) SetConsoleCP(inputCodePage);
        if (outputCodePage) SetConsoleOutputCP(outputCodePage);
        return 1;
    }

    int result = 1;
    try { result = run(); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }

    if (inputCodePage && !SetConsoleCP(inputCodePage)) result = 1;
    if (outputCodePage && !SetConsoleOutputCP(outputCodePage)) result = 1;
    return result;
}
