/* SSL socket module

   SSL support based on patches by Brian E Gallew and Laszlo Kovacs.
   Re-worked a bit by Bill Janssen to add server-side support and
   certificate decoding.  Chris Stawarz contributed some non-blocking
   patches.

   This module is imported by ssl.py. It should *not* be used
   directly.

   XXX should partial writes be enabled, SSL_MODE_ENABLE_PARTIAL_WRITE?

   XXX integrate several "shutdown modes" as suggested in
       http://bugs.python.org/issue8108#msg102867 ?
*/

#include "Python.h"

#ifdef WITH_THREAD
#include "pythread.h"

// Pyston change: changed to use our internal API
/*
#define PySSL_BEGIN_ALLOW_THREADS { \
            PyThreadState *_save = NULL;  \
            if (_ssl_locks_count>0) {_save = PyEval_SaveThread();}
#define PySSL_BLOCK_THREADS     if (_ssl_locks_count>0){PyEval_RestoreThread(_save)};
#define PySSL_UNBLOCK_THREADS   if (_ssl_locks_count>0){_save = PyEval_SaveThread()};
#define PySSL_END_ALLOW_THREADS if (_ssl_locks_count>0){PyEval_RestoreThread(_save);} \
         }
*/
#define PySSL_BEGIN_ALLOW_THREADS { \
                                    if (_ssl_locks_count>0) beginAllowThreads();
#define PySSL_BLOCK_THREADS         if (_ssl_locks_count>0) endAllowThreads();
#define PySSL_UNBLOCK_THREADS       if (_ssl_locks_count>0) beginAllowThreads();
#define PySSL_END_ALLOW_THREADS     if (_ssl_locks_count>0) endAllowThreads(); \
                                  }

#else   /* no WITH_THREAD */

#define PySSL_BEGIN_ALLOW_THREADS
#define PySSL_BLOCK_THREADS
#define PySSL_UNBLOCK_THREADS
#define PySSL_END_ALLOW_THREADS

#endif

enum py_ssl_error {
    /* these mirror ssl.h */
    PY_SSL_ERROR_NONE,
    PY_SSL_ERROR_SSL,
    PY_SSL_ERROR_WANT_READ,
    PY_SSL_ERROR_WANT_WRITE,
    PY_SSL_ERROR_WANT_X509_LOOKUP,
    PY_SSL_ERROR_SYSCALL,     /* look at error stack/return value/errno */
    PY_SSL_ERROR_ZERO_RETURN,
    PY_SSL_ERROR_WANT_CONNECT,
    /* start of non ssl.h errorcodes */
    PY_SSL_ERROR_EOF,         /* special case of SSL_ERROR_SYSCALL */
    PY_SSL_ERROR_INVALID_ERROR_CODE
};

enum py_ssl_server_or_client {
    PY_SSL_CLIENT,
    PY_SSL_SERVER
};

enum py_ssl_cert_requirements {
    PY_SSL_CERT_NONE,
    PY_SSL_CERT_OPTIONAL,
    PY_SSL_CERT_REQUIRED
};

enum py_ssl_version {
#ifndef OPENSSL_NO_SSL2
    PY_SSL_VERSION_SSL2,
#endif
    PY_SSL_VERSION_SSL3=1,
    PY_SSL_VERSION_SSL23,
    PY_SSL_VERSION_TLS1
};

/* Include symbols from _socket module */
#include "socketmodule.h"

#if defined(HAVE_POLL_H)
#include <poll.h>
#elif defined(HAVE_SYS_POLL_H)
#include <sys/poll.h>
#endif

/* Include OpenSSL header files */
#include "openssl/rsa.h"
#include "openssl/crypto.h"
#include "openssl/x509.h"
#include "openssl/x509v3.h"
#include "openssl/pem.h"
#include "openssl/ssl.h"
#include "openssl/err.h"
#include "openssl/rand.h"

/* SSL error object */
static PyObject *PySSLErrorObject;

#ifdef WITH_THREAD

/* serves as a flag to see whether we've initialized the SSL thread support. */
/* 0 means no, greater than 0 means yes */

static unsigned int _ssl_locks_count = 0;

#endif /* def WITH_THREAD */

/* SSL socket object */

#define X509_NAME_MAXLEN 256

/* RAND_* APIs got added to OpenSSL in 0.9.5 */
#if OPENSSL_VERSION_NUMBER >= 0x0090500fL
# define HAVE_OPENSSL_RAND 1
#else
# undef HAVE_OPENSSL_RAND
#endif

typedef struct {
    PyObject_HEAD
    PySocketSockObject *Socket;         /* Socket on which we're layered */
    SSL_CTX*            ctx;
    SSL*                ssl;
    X509*               peer_cert;
    char                server[X509_NAME_MAXLEN];
    char                issuer[X509_NAME_MAXLEN];
    int                 shutdown_seen_zero;

} PySSLObject;

static PyTypeObject PySSL_Type;
static PyObject *PySSL_SSLwrite(PySSLObject *self, PyObject *args);
static PyObject *PySSL_SSLread(PySSLObject *self, PyObject *args);
static int check_socket_and_wait_for_timeout(PySocketSockObject *s,
                                             int writing);
static PyObject *PySSL_peercert(PySSLObject *self, PyObject *args);
static PyObject *PySSL_cipher(PySSLObject *self);

#define PySSLObject_Check(v)    (Py_TYPE(v) == &PySSL_Type)

typedef enum {
    SOCKET_IS_NONBLOCKING,
    SOCKET_IS_BLOCKING,
    SOCKET_HAS_TIMED_OUT,
    SOCKET_HAS_BEEN_CLOSED,
    SOCKET_TOO_LARGE_FOR_SELECT,
    SOCKET_OPERATION_OK
} timeout_state;

/* Wrap error strings with filename and line # */
#define STRINGIFY1(x) #x
#define STRINGIFY2(x) STRINGIFY1(x)
#define ERRSTR1(x,y,z) (x ":" y ": " z)
#define ERRSTR(x) ERRSTR1("_ssl.c", STRINGIFY2(__LINE__), x)

/* XXX It might be helpful to augment the error message generated
   below with the name of the SSL function that generated the error.
   I expect it's obvious most of the time.
*/

static PyObject *
PySSL_SetError(PySSLObject *obj, int ret, char *filename, int lineno)
{
    PyObject *v;
    char buf[2048];
    char *errstr;
    int err;
    enum py_ssl_error p = PY_SSL_ERROR_NONE;

    assert(ret <= 0);

    if (obj->ssl != NULL) {
        err = SSL_get_error(obj->ssl, ret);

        switch (err) {
        case SSL_ERROR_ZERO_RETURN:
            errstr = "TLS/SSL connection has been closed";
            p = PY_SSL_ERROR_ZERO_RETURN;
            break;
        case SSL_ERROR_WANT_READ:
            errstr = "The operation did not complete (read)";
            p = PY_SSL_ERROR_WANT_READ;
            break;
        case SSL_ERROR_WANT_WRITE:
            p = PY_SSL_ERROR_WANT_WRITE;
            errstr = "The operation did not complete (write)";
            break;
        case SSL_ERROR_WANT_X509_LOOKUP:
            p = PY_SSL_ERROR_WANT_X509_LOOKUP;
            errstr = "The operation did not complete (X509 lookup)";
            break;
        case SSL_ERROR_WANT_CONNECT:
            p = PY_SSL_ERROR_WANT_CONNECT;
            errstr = "The operation did not complete (connect)";
            break;
        case SSL_ERROR_SYSCALL:
        {
            unsigned long e = ERR_get_error();
            if (e == 0) {
                if (ret == 0 || !obj->Socket) {
                    p = PY_SSL_ERROR_EOF;
                    errstr = "EOF occurred in violation of protocol";
                } else if (ret == -1) {
                    /* underlying BIO reported an I/O error */
                    ERR_clear_error();
                    return obj->Socket->errorhandler();
                } else { /* possible? */
                    p = PY_SSL_ERROR_SYSCALL;
                    errstr = "Some I/O error occurred";
                }
            } else {
                p = PY_SSL_ERROR_SYSCALL;
                /* XXX Protected by global interpreter lock */
                errstr = ERR_error_string(e, NULL);
            }
            break;
        }
        case SSL_ERROR_SSL:
        {
            unsigned long e = ERR_get_error();
            p = PY_SSL_ERROR_SSL;
            if (e != 0)
                /* XXX Protected by global interpreter lock */
                errstr = ERR_error_string(e, NULL);
            else {              /* possible? */
                errstr = "A failure in the SSL library occurred";
            }
            break;
        }
        default:
            p = PY_SSL_ERROR_INVALID_ERROR_CODE;
            errstr = "Invalid error code";
        }
    } else {
        errstr = ERR_error_string(ERR_peek_last_error(), NULL);
    }
    PyOS_snprintf(buf, sizeof(buf), "_ssl.c:%d: %s", lineno, errstr);
    ERR_clear_error();
    v = Py_BuildValue("(is)", p, buf);
    if (v != NULL) {
        PyErr_SetObject(PySSLErrorObject, v);
        Py_DECREF(v);
    }
    return NULL;
}

