#include <Python.h>

typedef struct {
	PyObject_HEAD
} SimpleObject;

static PyTypeObject BaseObjectType = {
	PyObject_HEAD_INIT(NULL)
	0,				/* ob_size        */
	"type_test.Base",		/* tp_name        */
	sizeof(SimpleObject),		/* tp_basicsize   */
	0,				/* tp_itemsize    */
	0,				/* tp_dealloc     */
	0,				/* tp_print       */
	0,				/* tp_getattr     */
	0,				/* tp_setattr     */
	0,				/* tp_compare     */
	0,				/* tp_repr        */
	0,				/* tp_as_number   */
	0,				/* tp_as_sequence */
	0,				/* tp_as_mapping  */
	0,				/* tp_hash        */
	0,				/* tp_call        */
	0,				/* tp_str         */
	0,				/* tp_getattro    */
	0,				/* tp_setattro    */
	0,				/* tp_as_buffer   */
	Py_TPFLAGS_DEFAULT,		/* tp_flags       */
	"Act as a ase type.",	/* tp_doc         */
    0,		        /* tp_traverse */
    0,		        /* tp_clear */
    0,		        /* tp_richcompare */
    0,		        /* tp_weaklistoffset */
    0,		        /* tp_iter */
    0,		        /* tp_iternext */
    0,              /* tp_methods */
    0,              /* tp_members */
    0,              /* tp_getset */
    0,              /* tp_base */
    0,              /* tp_dict */
    0,              /* tp_descr_get */
    0,              /* tp_descr_set */
    0,              /* tp_dictoffset */
    0,              /* tp_init */
    0,              /* tp_alloc */
    0,              /* tp_new */
};

static PyTypeObject SubObjectType = {
	PyObject_HEAD_INIT(NULL)
	0,				/* ob_size        */
	"type_test.Sub",		/* tp_name        */
	sizeof(SimpleObject),		/* tp_basicsize   */
	0,				/* tp_itemsize    */
	0,				/* tp_dealloc     */
	0,				/* tp_print       */
	0,				/* tp_getattr     */
	0,				/* tp_setattr     */
	0,				/* tp_compare     */
	0,				/* tp_repr        */
	0,				/* tp_as_number   */
	0,				/* tp_as_sequence */
	0,				/* tp_as_mapping  */
	0,				/* tp_hash        */
	0,				/* tp_call        */
	0,				/* tp_str         */
	0,				/* tp_getattro    */
	0,				/* tp_setattro    */
	0,				/* tp_as_buffer   */
	Py_TPFLAGS_DEFAULT,		/* tp_flags       */
	"Act as a subtype",	/* tp_doc         */
    0,		        /* tp_traverse */
    0,		        /* tp_clear */
    0,		        /* tp_richcompare */
    0,		        /* tp_weaklistoffset */
    0,		        /* tp_iter */
    0,		        /* tp_iternext */
    0,              /* tp_methods */
    0,              /* tp_members */
    0,              /* tp_getset */
    &BaseObjectType,/* tp_base */
    0,              /* tp_dict */
    0,              /* tp_descr_get */
    0,              /* tp_descr_set */
    0,              /* tp_dictoffset */
    0,              /* tp_init */
    0,              /* tp_alloc */
    0,              /* tp_new */
};

PyMODINIT_FUNC
inittype_test(void) 
{
	PyObject* m;

	SubObjectType.tp_new = PyType_GenericNew;
	if (PyType_Ready(&SubObjectType) < 0)
		return;

	m = Py_InitModule3("type_test", NULL,
			   "A module that creates two extension type.");
	if (m == NULL)
		return;

	Py_INCREF(&SubObjectType);
	PyModule_AddObject(m, "Sub", (PyObject *)&SubObjectType);
}
