#include <iostream>
#include <string>

int main()
{
    try {
        std::string line;
        while (std::getline(std::cin, line)) {
            size_t length = 0;
            bool previousSpace = false;

            // Сжимаем пробелы внутри уже прочитанной строки.
            for (char symbol : line) {
                if (symbol != ' ' || !previousSpace)
                    line[length++] = symbol;
                previousSpace = symbol == ' ';
            }
            line.resize(length);

            std::cout << line << std::endl;
            if (!std::cout) {
                std::cerr << "child2: output error\n";
                return 1;
            }
        }
        if (!std::cin.eof() || std::cin.bad()) {
            std::cerr << "child2: input error\n";
            return 1;
        }
    } catch (const std::exception& error) {
        std::cerr << "child2: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
