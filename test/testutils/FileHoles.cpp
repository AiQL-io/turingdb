#include "FileHoles.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vector>

using namespace turing::test;

size_t turing::test::getAllocatedBytes(const std::string& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return 0;
    }

    return static_cast<size_t>(info.st_blocks) * 512;
}

bool turing::test::directoryKeepsHoles(const std::string& directory) {
    static constexpr size_t WRITTEN_BYTES = 4096;
    static constexpr size_t FILE_BYTES = 4 * 1024 * 1024;

    const std::string path = directory + "/hole-probe";
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        return false;
    }

    const std::vector<char> data(WRITTEN_BYTES, 'x');
    const bool written = ::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size());
    const bool extended = ::ftruncate(fd, FILE_BYTES) == 0;
    ::close(fd);

    const bool keepsHoles = written && extended && getAllocatedBytes(path) < FILE_BYTES / 2;
    ::unlink(path.c_str());

    return keepsHoles;
}
