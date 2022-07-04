#include <stdio.h>
#include <ATen/ATen.h>
#include <torch/extension.h>
#include <torch/csrc/utils/pybind.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <string>
#include <stdexcept>
#include <unordered_map>
#include <error.h>
#include "aio.h"
#include "space_mgr.h"

class Offloader
{
public:
    Offloader(const std::string &filename, unsigned int n_entries) : filename(filename), aio(AsyncIO(n_entries)), space_mgr(SpaceManager(0))
    {
        this->fd = open(filename.c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
    }

    void write(const at::Tensor &tensor, const std::string &key)
    {
        if (!tensor.is_contiguous() || !tensor.is_cpu())
            throw std::runtime_error("Tensor must be contiguous and on cpu");
        ull bytes = tensor.storage().nbytes();
        ull offset = this->space_mgr.alloc(bytes);
        this->tensors_info[key] = SpaceInfo(offset, bytes);
        this->aio.write(this->fd, tensor.data_ptr(), bytes, offset, nullptr);
    }

    void read(const at::Tensor &tensor, const std::string &key)
    {
        if (!tensor.is_contiguous() || !tensor.is_cpu())
            throw std::runtime_error("Tensor must be contiguous and on cpu");
        if (this->tensors_info.find(key) == this->tensors_info.end())
            throw std::runtime_error("Read error, tensor not found");
        ull bytes = tensor.storage().nbytes();
        if (bytes != this->tensors_info[key].second)
            throw std::runtime_error("Read error, tensor shape mismatch");
        auto fn = std::bind(&Offloader::release, this, std::ref(key), this->tensors_info[key].first, bytes);
        this->aio.read(this->fd, tensor.data_ptr(), bytes, this->tensors_info[key].first, fn);
    }

    void sync_write_events()
    {
        this->aio.sync_write_events();
    }

    void sync_read_events()
    {
        this->aio.sync_read_events();
    }

    void synchronize()
    {
        this->aio.synchronize();
    }

    ~Offloader()
    {
        errno = 0;
        synchronize();
        close(this->fd);
        if (remove(this->filename.c_str()) != 0)
            printf("Remove \"%s\" error(%d): %s\n", this->filename.c_str(), errno, strerror(errno));
    }

private:
    const std::string filename;
    int fd;
    AsyncIO aio;
    SpaceManager space_mgr;
    std::unordered_map<std::string, SpaceInfo> tensors_info;

    void release(const std::string &key, ull offset, ull bytes)
    {
        this->space_mgr.free(offset, bytes);
        this->tensors_info.erase(key);
    }
};

PYBIND11_MODULE(colo_nvme, m)
{
    pybind11::class_<Offloader>(m, "Offloader")
        .def(pybind11::init<const std::string &, unsigned int>())
        .def("write", &Offloader::write)
        .def("read", &Offloader::read)
        .def("sync_write_events", &Offloader::sync_write_events)
        .def("sync_read_events", &Offloader::sync_write_events)
        .def("synchronize", &Offloader::synchronize);
}