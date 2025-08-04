#include <iostream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctime>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <memory>

namespace fs = std::filesystem;

std::string temp_container;
std::string temp_dir;

void cleanup() {
    if (!temp_container.empty()) {
        std::string cmd = "podman rm -f " + temp_container + " 2>/dev/null";
        std::system(cmd.c_str());
    }
    if (!temp_dir.empty()) {
        std::string cmd = "rm -rf " + temp_dir + " 2>/dev/null";
        std::system(cmd.c_str());
    }
}

bool check_dependency(const std::string& cmd_name) {
    std::string cmd = "command -v " + cmd_name + " > /dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

std::string sanitize_layer_name(const std::string& input) {
    // Удаляем путь до последнего слеша
    size_t pos = input.find_last_of('/');
    std::string base = (pos != std::string::npos) ? input.substr(pos + 1) : input;

    // Копируем и очищаем имя
    std::string output;
    for (char c : base) {
        if (c == ':' || c == '@') {
            output += '_';
        } else if (std::isalnum(c) || c == '_' || c == '-') {
            output += c;
        } else {
            output += '_';
        }
    }

    // Удаляем начальные и конечные подчеркивания
    size_t start = output.find_first_not_of('_');
    if (start == std::string::npos) {
        return "layer_" + std::to_string(std::time(nullptr));
    }
    
    size_t end = output.find_last_not_of('_');
    return output.substr(start, end - start + 1);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <image_name>\n"
                  << "Example: " << argv[0] << " quay.io/fedora:42\n";
        return 1;
    }

    std::string image_name = argv[1];

    // Генерация временных имен
    temp_container = "dbrl_temp_container_" + std::to_string(std::time(nullptr));
    
    // Создание временной директории
    char template_path[] = "/tmp/dbrl_import_XXXXXX";
    char* temp_dir_c = mkdtemp(template_path);
    if (!temp_dir_c) {
        std::cerr << "ERROR: Failed to create temp directory: " << strerror(errno) << "\n";
        return 1;
    }
    temp_dir = temp_dir_c;
    std::atexit(cleanup);

    // Проверка зависимостей
    if (!check_dependency("podman")) {
        std::cerr << "ERROR: podman not found\n";
        return 1;
    }
    if (!check_dependency("brl")) {
        std::cerr << "ERROR: brl not found\n";
        return 1;
    }

    // Pull образа
    std::cout << "Downloading image " << image_name << "\n";
    std::string cmd = "podman pull " + image_name;
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "ERROR: Failed to pull image\n";
        return 1;
    }

    // Создание контейнера
    std::cout << "Creating temporary container\n";
    cmd = "podman create --name " + temp_container + " " + image_name;
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "ERROR: Failed to create container\n";
        return 1;
    }

    // Подготовка путей
    fs::path temp_dir_path(temp_dir);
    fs::path tar_file = temp_dir_path / "export.tar";
    fs::path layer_dir = temp_dir_path / "layer_root";
    fs::path layer_tar = temp_dir_path / "layer.tar";
    fs::path bedrock_dir = layer_dir / "bedrock";

    // Создание директорий
    if (!fs::create_directory(layer_dir)) {
        std::cerr << "ERROR: Failed to create layer directory\n";
        return 1;
    }

    // Экспорт контейнера
    std::cout << "Exporting container filesystem\n";
    cmd = "podman export " + temp_container + " > " + tar_file.string();
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "ERROR: Export failed\n";
        return 1;
    }

    // Распаковка файловой системы
    std::cout << "Preparing Bedrock Linux layer\n";
    cmd = "tar -xf " + tar_file.string() + " -C " + layer_dir.string();
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "ERROR: Failed to unpack filesystem\n";
        return 1;
    }

    // Генерация имени слоя
    std::string layer_name = sanitize_layer_name(image_name);
    std::cout << "Using layer name: " << layer_name << "\n";

    // Создание метаданных Bedrock Linux
    if (!fs::create_directory(bedrock_dir)) {
        std::cerr << "ERROR: Failed to create bedrock directory\n";
        return 1;
    }

    // Запись метаданных
    std::ofstream layer_file(bedrock_dir / "layer");
    if (!layer_file || !(layer_file << layer_name << std::endl)) {
        std::cerr << "ERROR: Failed to write layer name\n";
        return 1;
    }

    std::ofstream version_file(bedrock_dir / "version");
    if (!version_file || !(version_file << "unknown" << std::endl)) {
        std::cerr << "ERROR: Failed to write version\n";
        return 1;
    }

    // Создание tar-архива слоя
    cmd = "tar -C " + layer_dir.string() + " -cf " + layer_tar.string() + " .";
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "ERROR: Failed to create layer tarball\n";
        return 1;
    }

    // Импорт в Bedrock Linux
    std::cout << "Importing into Bedrock Linux\n";
    cmd = "sudo brl import " + layer_name + " " + layer_tar.string();
    if (std::system(cmd.c_str()) != 0) {
        std::cerr << "ERROR: brl import failed\n";
        return 1;
    }

    std::cout << "\nSUCCESS: Image imported as layer '" << layer_name << "'\n";
    std::cout << "You can now use it with:\n";
    std::cout << "  brl run -l " << layer_name << " <command>\n\n";
    std::cout << "Example:\n";
    std::cout << "  brl run -l " << layer_name << " bash\n";
    
    return 0;
}