static PyObject *
_setSSLError (char *errstr, int errcode, char *filename, int lineno) {

    char buf[2048];
    PyObject *v;

    if (errstr == NULL) {
        errcode = ERR_peek_last_error();
        errstr = ERR_error_string(errcode, NULL);
    }
    PyOS_snprintf(buf, sizeof(buf), "_ssl.c:%d: %s", lineno, errstr);
    ERR_clear_error();
    v = Py_BuildValue("(is)", errcode, buf);
    if (v != NULL) {
        PyErr_SetObject(PySSLErrorObject, v);
        Py_DECREF(v);
    }
    return NULL;
}

static PySSLObject *
newPySSLObject(PySocketSockObject *Sock, char *key_file, char *cert_file,
               enum py_ssl_server_or_client socket_type,
               enum py_ssl_cert_requirements certreq,
               enum py_ssl_version proto_version,
               char *cacerts_file, char *ciphers)
{
    PySSLObject *self;
    char *errstr = NULL;
    int ret;
    int verification_mode;
    long options;

    self = PyObject_New(PySSLObject, &PySSL_Type); /* Create new object */
    if (self == NULL)
        return NULL;
    memset(self->server, '\0', sizeof(char) * X509_NAME_MAXLEN);
    memset(self->issuer, '\0', sizeof(char) * X509_NAME_MAXLEN);
    self->peer_cert = NULL;
    self->ssl = NULL;
    self->ctx = NULL;
    self->Socket = NULL;
    self->shutdown_seen_zero = 0;

    /* Make sure the SSL error state is initialized */
    (void) ERR_get_state();
    ERR_clear_error();

    if ((key_file && !cert_file) || (!key_file && cert_file)) {
        errstr = ERRSTR("Both the key & certificate files "
                        "must be specified");
        goto fail;
    }

    if ((socket_type == PY_SSL_SERVER) &&
        ((key_file == NULL) || (cert_file == NULL))) {
        errstr = ERRSTR("Both the key & certificate files "
                        "must be specified for server-side operation");
        goto fail;
    }

    PySSL_BEGIN_ALLOW_THREADS
    if (proto_version == PY_SSL_VERSION_TLS1)
        self->ctx = SSL_CTX_new(TLSv1_method()); /* Set up context */
    else if (proto_version == PY_SSL_VERSION_SSL3)
        self->ctx = SSL_CTX_new(SSLv3_method()); /* Set up context */
#ifndef OPENSSL_NO_SSL2
    else if (proto_version == PY_SSL_VERSION_SSL2)
        self->ctx = SSL_CTX_new(SSLv2_method()); /* Set up context */
#endif
    else if (proto_version == PY_SSL_VERSION_SSL23)
        self->ctx = SSL_CTX_new(SSLv23_method()); /* Set up context */
    PySSL_END_ALLOW_THREADS

    if (self->ctx == NULL) {
        errstr = ERRSTR("Invalid SSL protocol variant specified.");
        goto fail;
    }

    if (ciphers != NULL) {
        ret = SSL_CTX_set_cipher_list(self->ctx, ciphers);
        if (ret == 0) {
            errstr = ERRSTR("No cipher can be selected.");
            goto fail;
        }
    }

    if (certreq != PY_SSL_CERT_NONE) {
        if (cacerts_file == NULL) {
            errstr = ERRSTR("No root certificates specified for "
                            "verification of other-side certificates.");
            goto fail;
        } else {
            PySSL_BEGIN_ALLOW_THREADS
            ret = SSL_CTX_load_verify_locations(self->ctx,
                                                cacerts_file,
                                                NULL);
            PySSL_END_ALLOW_THREADS
            if (ret != 1) {
                _setSSLError(NULL, 0, __FILE__, __LINE__);
                goto fail;
            }
        }
    }
    if (key_file) {
        PySSL_BEGIN_ALLOW_THREADS
        ret = SSL_CTX_use_PrivateKey_file(self->ctx, key_file,
                                          SSL_FILETYPE_PEM);
        PySSL_END_ALLOW_THREADS
        if (ret != 1) {
            _setSSLError(NULL, ret, __FILE__, __LINE__);
            goto fail;
        }

        PySSL_BEGIN_ALLOW_THREADS
        ret = SSL_CTX_use_certificate_chain_file(self->ctx,
                                                 cert_file);
        PySSL_END_ALLOW_THREADS
        if (ret != 1) {
            /*
            fprintf(stderr, "ret is %d, errcode is %lu, %lu, with file \"%s\"\n",
                ret, ERR_peek_error(), ERR_peek_last_error(), cert_file);
                */
            if (ERR_peek_last_error() != 0) {
                _setSSLError(NULL, ret, __FILE__, __LINE__);
                goto fail;
            }
        }
    }

    /* ssl compatibility */
    options = SSL_OP_ALL & ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS;
    if (proto_version != PY_SSL_VERSION_SSL2)
        options |= SSL_OP_NO_SSLv2;
    SSL_CTX_set_options(self->ctx, options);

    verification_mode = SSL_VERIFY_NONE;
    if (certreq == PY_SSL_CERT_OPTIONAL)
        verification_mode = SSL_VERIFY_PEER;
    else if (certreq == PY_SSL_CERT_REQUIRED)
        verification_mode = (SSL_VERIFY_PEER |
                             SSL_VERIFY_FAIL_IF_NO_PEER_CERT);
    SSL_CTX_set_verify(self->ctx, verification_mode,
                       NULL); /* set verify lvl */

    PySSL_BEGIN_ALLOW_THREADS
    self->ssl = SSL_new(self->ctx); /* New ssl struct */
    PySSL_END_ALLOW_THREADS
    SSL_set_fd(self->ssl, Sock->sock_fd);       /* Set the socket for SSL */
#ifdef SSL_MODE_AUTO_RETRY
    SSL_set_mode(self->ssl, SSL_MODE_AUTO_RETRY);
#endif

    /* If the socket is in non-blocking mode or timeout mode, set the BIO
     * to non-blocking mode (blocking is the default)
     */
    if (Sock->sock_timeout >= 0.0) {
        /* Set both the read and write BIO's to non-blocking mode */
        BIO_set_nbio(SSL_get_rbio(self->ssl), 1);
        BIO_set_nbio(SSL_get_wbio(self->ssl), 1);
    }

    PySSL_BEGIN_ALLOW_THREADS
    if (socket_type == PY_SSL_CLIENT)
        SSL_set_connect_state(self->ssl);
    else
        SSL_set_accept_state(self->ssl);
    PySSL_END_ALLOW_THREADS

    self->Socket = Sock;
    Py_INCREF(self->Socket);
    return self;
 fail:
    if (errstr)
        PyErr_SetString(PySSLErrorObject, errstr);
    Py_DECREF(self);
    return NULL;
}

