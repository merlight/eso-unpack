//==========================================================================
//
//  This is free and unencumbered software released into the public domain.
//
//  Anyone is free to copy, modify, publish, use, compile, sell, or
//  distribute this software, either in source code form or as a compiled
//  binary, for any purpose, commercial or non-commercial, and by any
//  means.
//
//  In jurisdictions that recognize copyright laws, the author or authors
//  of this software dedicate any and all copyright interest in the
//  software to the public domain. We make this dedication for the benefit
//  of the public at large and to the detriment of our heirs and
//  successors. We intend this dedication to be an overt act of
//  relinquishment in perpetuity of all present and future rights to this
//  software under copyright law.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
//  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
//  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
//  IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
//  OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
//  ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
//  OTHER DEALINGS IN THE SOFTWARE.
//
//  For more information, please refer to <http://unlicense.org/>
//
//==========================================================================
#ifndef ESOUNPACK_FILEIO_H
#define ESOUNPACK_FILEIO_H

#include <cstring>
#include <memory>
#include <stdexcept>

#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


class File
{
public:

    File()
      : _fd(-1) {}

    File(const char* path, int flags, int mode = 0)
      : _fd(s_open(path, flags, mode)) {}

    File(const std::string& path, int flags, int mode = 0)
      : _fd(s_open(path.c_str(), flags, mode)) {}

    ~File()
    {
        if (_fd != -1) {
            ::close(_fd);
        }
    }

    int fd() const
    {
        return _fd;
    }

    bool is_open() const
    {
        return _fd != -1;
    }

    void open(const char* path, int flags, int mode = 0)
    {
        close();
        _fd = s_open(path, flags, mode);
    }

    void open(const std::string& path, int flags, int mode = 0)
    {
        close();
        _fd = s_open(path.c_str(), flags, mode);
    }

    void close()
    {
        if (_fd != -1) {
            ::close(_fd);
            _fd = -1;
        }
    }

    ssize_t read(void* buf, size_t count)
    {
        ssize_t n;
        while ((n = ::read(_fd, buf, count)) < 0) {
            if (errno != EINTR) {
                throw std::runtime_error("error reading from file");
            }
        }
        //std::clog << "read " << n << " bytes" << std::endl;
        return n;
    }

    ssize_t write(const void* buf, size_t count)
    {
        ssize_t n;
        while ((n = ::write(_fd, buf, count)) < 0) {
            if (errno != EINTR) {
                throw std::runtime_error("error writing to file");
            }
        }
        //std::clog << "wrote " << n << " bytes" << std::endl;
        return n;
    }

private:

    static int s_open(const char* path, int flags, int mode)
    {
        int fd = ::open(path, flags, mode);
        if (fd == -1) {
            static std::string msg = "failed to open ";
            throw std::runtime_error(msg + path);
        }
        return fd;
    }

    int     _fd;
};


class FileMapping
{
public:

    explicit FileMapping(const char* path, int flags = O_RDONLY, int mode = 0)
    {
        size_t pathLen = std::strlen(path);
        _errorPrefix.reserve(pathLen + 2);
        _errorPrefix.assign(path, pathLen);
        _errorPrefix.append(": ", 2);

        struct stat st;
        int fd = ::open(path, flags, mode);

        check(fd, fd != -1);
        check(fd, ::fstat(fd, &st) != -1);

        _size = st.st_size;
        _data = ::mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
        check(fd, _data != NULL);

        ::close(fd);
    }

    ~FileMapping()
    {
        ::munmap(_data, _size);
    }

    char* data() const
    {
        return reinterpret_cast<char*>(_data);
    }

    void error(const char* err) const
    {
        throw std::runtime_error(_errorPrefix + err);
    }

    off_t size() const
    {
        return _size;
    }

private:

    void check(int fd, bool cond) const
    {
        if (!cond) {
            const char* msg = ::strerror(errno);
            if (fd != -1) {
                ::close(fd);
            }
            throw std::runtime_error(_errorPrefix + msg);
        }
    }

    void*       _data;
    off_t       _size;
    std::string _errorPrefix;
};


template<typename T>
class TBuffer
{
public:

    TBuffer(size_t size, File& file)
      : _buffer(new T[size])
      , _size(size)
      , _end(0)
      , _pos(0)
      , _consumed(0)
      , _file(file)
    {}

    size_t consume(size_t count)
    {
        _consumed += count;
        _pos += count;
        return count;
    }

    size_t next(size_t count, size_t* p_count, T** p_data)
    {
        size_t avail = _end - _pos;

        if (avail <= _size / 16) {
            avail = read_more();
        }

        *p_data = _buffer.get() + _pos;
        *p_count = (avail < count ? avail : count);
        return *p_count;
    }

    size_t offset(size_t pos = 0) const
    {
        return _consumed + pos;
    }

protected:

    ssize_t read_more()
    {
        T* buffer = _buffer.get();

        if (_pos > 0) {
            std::memmove(buffer, buffer + _pos, _end - _pos);
            _end = _end - _pos;
            _pos = 0;
        }
        _end += _file.read(buffer + _end, _size - _end); // throw
        return _end;
    }

private:
    std::unique_ptr<T[]>    _buffer;
    size_t                  _size;
    size_t                  _end;
    size_t                  _pos;
    size_t                  _consumed;
    File&                   _file;
};


#endif // ESOUNPACK_FILEIO_H
