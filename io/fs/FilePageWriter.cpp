#include "FilePageWriter.h"

#include <fcntl.h>
#include <unistd.h>

#include "BioAssert.h"

using namespace fs;

namespace {

constexpr size_t DIRECT_IO_BLOCK_SIZE = 4096;

}

Result<FilePageWriter> FilePageWriter::open(const Path& path, size_t pageSize) {
#ifdef __APPLE__
    const int access = O_WRONLY | O_CREAT | O_TRUNC;
#else
    const int access = O_WRONLY | O_CREAT | O_TRUNC | O_DIRECT;
#endif
    const int permissions = S_IRUSR | S_IWUSR;

    const int fd = ::open(path.c_str(), access, permissions);

    if (fd < 0) {
        return Error::result(ErrorType::OPEN_FILE, errno);
    }

#ifdef __APPLE__
    // Disable caching on macOS (equivalent to O_DIRECT)
    fcntl(fd, F_NOCACHE, 1);
#endif

    return FilePageWriter {fd, pageSize};
}

Result<FilePageWriter> FilePageWriter::openNoDirect(const Path& path, size_t pageSize) {
    const int access = O_WRONLY | O_TRUNC | O_CREAT;
    const int permissions = S_IRUSR | S_IWUSR;

    const int fd = ::open(path.c_str(), access, permissions);

    if (fd < 0) {
        return Error::result(ErrorType::OPEN_FILE, errno);
    }

    return FilePageWriter {fd, pageSize};
}

void FilePageWriter::write(const uint8_t* data, size_t size) {
    size_t toWrite = size;
    const uint8_t* curr = data;

    while (toWrite != 0) {
        if (_buffer.avail() == 0) {
            flush();
        }

        const size_t currentSize = std::min(_buffer.avail(), toWrite);
        _buffer.write(curr, currentSize);
        toWrite -= currentSize;
        curr += currentSize;
        _written += currentSize;
    }
}

void FilePageWriter::writeToCurrentPage(std::span<const uint8_t> data) {
    bioassert(_buffer.avail() >= data.size(), "buffer has not enough available space");

    const size_t size = data.size();

    _buffer.write(data.data(), size);
    _written += size;
}

void FilePageWriter::sync() {
    if (auto res = ::fsync(_fd); res != 0) {
        _error = Error(ErrorType::SYNC_FILE, errno);
    }
}

void FilePageWriter::finish() {
    if (_fd < 0) {
        return;
    }

    if (_buffer.size() > 0) {
        flush();
    }

    if (_fileEnd > 0) {
        if (::ftruncate(_fd, static_cast<off_t>(_fileEnd)) != 0) {
            _error = Error(ErrorType::WRITE_FILE, errno);
        }
        sync();
    }

    ::close(_fd);
    _fd = -1;
}

void FilePageWriter::nextPage() {
    flush();
}

void FilePageWriter::seek(size_t offset) {
    flush();

    if (int res = ::lseek(_fd, offset, SEEK_SET); res < 0) {
        _error = Error(ErrorType::COULD_NOT_SEEK, errno);
    }

    _offset = offset;
    _written = 0;
}

void FilePageWriter::reserveSpace(size_t byteCount) {
    _buffer.reserveSpace(byteCount);
}

void FilePageWriter::flush() {
    const size_t usedBytes = _buffer.size();
    const size_t pageBytes = _buffer.capacity();
    const size_t alignedUsedBytes = (usedBytes + DIRECT_IO_BLOCK_SIZE - 1) / DIRECT_IO_BLOCK_SIZE * DIRECT_IO_BLOCK_SIZE;
    const size_t bytesToWrite = std::min(alignedUsedBytes, pageBytes);

    std::memset(_buffer.data() + usedBytes, 0, bytesToWrite - usedBytes);

    ssize_t remainingBytes = static_cast<ssize_t>(bytesToWrite);
    ssize_t bytesWritten = 0;

    while (remainingBytes > 0) {
        const ssize_t nbytes = ::write(_fd, _buffer.data() + bytesWritten, remainingBytes);

        if (nbytes == -1) {
            if (errno == EAGAIN) {
                continue;
            }

            _error = Error(ErrorType::WRITE_PAGE, errno);
            _buffer.resize(0);
            return;
        }

        bytesWritten += nbytes;
        remainingBytes -= nbytes;
    }

    // The zero tail of the page is left unwritten, so the filesystem keeps it as a hole
    if (bytesToWrite < pageBytes) {
        if (::lseek(_fd, static_cast<off_t>(pageBytes - bytesToWrite), SEEK_CUR) < 0) {
            _error = Error(ErrorType::COULD_NOT_SEEK, errno);
        }
    }

    _offset += pageBytes;
    _fileEnd = std::max(_fileEnd, _offset);
    _buffer.resize(0);
}