static PyObject *
PySSL_sslwrap(PyObject *self, PyObject *args)
{
    PySocketSockObject *Sock;
    int server_side = 0;
    int verification_mode = PY_SSL_CERT_NONE;
    int protocol = PY_SSL_VERSION_SSL23;
    char *key_file = NULL;
    char *cert_file = NULL;
    char *cacerts_file = NULL;
    char *ciphers = NULL;

    if (!PyArg_ParseTuple(args, "O!i|zziizz:sslwrap",
                          PySocketModule.Sock_Type,
                          &Sock,
                          &server_side,
                          &key_file, &cert_file,
                          &verification_mode, &protocol,
                          &cacerts_file, &ciphers))
        return NULL;

    /*
    fprintf(stderr,
        "server_side is %d, keyfile %p, certfile %p, verify_mode %d, "
        "protocol %d, certs %p\n",
        server_side, key_file, cert_file, verification_mode,
        protocol, cacerts_file);
     */

    return (PyObject *) newPySSLObject(Sock, key_file, cert_file,
                                       server_side, verification_mode,
                                       protocol, cacerts_file,
                                       ciphers);
}

PyDoc_STRVAR(ssl_doc,
"sslwrap(socket, server_side, [keyfile, certfile, certs_mode, protocol,\n"
"                              cacertsfile, ciphers]) -> sslobject");

/* SSL object methods */

