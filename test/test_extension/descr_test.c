#include <string.h>

#include <Python.h>
#include <structmember.h>

typedef struct {
    PyObject_HEAD

    short member_short;
    int member_int;
    long member_long;
    float member_float;
    double member_double;
    char* member_string;
    char member_string_inplace[80];
    char member_char;
    int8_t member_byte;
    uint8_t member_ubyte;
    unsigned short member_ushort;
    unsigned int member_uint;
    unsigned long member_ulong;
    char member_bool;
    PyObject* member_object;
    PyObject* member_object_ex;
    long long member_long_long;
    unsigned long long member_ulong_long;
    Py_ssize_t member_pyssizet;
} descr_tester_object;

char* string1 = "string1";
char* string2 = "string2";
char* string_empty = "";

static struct PyMemberDef descr_memberlist[] = {
    {"member_short", T_SHORT, offsetof(descr_tester_object, member_short), READONLY},
    {"member_int", T_INT, offsetof(descr_tester_object, member_int), READONLY},
    {"member_long", T_LONG, offsetof(descr_tester_object, member_long), READONLY},
    {"member_float", T_FLOAT, offsetof(descr_tester_object, member_float), READONLY},
    {"member_double", T_DOUBLE, offsetof(descr_tester_object, member_double), READONLY},
    {"member_string", T_STRING, offsetof(descr_tester_object, member_string), READONLY},
    {"member_string_inplace", T_STRING_INPLACE, offsetof(descr_tester_object, member_string_inplace), READONLY},
    {"member_char", T_CHAR, offsetof(descr_tester_object, member_char), READONLY},
    {"member_byte", T_BYTE, offsetof(descr_tester_object, member_byte), READONLY},
    {"member_ubyte", T_UBYTE, offsetof(descr_tester_object, member_ubyte), READONLY},
    {"member_ushort", T_USHORT, offsetof(descr_tester_object, member_ushort), READONLY},
    {"member_uint", T_UINT, offsetof(descr_tester_object, member_uint), READONLY},
    {"member_ulong", T_ULONG, offsetof(descr_tester_object, member_ulong), READONLY},
    {"member_bool", T_BOOL, offsetof(descr_tester_object, member_bool), READONLY},
    {"member_object", T_OBJECT, offsetof(descr_tester_object, member_object), READONLY},
    {"member_object_ex", T_OBJECT_EX, offsetof(descr_tester_object, member_object_ex), READONLY},
    {"member_long_long", T_LONGLONG, offsetof(descr_tester_object, member_long_long), READONLY},
    {"member_ulong_long", T_ULONGLONG, offsetof(descr_tester_object, member_ulong_long), READONLY},
    {"member_pyssizet", T_PYSSIZET, offsetof(descr_tester_object, member_pyssizet), READONLY},
    {NULL}
};

static void
descr_tester_dealloc(descr_tester_object *mc)
{
    printf("dealloc\n");
    if (mc->member_object)
        Py_DECREF(mc->member_object);
        
    if (mc->member_object_ex)
        Py_DECREF(mc->member_object_ex);

    PyObject_GC_UnTrack(mc);
    PyObject_GC_Del(mc);
    printf("done dealloc\n");
}

PyDoc_STRVAR(descr_tester_doc, "descr_tester doc");

static int
descr_tester_traverse(descr_tester_object *mc, visitproc visit, void *arg)
{
    printf("traverse");
    if (mc->member_object != NULL)
        Py_VISIT(mc->member_object);
    if (mc->member_object_ex != NULL)
        Py_VISIT(mc->member_object_ex);
    printf("done traverse");
    return 0;
}

static PyTypeObject descr_tester;

static PyObject *
descr_tester_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    printf("shouldn't get here? (new)");
    return NULL;
}

static PyObject *
descr_tester_call(descr_tester_object *mc, PyObject *args, PyObject *kw)
{
    printf("shouldn't get here? (call)");
    return NULL;
}

