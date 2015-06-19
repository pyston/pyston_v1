/***********************************************************
Copyright 1994 by Lance Ellinghouse,
Cathedral City, California Republic, United States of America.

                        All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the name of Lance Ellinghouse
not be used in advertising or publicity pertaining to distribution
of the software without specific, written prior permission.

LANCE ELLINGHOUSE DISCLAIMS ALL WARRANTIES WITH REGARD TO
THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS, IN NO EVENT SHALL LANCE ELLINGHOUSE BE LIABLE FOR ANY SPECIAL,
INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

******************************************************************/

/******************************************************************

Revision history:

2010/04/20 (Sean Reifschneider)
  - Use basename(sys.argv[0]) for the default "ident".
  - Arguments to openlog() are now keyword args and are all optional.
  - syslog() calls openlog() if it hasn't already been called.

1998/04/28 (Sean Reifschneider)
  - When facility not specified to syslog() method, use default from openlog()
    (This is how it was claimed to work in the documentation)
  - Potential resource leak of o_ident, now cleaned up in closelog()
  - Minor comment accuracy fix.

95/06/29 (Steve Clift)
  - Changed arg parsing to use PyArg_ParseTuple.
  - Added PyErr_Clear() call(s) where needed.
  - Fix core dumps if user message contains format specifiers.
  - Change openlog arg defaults to match normal syslog behavior.
  - Plug memory leak in openlog().
  - Fix setlogmask() to return previous mask value.

******************************************************************/

/* syslog module */

#include "Python.h"
#include "osdefs.h"

#include <syslog.h>

/*  only one instance, only one syslog, so globals should be ok  */
static PyObject *S_ident_o = NULL;                      /*  identifier, held by openlog()  */
static char S_log_open = 0;


static PyObject *
syslog_get_argv(void)
{
    /* Figure out what to use for as the program "ident" for openlog().
     * This swallows exceptions and continues rather than failing out,
     * because the syslog module can still be used because openlog(3)
     * is optional.
     */

    Py_ssize_t argv_len;
    PyObject *scriptobj;
    char *atslash;
    PyObject *argv = PySys_GetObject("argv");

    if (argv == NULL) {
        return(NULL);
    }

    argv_len = PyList_Size(argv);
    if (argv_len == -1) {
        PyErr_Clear();
        return(NULL);
    }
    if (argv_len == 0) {
        return(NULL);
    }

    scriptobj = PyList_GetItem(argv, 0);
    if (!PyString_Check(scriptobj)) {
        return(NULL);
    }
    if (PyString_GET_SIZE(scriptobj) == 0) {
        return(NULL);
    }

    atslash = strrchr(PyString_AsString(scriptobj), SEP);
    if (atslash) {
        return(PyString_FromString(atslash + 1));
    } else {
        Py_INCREF(scriptobj);
        return(scriptobj);
    }

    return(NULL);
}


static PyObject *
syslog_openlog(PyObject * self, PyObject * args, PyObject *kwds)
{
    long logopt = 0;
    long facility = LOG_USER;
    PyObject *new_S_ident_o = NULL;
    static char *keywords[] = {"ident", "logoption", "facility", 0};

    if (!PyArg_ParseTupleAndKeywords(args, kwds,
                          "|Sll:openlog", keywords, &new_S_ident_o, &logopt, &facility))
        return NULL;

    if (new_S_ident_o) { Py_INCREF(new_S_ident_o); }

    /*  get sys.argv[0] or NULL if we can't for some reason  */
    if (!new_S_ident_o) {
        new_S_ident_o = syslog_get_argv();
        }

    Py_XDECREF(S_ident_o);
    S_ident_o = new_S_ident_o;

    /* At this point, S_ident_o should be INCREF()ed.  openlog(3) does not
     * make a copy, and syslog(3) later uses it.  We can't garbagecollect it
     * If NULL, just let openlog figure it out (probably using C argv[0]).
     */

    openlog(S_ident_o ? PyString_AsString(S_ident_o) : NULL, logopt, facility);
    S_log_open = 1;

    Py_INCREF(Py_None);
    return Py_None;
}