static PyObject *PySSL_SSLdo_handshake(PySSLObject *self)
{
    int ret;
    int err;
    int sockstate, nonblocking;

    /* just in case the blocking state of the socket has been changed */
    nonblocking = (self->Socket->sock_timeout >= 0.0);
    BIO_set_nbio(SSL_get_rbio(self->ssl), nonblocking);
    BIO_set_nbio(SSL_get_wbio(self->ssl), nonblocking);

    /* Actually negotiate SSL connection */
    /* XXX If SSL_do_handshake() returns 0, it's also a failure. */
    do {
        PySSL_BEGIN_ALLOW_THREADS
        ret = SSL_do_handshake(self->ssl);
        err = SSL_get_error(self->ssl, ret);
        PySSL_END_ALLOW_THREADS
        if(PyErr_CheckSignals()) {
            return NULL;
        }
        if (err == SSL_ERROR_WANT_READ) {
            sockstate = check_socket_and_wait_for_timeout(self->Socket, 0);
        } else if (err == SSL_ERROR_WANT_WRITE) {
            sockstate = check_socket_and_wait_for_timeout(self->Socket, 1);
        } else {
            sockstate = SOCKET_OPERATION_OK;
        }
        if (sockstate == SOCKET_HAS_TIMED_OUT) {
            PyErr_SetString(PySSLErrorObject,
                            ERRSTR("The handshake operation timed out"));
            return NULL;
        } else if (sockstate == SOCKET_HAS_BEEN_CLOSED) {
            PyErr_SetString(PySSLErrorObject,
                            ERRSTR("Underlying socket has been closed."));
            return NULL;
        } else if (sockstate == SOCKET_TOO_LARGE_FOR_SELECT) {
            PyErr_SetString(PySSLErrorObject,
                            ERRSTR("Underlying socket too large for select()."));
            return NULL;
        } else if (sockstate == SOCKET_IS_NONBLOCKING) {
            break;
        }
    } while (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE);
    if (ret < 1)
        return PySSL_SetError(self, ret, __FILE__, __LINE__);

    if (self->peer_cert)
        X509_free (self->peer_cert);
    PySSL_BEGIN_ALLOW_THREADS
    if ((self->peer_cert = SSL_get_peer_certificate(self->ssl))) {
        X509_NAME_oneline(X509_get_subject_name(self->peer_cert),
                          self->server, X509_NAME_MAXLEN);
        X509_NAME_oneline(X509_get_issuer_name(self->peer_cert),
                          self->issuer, X509_NAME_MAXLEN);
    }
    PySSL_END_ALLOW_THREADS

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
PySSL_server(PySSLObject *self)
{
    return PyString_FromString(self->server);
}

static PyObject *
PySSL_issuer(PySSLObject *self)
{
    return PyString_FromString(self->issuer);
}

static PyObject *
_create_tuple_for_attribute (ASN1_OBJECT *name, ASN1_STRING *value) {

    char namebuf[X509_NAME_MAXLEN];
    int buflen;
    PyObject *name_obj;
    PyObject *value_obj;
    PyObject *attr;
    unsigned char *valuebuf = NULL;

    buflen = OBJ_obj2txt(namebuf, sizeof(namebuf), name, 0);
    if (buflen < 0) {
        _setSSLError(NULL, 0, __FILE__, __LINE__);
        goto fail;
    }
    name_obj = PyString_FromStringAndSize(namebuf, buflen);
    if (name_obj == NULL)
        goto fail;

    buflen = ASN1_STRING_to_UTF8(&valuebuf, value);
    if (buflen < 0) {
        _setSSLError(NULL, 0, __FILE__, __LINE__);
        Py_DECREF(name_obj);
        goto fail;
    }
    value_obj = PyUnicode_DecodeUTF8((char *) valuebuf,
                                     buflen, "strict");
    OPENSSL_free(valuebuf);
    if (value_obj == NULL) {
        Py_DECREF(name_obj);
        goto fail;
    }
    attr = PyTuple_New(2);
    if (attr == NULL) {
        Py_DECREF(name_obj);
        Py_DECREF(value_obj);
        goto fail;
    }
    PyTuple_SET_ITEM(attr, 0, name_obj);
    PyTuple_SET_ITEM(attr, 1, value_obj);
    return attr;

  fail:
    return NULL;
}

static PyObject *
_create_tuple_for_X509_NAME (X509_NAME *xname)
{
    PyObject *dn = NULL;    /* tuple which represents the "distinguished name" */
    PyObject *rdn = NULL;   /* tuple to hold a "relative distinguished name" */
    PyObject *rdnt;
    PyObject *attr = NULL;   /* tuple to hold an attribute */
    int entry_count = X509_NAME_entry_count(xname);
    X509_NAME_ENTRY *entry;
    ASN1_OBJECT *name;
    ASN1_STRING *value;
    int index_counter;
    int rdn_level = -1;
    int retcode;

    dn = PyList_New(0);
    if (dn == NULL)
        return NULL;
    /* now create another tuple to hold the top-level RDN */
    rdn = PyList_New(0);
    if (rdn == NULL)
        goto fail0;

    for (index_counter = 0;
         index_counter < entry_count;
         index_counter++)
    {
        entry = X509_NAME_get_entry(xname, index_counter);

        /* check to see if we've gotten to a new RDN */
        if (rdn_level >= 0) {
            if (rdn_level != entry->set) {
                /* yes, new RDN */
                /* add old RDN to DN */
                rdnt = PyList_AsTuple(rdn);
                Py_DECREF(rdn);
                if (rdnt == NULL)
                    goto fail0;
                retcode = PyList_Append(dn, rdnt);
                Py_DECREF(rdnt);
                if (retcode < 0)
                    goto fail0;
                /* create new RDN */
                rdn = PyList_New(0);
                if (rdn == NULL)
                    goto fail0;
            }
        }
        rdn_level = entry->set;

        /* now add this attribute to the current RDN */
        name = X509_NAME_ENTRY_get_object(entry);
        value = X509_NAME_ENTRY_get_data(entry);
        attr = _create_tuple_for_attribute(name, value);
        /*
        fprintf(stderr, "RDN level %d, attribute %s: %s\n",
            entry->set,
            PyString_AS_STRING(PyTuple_GET_ITEM(attr, 0)),
            PyString_AS_STRING(PyTuple_GET_ITEM(attr, 1)));
        */
        if (attr == NULL)
            goto fail1;
        retcode = PyList_Append(rdn, attr);
        Py_DECREF(attr);
        if (retcode < 0)
            goto fail1;
    }
    /* now, there's typically a dangling RDN */
    if (rdn != NULL) {
        if (PyList_GET_SIZE(rdn) > 0) {
            rdnt = PyList_AsTuple(rdn);
            Py_DECREF(rdn);
            if (rdnt == NULL)
                goto fail0;
            retcode = PyList_Append(dn, rdnt);
            Py_DECREF(rdnt);
            if (retcode < 0)
                goto fail0;
        }
        else {
            Py_DECREF(rdn);
        }
    }

    /* convert list to tuple */
    rdnt = PyList_AsTuple(dn);
    Py_DECREF(dn);
    if (rdnt == NULL)
        return NULL;
    return rdnt;

  fail1:
    Py_XDECREF(rdn);

  fail0:
    Py_XDECREF(dn);
    return NULL;
}

static PyObject *
_get_peer_alt_names (X509 *certificate) {

    /* this code follows the procedure outlined in
       OpenSSL's crypto/x509v3/v3_prn.c:X509v3_EXT_print()
       function to extract the STACK_OF(GENERAL_NAME),
       then iterates through the stack to add the
       names. */

    int i, j;
    PyObject *peer_alt_names = Py_None;
    PyObject *v = NULL, *t;
    X509_EXTENSION *ext = NULL;
    GENERAL_NAMES *names = NULL;
    GENERAL_NAME *name;
    const X509V3_EXT_METHOD *method;
    BIO *biobuf = NULL;
    char buf[2048];
    char *vptr;
    int len;
    /* Issue #2973: ASN1_item_d2i() API changed in OpenSSL 0.9.6m */
#if OPENSSL_VERSION_NUMBER >= 0x009060dfL
    const unsigned char *p;
#else
    unsigned char *p;
#endif

    if (certificate == NULL)
        return peer_alt_names;

    /* get a memory buffer */
    biobuf = BIO_new(BIO_s_mem());

    i = -1;
    while ((i = X509_get_ext_by_NID(
                    certificate, NID_subject_alt_name, i)) >= 0) {

        if (peer_alt_names == Py_None) {
            peer_alt_names = PyList_New(0);
            if (peer_alt_names == NULL)
                goto fail;
        }

        /* now decode the altName */
        ext = X509_get_ext(certificate, i);
        if(!(method = X509V3_EXT_get(ext))) {
            PyErr_SetString(PySSLErrorObject,
                            ERRSTR("No method for internalizing subjectAltName!"));
            goto fail;
        }

        p = ext->value->data;
        if (method->it)
            names = (GENERAL_NAMES*) (ASN1_item_d2i(NULL,
                                                    &p,
                                                    ext->value->length,
                                                    ASN1_ITEM_ptr(method->it)));
        else
            names = (GENERAL_NAMES*) (method->d2i(NULL,
                                                  &p,
                                                  ext->value->length));

        for(j = 0; j < sk_GENERAL_NAME_num(names); j++) {
            /* get a rendering of each name in the set of names */
            int gntype;
            ASN1_STRING *as = NULL;

            name = sk_GENERAL_NAME_value(names, j);
            gntype = name->type;
            switch (gntype) {
            case GEN_DIRNAME:
                /* we special-case DirName as a tuple of
                   tuples of attributes */

                t = PyTuple_New(2);
                if (t == NULL) {
                    goto fail;
                }

                v = PyString_FromString("DirName");
                if (v == NULL) {
                    Py_DECREF(t);
                    goto fail;
                }
                PyTuple_SET_ITEM(t, 0, v);

                v = _create_tuple_for_X509_NAME (name->d.dirn);
                if (v == NULL) {
                    Py_DECREF(t);
                    goto fail;
                }
                PyTuple_SET_ITEM(t, 1, v);
                break;

            case GEN_EMAIL:
            case GEN_DNS:
            case GEN_URI:
                /* GENERAL_NAME_print() doesn't handle NULL bytes in ASN1_string
                   correctly, CVE-2013-4238 */
                t = PyTuple_New(2);
                if (t == NULL)
                    goto fail;
                switch (gntype) {
                case GEN_EMAIL:
                    v = PyString_FromString("email");
                    as = name->d.rfc822Name;
                    break;
                case GEN_DNS:
                    v = PyString_FromString("DNS");
                    as = name->d.dNSName;
                    break;
                case GEN_URI:
                    v = PyString_FromString("URI");
                    as = name->d.uniformResourceIdentifier;
                    break;
                }
                if (v == NULL) {
                    Py_DECREF(t);
                    goto fail;
                }
                PyTuple_SET_ITEM(t, 0, v);
                v = PyString_FromStringAndSize((char *)ASN1_STRING_data(as),
                                               ASN1_STRING_length(as));
                if (v == NULL) {
                    Py_DECREF(t);
                    goto fail;
                }
                PyTuple_SET_ITEM(t, 1, v);
                break;

            default:
                /* for everything else, we use the OpenSSL print form */
                switch (gntype) {
                    /* check for new general name type */
                    case GEN_OTHERNAME:
                    case GEN_X400:
                    case GEN_EDIPARTY:
                    case GEN_IPADD:
                    case GEN_RID:
                        break;
                    default:
                        if (PyErr_Warn(PyExc_RuntimeWarning,
                                       "Unknown general name type") == -1) {
                            goto fail;
                        }
                        break;
                }
                (void) BIO_reset(biobuf);
                GENERAL_NAME_print(biobuf, name);
                len = BIO_gets(biobuf, buf, sizeof(buf)-1);
                if (len < 0) {
                    _setSSLError(NULL, 0, __FILE__, __LINE__);
                    goto fail;
                }
                vptr = strchr(buf, ':');
                if (vptr == NULL)
                    goto fail;
                t = PyTuple_New(2);
                if (t == NULL)
                    goto fail;
                v = PyString_FromStringAndSize(buf, (vptr - buf));
                if (v == NULL) {
                    Py_DECREF(t);
                    goto fail;
                }
                PyTuple_SET_ITEM(t, 0, v);
                v = PyString_FromStringAndSize((vptr + 1), (len - (vptr - buf + 1)));
                if (v == NULL) {
                    Py_DECREF(t);
                    goto fail;
                }
                PyTuple_SET_ITEM(t, 1, v);
                break;
            }

            /* and add that rendering to the list */

            if (PyList_Append(peer_alt_names, t) < 0) {
                Py_DECREF(t);
                goto fail;
            }
            Py_DECREF(t);
        }
        sk_GENERAL_NAME_pop_free(names, GENERAL_NAME_free);
    }
    BIO_free(biobuf);
    if (peer_alt_names != Py_None) {
        v = PyList_AsTuple(peer_alt_names);
        Py_DECREF(peer_alt_names);
        return v;
    } else {
        return peer_alt_names;
    }


  fail:
    if (biobuf != NULL)
        BIO_free(biobuf);

    if (peer_alt_names != Py_None) {
        Py_XDECREF(peer_alt_names);
    }

    return NULL;
}

static PyObject *
_decode_certificate (X509 *certificate, int verbose) {

    PyObject *retval = NULL;
    BIO *biobuf = NULL;
    PyObject *peer;
    PyObject *peer_alt_names = NULL;
    PyObject *issuer;
    PyObject *version;
    PyObject *sn_obj;
    ASN1_INTEGER *serialNumber;
    char buf[2048];
    int len;
    ASN1_TIME *notBefore, *notAfter;
    PyObject *pnotBefore, *pnotAfter;

    retval = PyDict_New();
    if (retval == NULL)
        return NULL;

    peer = _create_tuple_for_X509_NAME(
        X509_get_subject_name(certificate));
    if (peer == NULL)
        goto fail0;
    if (PyDict_SetItemString(retval, (const char *) "subject", peer) < 0) {
        Py_DECREF(peer);
        goto fail0;
    }
    Py_DECREF(peer);

    if (verbose) {
        issuer = _create_tuple_for_X509_NAME(
            X509_get_issuer_name(certificate));
        if (issuer == NULL)
            goto fail0;
        if (PyDict_SetItemString(retval, (const char *)"issuer", issuer) < 0) {
            Py_DECREF(issuer);
            goto fail0;
        }
        Py_DECREF(issuer);

        version = PyInt_FromLong(X509_get_version(certificate) + 1);
        if (PyDict_SetItemString(retval, "version", version) < 0) {
            Py_DECREF(version);
            goto fail0;
        }
        Py_DECREF(version);
    }

    /* get a memory buffer */
    biobuf = BIO_new(BIO_s_mem());

    if (verbose) {

        (void) BIO_reset(biobuf);
        serialNumber = X509_get_serialNumber(certificate);
        /* should not exceed 20 octets, 160 bits, so buf is big enough */
        i2a_ASN1_INTEGER(biobuf, serialNumber);
        len = BIO_gets(biobuf, buf, sizeof(buf)-1);
        if (len < 0) {
            _setSSLError(NULL, 0, __FILE__, __LINE__);
            goto fail1;
        }
        sn_obj = PyString_FromStringAndSize(buf, len);
        if (sn_obj == NULL)
            goto fail1;
        if (PyDict_SetItemString(retval, "serialNumber", sn_obj) < 0) {
            Py_DECREF(sn_obj);
            goto fail1;
        }
        Py_DECREF(sn_obj);

        (void) BIO_reset(biobuf);
        notBefore = X509_get_notBefore(certificate);
        ASN1_TIME_print(biobuf, notBefore);
        len = BIO_gets(biobuf, buf, sizeof(buf)-1);
        if (len < 0) {
            _setSSLError(NULL, 0, __FILE__, __LINE__);
            goto fail1;
        }
        pnotBefore = PyString_FromStringAndSize(buf, len);
        if (pnotBefore == NULL)
            goto fail1;
        if (PyDict_SetItemString(retval, "notBefore", pnotBefore) < 0) {
            Py_DECREF(pnotBefore);
            goto fail1;
        }
        Py_DECREF(pnotBefore);
    }

    (void) BIO_reset(biobuf);
    notAfter = X509_get_notAfter(certificate);
    ASN1_TIME_print(biobuf, notAfter);
    len = BIO_gets(biobuf, buf, sizeof(buf)-1);
    if (len < 0) {
        _setSSLError(NULL, 0, __FILE__, __LINE__);
        goto fail1;
    }
    pnotAfter = PyString_FromStringAndSize(buf, len);
    if (pnotAfter == NULL)
        goto fail1;
    if (PyDict_SetItemString(retval, "notAfter", pnotAfter) < 0) {
        Py_DECREF(pnotAfter);
        goto fail1;
    }
    Py_DECREF(pnotAfter);

    /* Now look for subjectAltName */

    peer_alt_names = _get_peer_alt_names(certificate);
    if (peer_alt_names == NULL)
        goto fail1;
    else if (peer_alt_names != Py_None) {
        if (PyDict_SetItemString(retval, "subjectAltName",
                                 peer_alt_names) < 0) {
            Py_DECREF(peer_alt_names);
            goto fail1;
        }
        Py_DECREF(peer_alt_names);
    }

    BIO_free(biobuf);
    return retval;

  fail1:
    if (biobuf != NULL)
        BIO_free(biobuf);
  fail0:
    Py_XDECREF(retval);
    return NULL;
}


static PyObject *
PySSL_test_decode_certificate (PyObject *mod, PyObject *args) {

    PyObject *retval = NULL;
    char *filename = NULL;
    X509 *x=NULL;
    BIO *cert;
    int verbose = 1;

    if (!PyArg_ParseTuple(args, "s|i:test_decode_certificate", &filename, &verbose))
        return NULL;

    if ((cert=BIO_new(BIO_s_file())) == NULL) {
        PyErr_SetString(PySSLErrorObject, "Can't malloc memory to read file");
        goto fail0;
    }

    if (BIO_read_filename(cert,filename) <= 0) {
        PyErr_SetString(PySSLErrorObject, "Can't open file");
        goto fail0;
    }

    x = PEM_read_bio_X509_AUX(cert,NULL, NULL, NULL);
    if (x == NULL) {
        PyErr_SetString(PySSLErrorObject, "Error decoding PEM-encoded file");
        goto fail0;
    }

    retval = _decode_certificate(x, verbose);
    X509_free(x);

  fail0:

    if (cert != NULL) BIO_free(cert);
    return retval;
}


static PyObject *
PySSL_peercert(PySSLObject *self, PyObject *args)
{
    PyObject *retval = NULL;
    int len;
    int verification;
    PyObject *binary_mode = Py_None;
    int b;

    if (!PyArg_ParseTuple(args, "|O:peer_certificate", &binary_mode))
        return NULL;

    if (!self->peer_cert)
        Py_RETURN_NONE;

    b = PyObject_IsTrue(binary_mode);
    if (b < 0)
        return NULL;
    if (b) {
        /* return cert in DER-encoded format */

        unsigned char *bytes_buf = NULL;

        bytes_buf = NULL;
        len = i2d_X509(self->peer_cert, &bytes_buf);
        if (len < 0) {
            PySSL_SetError(self, len, __FILE__, __LINE__);
            return NULL;
        }
        retval = PyString_FromStringAndSize((const char *) bytes_buf, len);
        OPENSSL_free(bytes_buf);
        return retval;

    } else {

        verification = SSL_CTX_get_verify_mode(self->ctx);
        if ((verification & SSL_VERIFY_PEER) == 0)
            return PyDict_New();
        else
            return _decode_certificate (self->peer_cert, 0);
    }
}

PyDoc_STRVAR(PySSL_peercert_doc,
"peer_certificate([der=False]) -> certificate\n\
\n\
Returns the certificate for the peer.  If no certificate was provided,\n\
returns None.  If a certificate was provided, but not validated, returns\n\
an empty dictionary.  Otherwise returns a dict containing information\n\
about the peer certificate.\n\
\n\
If the optional argument is True, returns a DER-encoded copy of the\n\
peer certificate, or None if no certificate was provided.  This will\n\
return the certificate even if it wasn't validated.");

static PyObject *PySSL_cipher (PySSLObject *self) {

    PyObject *retval, *v;
    const SSL_CIPHER *current;
    char *cipher_name;
    char *cipher_protocol;

    if (self->ssl == NULL)
        Py_RETURN_NONE;
    current = SSL_get_current_cipher(self->ssl);
    if (current == NULL)
        Py_RETURN_NONE;

    retval = PyTuple_New(3);
    if (retval == NULL)
        return NULL;

    cipher_name = (char *) SSL_CIPHER_get_name(current);
    if (cipher_name == NULL) {
        Py_INCREF(Py_None);
        PyTuple_SET_ITEM(retval, 0, Py_None);
    } else {
        v = PyString_FromString(cipher_name);
        if (v == NULL)
            goto fail0;
        PyTuple_SET_ITEM(retval, 0, v);
    }
    cipher_protocol = SSL_CIPHER_get_version(current);
    if (cipher_protocol == NULL) {
        Py_INCREF(Py_None);
        PyTuple_SET_ITEM(retval, 1, Py_None);
    } else {
        v = PyString_FromString(cipher_protocol);
        if (v == NULL)
            goto fail0;
        PyTuple_SET_ITEM(retval, 1, v);
    }
    v = PyInt_FromLong(SSL_CIPHER_get_bits(current, NULL));
    if (v == NULL)
        goto fail0;
    PyTuple_SET_ITEM(retval, 2, v);
    return retval;

  fail0:
    Py_DECREF(retval);
    return NULL;
}

static void PySSL_dealloc(PySSLObject *self)
{
    if (self->peer_cert)        /* Possible not to have one? */
        X509_free (self->peer_cert);
    if (self->ssl)
        SSL_free(self->ssl);
    if (self->ctx)
        SSL_CTX_free(self->ctx);
    Py_XDECREF(self->Socket);
    PyObject_Del(self);
}

/* If the socket has a timeout, do a select()/poll() on the socket.
   The argument writing indicates the direction.
   Returns one of the possibilities in the timeout_state enum (above).
 */

static int
check_socket_and_wait_for_timeout(PySocketSockObject *s, int writing)
{
    fd_set fds;
    struct timeval tv;
    int rc;

    /* Nothing to do unless we're in timeout mode (not non-blocking) */
    if (s->sock_timeout < 0.0)
        return SOCKET_IS_BLOCKING;
    else if (s->sock_timeout == 0.0)
        return SOCKET_IS_NONBLOCKING;

    /* Guard against closed socket */
    if (s->sock_fd < 0)
        return SOCKET_HAS_BEEN_CLOSED;

    /* Prefer poll, if available, since you can poll() any fd
     * which can't be done with select(). */
#ifdef HAVE_POLL
    {
        struct pollfd pollfd;
        int timeout;

        pollfd.fd = s->sock_fd;
        pollfd.events = writing ? POLLOUT : POLLIN;

        /* s->sock_timeout is in seconds, timeout in ms */
        timeout = (int)(s->sock_timeout * 1000 + 0.5);
        PySSL_BEGIN_ALLOW_THREADS
        rc = poll(&pollfd, 1, timeout);
        PySSL_END_ALLOW_THREADS

        goto normal_return;
    }
#endif

    /* Guard against socket too large for select*/
    if (!_PyIsSelectable_fd(s->sock_fd))
        return SOCKET_TOO_LARGE_FOR_SELECT;

    /* Construct the arguments to select */
    tv.tv_sec = (int)s->sock_timeout;
    tv.tv_usec = (int)((s->sock_timeout - tv.tv_sec) * 1e6);
    FD_ZERO(&fds);
    FD_SET(s->sock_fd, &fds);

    /* See if the socket is ready */
    PySSL_BEGIN_ALLOW_THREADS
    if (writing)
        rc = select(s->sock_fd+1, NULL, &fds, NULL, &tv);
    else
        rc = select(s->sock_fd+1, &fds, NULL, NULL, &tv);
    PySSL_END_ALLOW_THREADS

#ifdef HAVE_POLL
normal_return:
#endif
    /* Return SOCKET_TIMED_OUT on timeout, SOCKET_OPERATION_OK otherwise
       (when we are able to write or when there's something to read) */
    return rc == 0 ? SOCKET_HAS_TIMED_OUT : SOCKET_OPERATION_OK;
}

static PyObject *PySSL_SSLwrite(PySSLObject *self, PyObject *args)
{
    Py_buffer buf;
    int len;
    int sockstate;
    int err;
    int nonblocking;

    if (!PyArg_ParseTuple(args, "s*:write", &buf))
        return NULL;

    if (buf.len > INT_MAX) {
        PyErr_Format(PyExc_OverflowError,
                     "string longer than %d bytes", INT_MAX);
        goto error;
    }

    /* just in case the blocking state of the socket has been changed */
    nonblocking = (self->Socket->sock_timeout >= 0.0);
    BIO_set_nbio(SSL_get_rbio(self->ssl), nonblocking);
    BIO_set_nbio(SSL_get_wbio(self->ssl), nonblocking);

    sockstate = check_socket_and_wait_for_timeout(self->Socket, 1);
    if (sockstate == SOCKET_HAS_TIMED_OUT) {
        PyErr_SetString(PySSLErrorObject,
                        "The write operation timed out");
        goto error;
    } else if (sockstate == SOCKET_HAS_BEEN_CLOSED) {
        PyErr_SetString(PySSLErrorObject,
                        "Underlying socket has been closed.");
        goto error;
    } else if (sockstate == SOCKET_TOO_LARGE_FOR_SELECT) {
        PyErr_SetString(PySSLErrorObject,
                        "Underlying socket too large for select().");
        goto error;
    }
    do {
        PySSL_BEGIN_ALLOW_THREADS
        len = SSL_write(self->ssl, buf.buf, (int)buf.len);
        err = SSL_get_error(self->ssl, len);
        PySSL_END_ALLOW_THREADS
        if (PyErr_CheckSignals()) {
            goto error;
        }
        if (err == SSL_ERROR_WANT_READ) {
            sockstate = check_socket_and_wait_for_timeout(self->Socket, 0);
        } else if (err == SSL_ERROR_WANT_WRITE) {
            sockstate = check_socket_and_wait_for_timeout(self->Socket, 1);
        } else {
            sockstate = SOCKET_OPERATION_OK;
        }
        if (sockstate == SOCKET_HAS_TIMED_OUT) {
            PyErr_SetString(PySSLErrorObject,
                            "The write operation timed out");
            goto error;
        } else if (sockstate == SOCKET_HAS_BEEN_CLOSED) {
            PyErr_SetString(PySSLErrorObject,
                            "Underlying socket has been closed.");
            goto error;
        } else if (sockstate == SOCKET_IS_NONBLOCKING) {
            break;
        }
    } while (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE);

    PyBuffer_Release(&buf);
    if (len > 0)
        return PyInt_FromLong(len);
    else
        return PySSL_SetError(self, len, __FILE__, __LINE__);

error:
    PyBuffer_Release(&buf);
    return NULL;
}

PyDoc_STRVAR(PySSL_SSLwrite_doc,
"write(s) -> len\n\
\n\
Writes the string s into the SSL object.  Returns the number\n\
of bytes written.");

static PyObject *PySSL_SSLpending(PySSLObject *self)
{
    int count = 0;

    PySSL_BEGIN_ALLOW_THREADS
    count = SSL_pending(self->ssl);
    PySSL_END_ALLOW_THREADS
    if (count < 0)
        return PySSL_SetError(self, count, __FILE__, __LINE__);
    else
        return PyInt_FromLong(count);
}

PyDoc_STRVAR(PySSL_SSLpending_doc,
"pending() -> count\n\
\n\
Returns the number of already decrypted bytes available for read,\n\
pending on the connection.\n");

static PyObject *PySSL_SSLread(PySSLObject *self, PyObject *args)
{
    PyObject *buf;
    int count = 0;
    int len = 1024;
    int sockstate;
    int err;
    int nonblocking;

    if (!PyArg_ParseTuple(args, "|i:read", &len))
        return NULL;

    if (!(buf = PyString_FromStringAndSize((char *) 0, len)))
        return NULL;

    /* just in case the blocking state of the socket has been changed */
    nonblocking = (self->Socket->sock_timeout >= 0.0);
    BIO_set_nbio(SSL_get_rbio(self->ssl), nonblocking);
    BIO_set_nbio(SSL_get_wbio(self->ssl), nonblocking);

    /* first check if there are bytes ready to be read */
    PySSL_BEGIN_ALLOW_THREADS
    count = SSL_pending(self->ssl);
    PySSL_END_ALLOW_THREADS

    if (!count) {
        sockstate = check_socket_and_wait_for_timeout(self->Socket, 0);
        if (sockstate == SOCKET_HAS_TIMED_OUT) {
            PyErr_SetString(PySSLErrorObject,
                            "The read operation timed out");
            Py_DECREF(buf);
            return NULL;
        } else if (sockstate == SOCKET_TOO_LARGE_FOR_SELECT) {
            PyErr_SetString(PySSLErrorObject,
                            "Underlying socket too large for select().");
            Py_DECREF(buf);
            return NULL;
        } else if (sockstate == SOCKET_HAS_BEEN_CLOSED) {
            if (SSL_get_shutdown(self->ssl) !=
                SSL_RECEIVED_SHUTDOWN)
            {
                Py_DECREF(buf);
                PyErr_SetString(PySSLErrorObject,
                                "Socket closed without SSL shutdown handshake");
                return NULL;
            } else {
                /* should contain a zero-length string */
                _PyString_Resize(&buf, 0);
                return buf;
            }
        }
    }
    do {
        PySSL_BEGIN_ALLOW_THREADS
        count = SSL_read(self->ssl, PyString_AsString(buf), len);
        err = SSL_get_error(self->ssl, count);
        PySSL_END_ALLOW_THREADS
        if(PyErr_CheckSignals()) {
            Py_DECREF(buf);
            return NULL;
        }
        if (err == SSL_ERROR_WANT_READ) {
            sockstate = check_socket_and_wait_for_timeout(self->Socket, 0);
        } else if (err == SSL_ERROR_WANT_WRITE) {
            sockstate = check_socket_and_wait_for_timeout(self->Socket, 1);
        } else if ((err == SSL_ERROR_ZERO_RETURN) &&
                   (SSL_get_shutdown(self->ssl) ==
                    SSL_RECEIVED_SHUTDOWN))
        {
            _PyString_Resize(&buf, 0);
            return buf;
        } else {
            sockstate = SOCKET_OPERATION_OK;
        }
        if (sockstate == SOCKET_HAS_TIMED_OUT) {
            PyErr_SetString(PySSLErrorObject,
                            "The read operation timed out");
            Py_DECREF(buf);
            return NULL;
        } else if (sockstate == SOCKET_IS_NONBLOCKING) {
            break;
        }
    } while (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE);
    if (count <= 0) {
        Py_DECREF(buf);
        return PySSL_SetError(self, count, __FILE__, __LINE__);
    }
    if (count != len)
        _PyString_Resize(&buf, count);
    return buf;
}

PyDoc_STRVAR(PySSL_SSLread_doc,
"read([len]) -> string\n\
\n\
Read up to len bytes from the SSL socket.");

static PyObject *PySSL_SSLshutdown(PySSLObject *self)
{
    int err, ssl_err, sockstate, nonblocking;
    int zeros = 0;

    /* Guard against closed socket */
    if (self->Socket->sock_fd < 0) {
        PyErr_SetString(PySSLErrorObject,
                        "Underlying socket has been closed.");
        return NULL;
    }

    /* Just in case the blocking state of the socket has been changed */
    nonblocking = (self->Socket->sock_timeout >= 0.0);
    BIO_set_nbio(SSL_get_rbio(self->ssl), nonblocking);
    BIO_set_nbio(SSL_get_wbio(self->ssl), nonblocking);

    while (1) {
        PySSL_BEGIN_ALLOW_THREADS
        /* Disable read-ahead so that unwrap can work correctly.
         * Otherwise OpenSSL might read in too much data,
         * eating clear text data that happens to be
         * transmitted after the SSL shutdown.
         * Should be safe to call repeatedly every time this
         * function is used and the shutdown_seen_zero != 0
         * condition is met.
         */
        if (self->shutdown_seen_zero)
            SSL_set_read_ahead(self->ssl, 0);
        err = SSL_shutdown(self->ssl);
        PySSL_END_ALLOW_THREADS
        /* If err == 1, a secure shutdown with SSL_shutdown() is complete */
        if (err > 0)
            break;
        if (err == 0) {
            /* Don't loop endlessly; instead preserve legacy
               behaviour of trying SSL_shutdown() only twice.
               This looks necessary for OpenSSL < 0.9.8m */
            if (++zeros > 1)
                break;
            /* Shutdown was sent, now try receiving */
            self->shutdown_seen_zero = 1;
            continue;
        }

        /* Possibly retry shutdown until timeout or failure */
        ssl_err = SSL_get_error(self->ssl, err);
        if (ssl_err == SSL_ERROR_WANT_READ)
            sockstate = check_socket_and_wait_for_timeout(self->Socket, 0);
        else if (ssl_err == SSL_ERROR_WANT_WRITE)
            sockstate = check_socket_and_wait_for_timeout(self->Socket, 1);
        else
            break;
        if (sockstate == SOCKET_HAS_TIMED_OUT) {
            if (ssl_err == SSL_ERROR_WANT_READ)
                PyErr_SetString(PySSLErrorObject,
                                "The read operation timed out");
            else
                PyErr_SetString(PySSLErrorObject,
                                "The write operation timed out");
            return NULL;
        }
        else if (sockstate == SOCKET_TOO_LARGE_FOR_SELECT) {
            PyErr_SetString(PySSLErrorObject,
                            "Underlying socket too large for select().");
            return NULL;
        }
        else if (sockstate != SOCKET_OPERATION_OK)
            /* Retain the SSL error code */
            break;
    }

    if (err < 0)
        return PySSL_SetError(self, err, __FILE__, __LINE__);
    else {
        Py_INCREF(self->Socket);
        return (PyObject *) (self->Socket);
    }
}

PyDoc_STRVAR(PySSL_SSLshutdown_doc,
"shutdown(s) -> socket\n\
\n\
Does the SSL shutdown handshake with the remote end, and returns\n\
the underlying socket object.");

static PyMethodDef PySSLMethods[] = {
    {"do_handshake", (PyCFunction)PySSL_SSLdo_handshake, METH_NOARGS},
    {"write", (PyCFunction)PySSL_SSLwrite, METH_VARARGS,
     PySSL_SSLwrite_doc},
    {"read", (PyCFunction)PySSL_SSLread, METH_VARARGS,
     PySSL_SSLread_doc},
    {"pending", (PyCFunction)PySSL_SSLpending, METH_NOARGS,
     PySSL_SSLpending_doc},
    {"server", (PyCFunction)PySSL_server, METH_NOARGS},
    {"issuer", (PyCFunction)PySSL_issuer, METH_NOARGS},
    {"peer_certificate", (PyCFunction)PySSL_peercert, METH_VARARGS,
     PySSL_peercert_doc},
    {"cipher", (PyCFunction)PySSL_cipher, METH_NOARGS},
    {"shutdown", (PyCFunction)PySSL_SSLshutdown, METH_NOARGS,
     PySSL_SSLshutdown_doc},
    {NULL, NULL}
};

static PyObject *PySSL_getattr(PySSLObject *self, char *name)
{
    return Py_FindMethod(PySSLMethods, (PyObject *)self, name);
}

static PyTypeObject PySSL_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "ssl.SSLContext",                   /*tp_name*/
    sizeof(PySSLObject),                /*tp_basicsize*/
    0,                                  /*tp_itemsize*/
    /* methods */
    (destructor)PySSL_dealloc,          /*tp_dealloc*/
    0,                                  /*tp_print*/
    (getattrfunc)PySSL_getattr,         /*tp_getattr*/
    0,                                  /*tp_setattr*/
    0,                                  /*tp_compare*/
    0,                                  /*tp_repr*/
    0,                                  /*tp_as_number*/
    0,                                  /*tp_as_sequence*/
    0,                                  /*tp_as_mapping*/
    0,                                  /*tp_hash*/
};

