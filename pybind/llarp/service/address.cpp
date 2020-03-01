#include "common.hpp"
#include "service/address.hpp"

namespace llarp
{
  namespace service
  {
    void 
    Address_Init(py::module & mod)
    {
      py::class_<Address>(mod, "ServiceAddress")
      .def(py::init<std::string>())
      .def("__str__", [](const Address & addr) -> std::string {
        return addr.ToString();
      });
    }
  } 
}