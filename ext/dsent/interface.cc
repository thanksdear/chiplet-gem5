/* Copyright (c) 2014 Mark D. Hill and David A. Wood
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <Python.h>
#include <cstdio>

#include "DSENT.h"
#include "libutil/String.h"
#include "model/Model.h"

using namespace std;
using namespace LibUtil;

static PyObject *DSENTError;
static PyObject* dsent_initialize(PyObject*, PyObject*);
static PyObject* dsent_finalize(PyObject*, PyObject*);
static PyObject* dsent_computeRouterPowerAndArea(PyObject*, PyObject*);
static PyObject* dsent_computeLinkPower(PyObject*, PyObject*);

// Create DSENT configuration map.  This map is supposed to retain all
// the information between calls to initialize() and finalize().
map<String, String> params;
DSENT::Model *ms_model;


static PyMethodDef DSENTMethods[] = {
    {"initialize", dsent_initialize, METH_O,
     "initialize dsent using a config file."},

    {"finalize", dsent_finalize, METH_NOARGS,
     "finalize dsent by dstroying the config object"},

    {"computeRouterPowerAndArea", dsent_computeRouterPowerAndArea,
     METH_VARARGS, "compute quantities related power consumption of a router"},

    {"computeLinkPower", dsent_computeLinkPower, METH_O,
     "compute quantities related power consumption of a link"},

    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef dsentmodule = {
    PyModuleDef_HEAD_INIT,
    "dsent",
    NULL,
    -1,
    DSENTMethods
};

PyMODINIT_FUNC
PyInit_dsent(void)
{
    PyObject *m;

    m = PyModule_Create(&dsentmodule);
    if (m == NULL) return NULL;

    DSENTError = PyErr_NewException("dsent.error", NULL, NULL);
    Py_INCREF(DSENTError);
    PyModule_AddObject(m, "error", DSENTError);

    ms_model = nullptr;

    return m;
}


static PyObject *
dsent_initialize(PyObject *self, PyObject *arg)
{
    const char *config_file = PyUnicode_AsUTF8(arg);
    //Read the arguments sent from the python script
    if (!config_file) {
        Py_RETURN_NONE;
    }

    // Initialize DSENT
    ms_model = DSENT::initialize(config_file, params);
    Py_RETURN_NONE;
}


static PyObject *
dsent_finalize(PyObject *self, PyObject *args)
{
    // Finalize DSENT
    DSENT::finalize(params, ms_model);
    ms_model = nullptr;
    Py_RETURN_NONE;
}


static PyObject *
dsent_computeRouterPowerAndArea(PyObject *self, PyObject *args)
{
    uint64_t frequency;
    unsigned int num_in_port;
    unsigned int num_out_port;
    unsigned int num_vclass;
    unsigned int num_vchannels;
    unsigned int flit_width;
    PyObject *buffers_obj;  // can be int or tuple of ints

    // Read the arguments sent from the python script
    // Format: K=int64, I=unsigned int, O=any PyObject
    if (!PyArg_ParseTuple(args, "KIIIIOI", &frequency, &num_in_port,
                          &num_out_port, &num_vclass, &num_vchannels,
                          &buffers_obj, &flit_width)) {
        Py_RETURN_NONE;
    }

    assert(frequency > 0.0);
    assert(num_in_port != 0);
    assert(num_out_port != 0);
    assert(num_vclass != 0);
    assert(flit_width != 0);

    vector<unsigned int> num_vchannels_vec(num_vclass, num_vchannels);
    vector<unsigned int> num_buffers_vec;

    // Flexible buffer depth per-VN: accept int (uniform) or tuple (per-VN)
    if (PyLong_Check(buffers_obj)) {
        // Single int: same depth for all VNs (backwards compatible)
        num_buffers_vec.resize(num_vclass, (unsigned int)PyLong_AsLong(buffers_obj));
    } else if (PyTuple_Check(buffers_obj) || PyList_Check(buffers_obj)) {
        // Tuple/list: one depth per VN
        Py_ssize_t n = PyTuple_Check(buffers_obj) ?
                       PyTuple_Size(buffers_obj) : PyList_Size(buffers_obj);
        if ((size_t)n != num_vclass) {
            PyErr_SetString(PyExc_ValueError,
                "Number of buffer depths must match number of VNs");
            return NULL;
        }
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *item = PyTuple_Check(buffers_obj) ?
                             PyTuple_GetItem(buffers_obj, i) :
                             PyList_GetItem(buffers_obj, i);
            num_buffers_vec.push_back((unsigned int)PyLong_AsLong(item));
        }
    } else {
        PyErr_SetString(PyExc_TypeError,
            "buffers_per_vc must be int or tuple/list of ints");
        return NULL;
    }
    // DSENT outputs
    map<string, double> outputs;

    params["Frequency"] = String(frequency);
    params["NumberInputPorts"] = String(num_in_port);
    params["NumberOutputPorts"] = String(num_out_port);
    params["NumberVirtualNetworks"] = String(num_vclass);
    params["NumberVirtualChannelsPerVirtualNetwork"] =
        vectorToString<unsigned int>(num_vchannels_vec);
    params["NumberBuffersPerVirtualChannel"] =
        vectorToString<unsigned int>(num_buffers_vec);
    params["NumberBitsPerFlit"] = String(flit_width);

    // Rebuild model with updated structural params (buffer depths, VNs, etc.)
    delete ms_model;
    ms_model = DSENT::rebuildModel(params);

    // Run DSENT
    DSENT::run(params, ms_model, outputs);

    // Store outputs
    PyObject *r = PyTuple_New(outputs.size());
    int index = 0;

    // Prepare the output.
    for (const auto &it : outputs) {
        PyObject *s = PyTuple_New(2);
        PyTuple_SetItem(s, 0, PyUnicode_FromString(it.first.c_str()));
        PyTuple_SetItem(s, 1, PyFloat_FromDouble(it.second));

        PyTuple_SetItem(r, index, s);
        index++;
    }

    return r;
}


static PyObject *
dsent_computeLinkPower(PyObject *self, PyObject *arg)
{
    uint64_t frequency = PyLong_AsLongLong(arg);

    // Read the arguments sent from the python script
    if (PyErr_Occurred()) {
        Py_RETURN_NONE;
    }

    // DSENT outputs
    map<string, double> outputs;
    params["Frequency"] = String(frequency);

    // Run DSENT
    DSENT::run(params, ms_model, outputs);

    // Store outputs
    PyObject *r = PyTuple_New(outputs.size());
    int index = 0;

    // Prepare the output.
    for (const auto &it : outputs) {
        PyObject *s = PyTuple_New(2);
        PyTuple_SetItem(s, 0, PyUnicode_FromString(it.first.c_str()));
        PyTuple_SetItem(s, 1, PyFloat_FromDouble(it.second));

        PyTuple_SetItem(r, index, s);
        index++;
    }

    return r;
}