#ifdef HAVE_OPENSSL_RAND

/* helper routines for seeding the SSL PRNG */
static PyObject *
PySSL_RAND_add(PyObject *self, PyObject *args)
{
    char *buf;
    int len;
    double entropy;

    if (!PyArg_ParseTuple(args, "s#d:RAND_add", &buf, &len, &entropy))
        return NULL;
    RAND_add(buf, len, entropy);
    Py_INCREF(Py_None);
    return Py_None;
}

PyDoc_STRVAR(PySSL_RAND_add_doc,
"RAND_add(string, entropy)\n\
\n\
Mix string into the OpenSSL PRNG state.  entropy (a float) is a lower\n\
bound on the entropy contained in string.  See RFC 1750.");

static PyObject *
PySSL_RAND_status(PyObject *self)
{
    return PyInt_FromLong(RAND_status());
}

PyDoc_STRVAR(PySSL_RAND_status_doc,
"RAND_status() -> 0 or 1\n\
\n\
Returns 1 if the OpenSSL PRNG has been seeded with enough data and 0 if not.\n\
It is necessary to seed the PRNG with RAND_add() on some platforms before\n\
using the ssl() function.");

static PyObject *
PySSL_RAND_egd(PyObject *self, PyObject *arg)
{
    int bytes;

    if (!PyString_Check(arg))
        return PyErr_Format(PyExc_TypeError,
                            "RAND_egd() expected string, found %s",
                            Py_TYPE(arg)->tp_name);
    bytes = RAND_egd(PyString_AS_STRING(arg));
    if (bytes == -1) {
        PyErr_SetString(PySSLErrorObject,
                        "EGD connection failed or EGD did not return "
                        "enough data to seed the PRNG");
        return NULL;
    }
    return PyInt_FromLong(bytes);
}