static PyObject *
syslog_syslog(PyObject * self, PyObject * args)
{
    char *message;
    int   priority = LOG_INFO;

    if (!PyArg_ParseTuple(args, "is;[priority,] message string",
                          &priority, &message)) {
        PyErr_Clear();
        if (!PyArg_ParseTuple(args, "s;[priority,] message string",
                              &message))
            return NULL;
    }

    /*  if log is not opened, open it now  */
    if (!S_log_open) {
        PyObject *openargs;

        /* Continue even if PyTuple_New fails, because openlog(3) is optional.
         * So, we can still do loggin in the unlikely event things are so hosed
         * that we can't do this tuple.
         */
        if ((openargs = PyTuple_New(0))) {
            PyObject *openlog_ret = syslog_openlog(self, openargs, NULL);
            Py_XDECREF(openlog_ret);
            Py_DECREF(openargs);
        }
    }

    Py_BEGIN_ALLOW_THREADS;
    syslog(priority, "%s", message);
    Py_END_ALLOW_THREADS;
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
syslog_closelog(PyObject *self, PyObject *unused)
{
    if (S_log_open) {
        closelog();
        Py_XDECREF(S_ident_o);
        S_ident_o = NULL;
        S_log_open = 0;
    }
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
syslog_setlogmask(PyObject *self, PyObject *args)
{
    long maskpri, omaskpri;

    if (!PyArg_ParseTuple(args, "l;mask for priority", &maskpri))
        return NULL;
    omaskpri = setlogmask(maskpri);
    return PyInt_FromLong(omaskpri);
}

static PyObject *
syslog_log_mask(PyObject *self, PyObject *args)
{
    long mask;
    long pri;
    if (!PyArg_ParseTuple(args, "l:LOG_MASK", &pri))
        return NULL;
    mask = LOG_MASK(pri);
    return PyInt_FromLong(mask);
}

static PyObject *
syslog_log_upto(PyObject *self, PyObject *args)
{
    long mask;
    long pri;
    if (!PyArg_ParseTuple(args, "l:LOG_UPTO", &pri))
        return NULL;
    mask = LOG_UPTO(pri);
    return PyInt_FromLong(mask);
}

/* List of functions defined in the module */

static PyMethodDef syslog_methods[] = {
    {"openlog",         (PyCFunction) syslog_openlog,           METH_VARARGS | METH_KEYWORDS},
    {"closelog",        syslog_closelog,        METH_NOARGS},
    {"syslog",          syslog_syslog,          METH_VARARGS},
    {"setlogmask",      syslog_setlogmask,      METH_VARARGS},
    {"LOG_MASK",        syslog_log_mask,        METH_VARARGS},
    {"LOG_UPTO",        syslog_log_upto,        METH_VARARGS},
    {NULL,              NULL,                   0}
};

/* Initialization function for the module */

PyMODINIT_FUNC
initsyslog(void)
{
    PyObject *m;

    /* Create the module and add the functions */
    m = Py_InitModule("syslog", syslog_methods);
    if (m == NULL)
        return;

    /* Add some symbolic constants to the module */

    /* Priorities */
    PyModule_AddIntConstant(m, "LOG_EMERG",       LOG_EMERG);
    PyModule_AddIntConstant(m, "LOG_ALERT",       LOG_ALERT);
    PyModule_AddIntConstant(m, "LOG_CRIT",        LOG_CRIT);
    PyModule_AddIntConstant(m, "LOG_ERR",         LOG_ERR);
    PyModule_AddIntConstant(m, "LOG_WARNING", LOG_WARNING);
    PyModule_AddIntConstant(m, "LOG_NOTICE",  LOG_NOTICE);
    PyModule_AddIntConstant(m, "LOG_INFO",        LOG_INFO);
    PyModule_AddIntConstant(m, "LOG_DEBUG",       LOG_DEBUG);

    /* openlog() option flags */
    PyModule_AddIntConstant(m, "LOG_PID",         LOG_PID);
    PyModule_AddIntConstant(m, "LOG_CONS",        LOG_CONS);
    PyModule_AddIntConstant(m, "LOG_NDELAY",  LOG_NDELAY);
#ifdef LOG_NOWAIT
    PyModule_AddIntConstant(m, "LOG_NOWAIT",  LOG_NOWAIT);
#endif
#ifdef LOG_PERROR
    PyModule_AddIntConstant(m, "LOG_PERROR",  LOG_PERROR);
#endif

    /* Facilities */
    PyModule_AddIntConstant(m, "LOG_KERN",        LOG_KERN);
    PyModule_AddIntConstant(m, "LOG_USER",        LOG_USER);
    PyModule_AddIntConstant(m, "LOG_MAIL",        LOG_MAIL);
    PyModule_AddIntConstant(m, "LOG_DAEMON",  LOG_DAEMON);
    PyModule_AddIntConstant(m, "LOG_AUTH",        LOG_AUTH);
    PyModule_AddIntConstant(m, "LOG_LPR",         LOG_LPR);
    PyModule_AddIntConstant(m, "LOG_LOCAL0",  LOG_LOCAL0);
    PyModule_AddIntConstant(m, "LOG_LOCAL1",  LOG_LOCAL1);
    PyModule_AddIntConstant(m, "LOG_LOCAL2",  LOG_LOCAL2);
    PyModule_AddIntConstant(m, "LOG_LOCAL3",  LOG_LOCAL3);
    PyModule_AddIntConstant(m, "LOG_LOCAL4",  LOG_LOCAL4);
    PyModule_AddIntConstant(m, "LOG_LOCAL5",  LOG_LOCAL5);
    PyModule_AddIntConstant(m, "LOG_LOCAL6",  LOG_LOCAL6);
    PyModule_AddIntConstant(m, "LOG_LOCAL7",  LOG_LOCAL7);

#ifndef LOG_SYSLOG
#define LOG_SYSLOG              LOG_DAEMON
#endif
#ifndef LOG_NEWS
#define LOG_NEWS                LOG_MAIL
#endif
#ifndef LOG_UUCP
#define LOG_UUCP                LOG_MAIL
#endif
#ifndef LOG_CRON
#define LOG_CRON                LOG_DAEMON
#endif

    PyModule_AddIntConstant(m, "LOG_SYSLOG",  LOG_SYSLOG);
    PyModule_AddIntConstant(m, "LOG_CRON",        LOG_CRON);
    PyModule_AddIntConstant(m, "LOG_UUCP",        LOG_UUCP);
    PyModule_AddIntConstant(m, "LOG_NEWS",        LOG_NEWS);
}
