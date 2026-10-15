#include <iostream>
#include <string>
#include <cctype>
#include <clocale>

int main()
{
    if (!std::setlocale(LC_CTYPE, "Russian_Russia.1251")) {
        std::cerr << "child1: cannot set Russian locale\n";
        return 1;
    }

    try {
        std::string line;
        while (std::getline(std::cin, line)) {
            // Сначала получаем всю строку, затем отправляем результат.
            for (char& symbol : line)
                symbol = static_cast<char>(std::tolower(static_cast<unsigned char>(symbol)));

            std::cout << line << std::endl;
            if (!std::cout) {
                std::cerr << "child1: output error\n";
                return 1;
            }
        }
        if (!std::cin.eof() || std::cin.bad()) {
            std::cerr << "child1: input error\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "child1: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