PyDoc_STRVAR(PySSL_RAND_egd_doc,
"RAND_egd(path) -> bytes\n\
\n\
Queries the entropy gather daemon (EGD) on the socket named by 'path'.\n\
Returns number of bytes read.  Raises SSLError if connection to EGD\n\
fails or if it does not provide enough data to seed PRNG.");

#endif /* HAVE_OPENSSL_RAND */


/* List of functions exported by this module. */

static PyMethodDef PySSL_methods[] = {
    {"sslwrap",             PySSL_sslwrap,
     METH_VARARGS, ssl_doc},
    {"_test_decode_cert",       PySSL_test_decode_certificate,
     METH_VARARGS},
#ifdef HAVE_OPENSSL_RAND
    {"RAND_add",            PySSL_RAND_add, METH_VARARGS,
     PySSL_RAND_add_doc},
    {"RAND_egd",            PySSL_RAND_egd, METH_O,
     PySSL_RAND_egd_doc},
    {"RAND_status",         (PyCFunction)PySSL_RAND_status, METH_NOARGS,
     PySSL_RAND_status_doc},
#endif
    {NULL,                  NULL}            /* Sentinel */
};


#ifdef WITH_THREAD

/* an implementation of OpenSSL threading operations in terms
   of the Python C thread library */

