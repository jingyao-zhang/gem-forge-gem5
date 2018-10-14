
#include "pybind11/pybind11.h"
#include "pybind11/stl.h"

#include "mcpat/mcpat_manager.hh"

namespace py = pybind11;

// void
// pythonDump()
// {
//     py::module m = py::module::import("m5.stats");
//     m.attr("dump")();
// }

// void
// pythonReset()
// {
//     py::module m = py::module::import("m5.stats");
//     m.attr("reset")();
// }

// }

void pybind_init_mcpat(py::module &m_native) {
  py::module m = m_native.def_submodule("mcpat");
  m.def("dump", &McPATManager::dump);
}