static PyTypeObject descr_tester = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "descr_test.descr_tester",            /* tp_name */
    sizeof(descr_tester_object),          /* tp_basicsize */
    0,                                  /* tp_itemsize */
    /* methods */
    (destructor)descr_tester_dealloc,      /* tp_dealloc */
    0,                                  /* tp_print */
    0,                                  /* tp_getattr */
    0,                                  /* tp_setattr */
    0,                                  /* tp_compare */
    0,                                  /* tp_repr */
    0,                                  /* tp_as_number */
    0,                                  /* tp_as_sequence */
    0,                                  /* tp_as_mapping */
    0,                                  /* tp_hash */
    (ternaryfunc)descr_tester_call,     /* tp_call */
    0,                                  /* tp_str */
    PyObject_GenericGetAttr,            /* tp_getattro */
    0,                                  /* tp_setattro */
    0,                                  /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,/* tp_flags */
    descr_tester_doc,                   /* tp_doc */
    (traverseproc)descr_tester_traverse,        /* tp_traverse */
    0,                                  /* tp_clear */
    0,                                  /* tp_richcompare */
    0,                                  /* tp_weaklistoffset */
    0,                                  /* tp_iter */
    0,                                  /* tp_iternext */
    0,                                  /* tp_methods */
    descr_memberlist,                   /* tp_members */
    0,                                  /* tp_getset */
    0,                                  /* tp_base */
    0,                                  /* tp_dict */
    0,                                  /* tp_descr_get */
    0,                                  /* tp_descr_set */
    0,                                  /* tp_dictoffset */
    0,                                  /* tp_init */
    0,                                  /* tp_alloc */
    descr_tester_new,                   /* tp_new */
    0,                                  /* tp_free */
};

static PyMethodDef DescrTestMethods[] = {
    {NULL, NULL, 0, NULL}        /* Sentinel */
};

PyMODINIT_FUNC
initdescr_test(void)
{
    int res = PyType_Ready(&descr_tester);
    if (res < 0)
        return;
    Py_INCREF(&descr_tester);

    descr_tester_object* o1 = PyObject_GC_New(descr_tester_object, &descr_tester);
    if (o1 == NULL)
        return;
    o1->member_short = SHRT_MAX;
    o1->member_int = INT_MAX;
    o1->member_long = LONG_MAX;
    o1->member_float = 1.0f;
    o1->member_double = 2.0;
    o1->member_string = string1;
    strcpy(o1->member_string_inplace, string2);
    o1->member_char = 'A';
    o1->member_byte = CHAR_MAX;
    o1->member_ubyte = UCHAR_MAX;
    o1->member_ushort = USHRT_MAX;
    o1->member_uint = UINT_MAX;
    o1->member_ulong = ULONG_MAX;
    o1->member_bool = 1;
    o1->member_object = PyInt_FromLong(1500);
    o1->member_object_ex = PyInt_FromLong(1600);
    o1->member_long_long = LLONG_MAX;
    o1->member_ulong_long = ULLONG_MAX;
    o1->member_pyssizet = (Py_ssize_t)((1ULL << (8 * sizeof(Py_ssize_t) - 1)) - 1); // max Py_ssize_t

    descr_tester_object* o2 = PyObject_GC_New(descr_tester_object, &descr_tester);

    if (o2 == NULL)
        return;
    o2->member_short = SHRT_MIN;
    o2->member_int = INT_MIN;
    o2->member_long = LONG_MIN;
    o2->member_float = 3.0f;
    o2->member_double = 4.0;
    o2->member_string = NULL; // let's see what happens!
    strcpy(o2->member_string_inplace, string_empty);
    o2->member_char = 'a';
    o2->member_byte = CHAR_MIN;
    o2->member_ubyte = 0;
    o2->member_ushort = 0;
    o2->member_uint = 0;
    o2->member_ulong = 0;
    o2->member_bool = 0;
    o2->member_object = NULL; // None
    o2->member_object_ex = NULL; // Exception
    o2->member_long_long = LLONG_MIN;
    o2->member_ulong_long = 0;
    o2->member_pyssizet = o1->member_pyssizet + 1; // min Py_ssize_t


    PyObject *m;
    m = Py_InitModule("descr_test", DescrTestMethods);
    if (m == NULL)
        return;
    PyModule_AddObject(m, "descr_tester", (PyObject *)&descr_tester);

    PyModule_AddObject(m, "member_descr_object1", (PyObject *)o1);
    PyModule_AddObject(m, "member_descr_object2", (PyObject *)o2);
}