static PyThread_type_lock *_ssl_locks = NULL;

#if OPENSSL_VERSION_NUMBER >= 0x10000000
/* use new CRYPTO_THREADID API. */
static void
_ssl_threadid_callback(CRYPTO_THREADID *id)
{
    CRYPTO_THREADID_set_numeric(id,
                                (unsigned long)PyThread_get_thread_ident());
}
#else
/* deprecated CRYPTO_set_id_callback() API. */
static unsigned long
_ssl_thread_id_function (void) {
    return PyThread_get_thread_ident();
}
#endif

static void _ssl_thread_locking_function (int mode, int n, const char *file, int line) {
    /* this function is needed to perform locking on shared data
       structures. (Note that OpenSSL uses a number of global data
       structures that will be implicitly shared whenever multiple threads
       use OpenSSL.) Multi-threaded applications will crash at random if
       it is not set.

       locking_function() must be able to handle up to CRYPTO_num_locks()
       different mutex locks. It sets the n-th lock if mode & CRYPTO_LOCK, and
       releases it otherwise.

       file and line are the file number of the function setting the
       lock. They can be useful for debugging.
    */

    if ((_ssl_locks == NULL) ||
        (n < 0) || ((unsigned)n >= _ssl_locks_count))
        return;

    if (mode & CRYPTO_LOCK) {
        PyThread_acquire_lock(_ssl_locks[n], 1);
    } else {
        PyThread_release_lock(_ssl_locks[n]);
    }
}

static int _setup_ssl_threads(void) {

    unsigned int i;

    if (_ssl_locks == NULL) {
        _ssl_locks_count = CRYPTO_num_locks();
        _ssl_locks = (PyThread_type_lock *)
            malloc(sizeof(PyThread_type_lock) * _ssl_locks_count);
        if (_ssl_locks == NULL)
            return 0;
        memset(_ssl_locks, 0, sizeof(PyThread_type_lock) * _ssl_locks_count);
        for (i = 0;  i < _ssl_locks_count;  i++) {
            _ssl_locks[i] = PyThread_allocate_lock();
            if (_ssl_locks[i] == NULL) {
                unsigned int j;
                for (j = 0;  j < i;  j++) {
                    PyThread_free_lock(_ssl_locks[j]);
                }
                free(_ssl_locks);
                return 0;
            }
        }
        CRYPTO_set_locking_callback(_ssl_thread_locking_function);
#if OPENSSL_VERSION_NUMBER >= 0x10000000
        CRYPTO_THREADID_set_callback(_ssl_threadid_callback);
#else
        CRYPTO_set_id_callback(_ssl_thread_id_function);
#endif
    }
    return 1;
}

#endif  /* def HAVE_THREAD */

PyDoc_STRVAR(module_doc,
"Implementation module for SSL socket operations.  See the socket module\n\
for documentation.");

PyMODINIT_FUNC
init_ssl(void)
{
    PyObject *m, *d, *r;
    unsigned long libver;
    unsigned int major, minor, fix, patch, status;

    Py_TYPE(&PySSL_Type) = &PyType_Type;

    // Pyston change
    PyType_Ready(&PySSL_Type);

    m = Py_InitModule3("_ssl", PySSL_methods, module_doc);
    if (m == NULL)
        return;
    d = PyModule_GetDict(m);

    /* Load _socket module and its C API */
    if (PySocketModule_ImportModuleAndAPI())
        return;

    /* Init OpenSSL */
    SSL_load_error_strings();
    SSL_library_init();
#ifdef WITH_THREAD
    /* note that this will start threading if not already started */
    if (!_setup_ssl_threads()) {
        return;
    }
#endif
    OpenSSL_add_all_algorithms();

    /* Add symbols to module dict */
    PySSLErrorObject = PyGC_AddRoot(PyErr_NewException("ssl.SSLError",
                                          PySocketModule.error,
                                          NULL));
    if (PySSLErrorObject == NULL)
        return;
    if (PyDict_SetItemString(d, "SSLError", PySSLErrorObject) != 0)
        return;
    if (PyDict_SetItemString(d, "SSLType",
                             (PyObject *)&PySSL_Type) != 0)
        return;
    PyModule_AddIntConstant(m, "SSL_ERROR_ZERO_RETURN",
                            PY_SSL_ERROR_ZERO_RETURN);
    PyModule_AddIntConstant(m, "SSL_ERROR_WANT_READ",
                            PY_SSL_ERROR_WANT_READ);
    PyModule_AddIntConstant(m, "SSL_ERROR_WANT_WRITE",
                            PY_SSL_ERROR_WANT_WRITE);
    PyModule_AddIntConstant(m, "SSL_ERROR_WANT_X509_LOOKUP",
                            PY_SSL_ERROR_WANT_X509_LOOKUP);
    PyModule_AddIntConstant(m, "SSL_ERROR_SYSCALL",
                            PY_SSL_ERROR_SYSCALL);
    PyModule_AddIntConstant(m, "SSL_ERROR_SSL",
                            PY_SSL_ERROR_SSL);
    PyModule_AddIntConstant(m, "SSL_ERROR_WANT_CONNECT",
                            PY_SSL_ERROR_WANT_CONNECT);
    /* non ssl.h errorcodes */
    PyModule_AddIntConstant(m, "SSL_ERROR_EOF",
                            PY_SSL_ERROR_EOF);
    PyModule_AddIntConstant(m, "SSL_ERROR_INVALID_ERROR_CODE",
                            PY_SSL_ERROR_INVALID_ERROR_CODE);
    /* cert requirements */
    PyModule_AddIntConstant(m, "CERT_NONE",
                            PY_SSL_CERT_NONE);
    PyModule_AddIntConstant(m, "CERT_OPTIONAL",
                            PY_SSL_CERT_OPTIONAL);
    PyModule_AddIntConstant(m, "CERT_REQUIRED",
                            PY_SSL_CERT_REQUIRED);

    /* protocol versions */
#ifndef OPENSSL_NO_SSL2
    PyModule_AddIntConstant(m, "PROTOCOL_SSLv2",
                            PY_SSL_VERSION_SSL2);
#endif
    PyModule_AddIntConstant(m, "PROTOCOL_SSLv3",
                            PY_SSL_VERSION_SSL3);
    PyModule_AddIntConstant(m, "PROTOCOL_SSLv23",
                            PY_SSL_VERSION_SSL23);
    PyModule_AddIntConstant(m, "PROTOCOL_TLSv1",
                            PY_SSL_VERSION_TLS1);

    /* OpenSSL version */
    /* SSLeay() gives us the version of the library linked against,
       which could be different from the headers version.
    */
    libver = SSLeay();
    r = PyLong_FromUnsignedLong(libver);
    if (r == NULL)
        return;
    if (PyModule_AddObject(m, "OPENSSL_VERSION_NUMBER", r))
        return;
    status = libver & 0xF;
    libver >>= 4;
    patch = libver & 0xFF;
    libver >>= 8;
    fix = libver & 0xFF;
    libver >>= 8;
    minor = libver & 0xFF;
    libver >>= 8;
    major = libver & 0xFF;
    r = Py_BuildValue("IIIII", major, minor, fix, patch, status);
    if (r == NULL || PyModule_AddObject(m, "OPENSSL_VERSION_INFO", r))
        return;
    r = PyString_FromString(SSLeay_version(SSLEAY_VERSION));
    if (r == NULL || PyModule_AddObject(m, "OPENSSL_VERSION", r))
        return;

}
