/*

 yappi
 Yet Another Python Profiler

 Sumer Cip 2012

*/

#include "config.h"

#if !defined(HAVE_LONG_LONG)
#error "Yappi requires long longs!"
#endif

#ifdef IS_PY3K
#include "bytesobject.h"
#endif
#include "frameobject.h"
#include "callstack.h"
#include "hashtab.h"
#include "debug.h"
#include "timing.h"
#include "freelist.h"
#include "mem.h"

#ifdef IS_PY3K
PyDoc_STRVAR(_yappi__doc__, "Yet Another Python Profiler");
#endif

// linked list for holding callee/caller info in the pit
// we need to record the timing data on the pairs (parent, child) only index is not enough
typedef struct {
    unsigned int index;
    unsigned long callcount;
    unsigned long nonrecursive_callcount;   // the number of non-recursive calls.
    unsigned long recursionlevel;           // holds the current recursion level.
    long long tsubtotal;                    // the time that the child function spent in its children
    long long ttotal;                       // the total time that the child function spent
    struct _pit_children_info *next;
} _pit_children_info;

// module definitions
typedef struct {
    PyObject *name;
    PyObject *modname;
    unsigned long lineno;
    unsigned long callcount;
    unsigned long nonrecursive_callcount;   // the number of actual calls when the function is recursive.
    unsigned long recursionlevel;           // holds the current recursion level.
    long long tsubtotal;                    // time function spent in its children (excluding recursive calls)
    long long ttotal;                       // the total time that a function spent
    unsigned int builtin;                   // 0 for normal, 1 for ccall
    unsigned int index;    
    _pit_children_info *children;
} _pit; // profile_item

typedef struct {
    _cstack *cs;
    long id;
    _pit *last_pit;
    unsigned long sched_cnt;
    char *class_name;
    long long t0; // profiling start CPU time
} _ctx; // context

typedef struct {
    int builtins;
    int multithreaded;
} _flag; // flags passed from yappi.start()

// globals
static PyObject *YappiProfileError;
static _htab *contexts;
static _htab *pits;
static _flag flags;
static _freelist *flpit;
static _freelist *flctx;
static int yappinitialized;
static unsigned int ycurfuncindex; // used for providing unique index for functions
static int yapphavestats;	// start() called at least once or stats cleared?
static int yapprunning;
static time_t yappstarttime;
static long long yappstarttick;
static long long yappstoptick;
static _ctx *prev_ctx;
static _ctx *current_ctx;
static PyObject *test_timings; // used for testing

// defines
#define UNINITIALIZED_STRING_VAL "N/A"

// forwards
static _ctx * _profile_thread(PyThreadState *ts);


// string formatting helper functions compatible with with both 2.x and 3.x
#ifdef IS_PY3K
#define PyStr_AS_CSTRING(s) PyBytes_AS_STRING(PyUnicode_AsUTF8String(s))
#else
#define PyStr_AS_CSTRING(s) PyString_AS_STRING(s)
#endif

static PyObject * 
PyStr_FromFormat(const char *fmt, ...)
{
    PyObject* ret;
    va_list vargs;
    
    va_start(vargs, fmt);
#ifdef IS_PY3K
    ret = PyUnicode_FromFormatV(fmt, vargs);
#else
    ret = PyString_FromFormatV(fmt, vargs);
#endif
    return ret;
}

static PyObject * 
PyStr_FromString(const char *s)
{
    PyObject* ret;
    
#ifdef IS_PY3K
    ret = PyUnicode_FromString(s);
#else
    ret = PyString_FromString(s);
#endif
    return ret;
}



// module functions
static _pit *
_create_pit(void)
{
    _pit *pit;

    pit = flget(flpit);
    if (!pit)
        return NULL;
    pit->callcount = 0;
    pit->nonrecursive_callcount = 0;
    pit->ttotal = 0;
    pit->tsubtotal = 0;
    pit->name = NULL;
    pit->modname = NULL;
    pit->lineno = 0;
    pit->builtin = 0;
    pit->index = ycurfuncindex++;
    pit->children = NULL;

    return pit;
}

static _ctx *
_create_ctx(void)
{
    _ctx *ctx;

    ctx = flget(flctx);
    if (!ctx)
        return NULL;
    ctx->cs = screate(100);
    if (!ctx->cs)
        return NULL;
    ctx->last_pit = NULL;
    ctx->sched_cnt = 0;
    ctx->id = 0;
    ctx->class_name = NULL;
    ctx->t0 = tickcount();
    return ctx;
}

char *
_get_current_thread_class_name(void)
{
    PyObject *mthr, *cthr, *tattr1, *tattr2;

    mthr = cthr = tattr1 = tattr2 = NULL;

    mthr = PyImport_ImportModule("threading");
    if (!mthr)
        goto err;
    cthr = PyObject_CallMethod(mthr, "currentThread", "");
    if (!cthr)
        goto err;
    tattr1 = PyObject_GetAttrString(cthr, "__class__");
    if (!tattr1)
        goto err;
    tattr2 = PyObject_GetAttrString(tattr1, "__name__");
    if (!tattr2)
        goto err;

    return PyStr_AS_CSTRING(tattr2);

err:
    Py_XDECREF(mthr);
    Py_XDECREF(cthr);
    Py_XDECREF(tattr1);
    Py_XDECREF(tattr2);
    return NULL; //continue enumeration on err.
}

static _ctx *
_thread2ctx(PyThreadState *ts)
{
    _hitem *it;

    it = hfind(contexts, (uintptr_t)ts->thread_id);
    if (!it) {
        // callback functions in some circumtances, can be called before the context entry is not
        // created. (See issue 21). To prevent this problem we need to ensure the context entry for
        // the thread is always available here.
        return _profile_thread(ts);
    }
    return (_ctx *)it->val;
}

// the pit will be cleared by the relevant freelist. we do not free it here.
// we only DECREF the CodeObject or the MethodDescriptive string.
static void
_del_pit(_pit *pit)
{
    _pit_children_info *it,*next;
    it = pit->children;
    while(it) {
        next = (_pit_children_info *)it->next;
        yfree(it);
        it = next;
    }
    pit->children = NULL;
    Py_XDECREF(pit->name);
    Py_XDECREF(pit->modname);
}

static _pit *
_ccode2pit(void *cco)
{
    PyCFunctionObject *cfn;
    _hitem *it;
    PyObject *mod;
    char *modname;
    PyObject *name;

    cfn = cco;
    // Issue #15:
    // Hashing cfn to the pits table causes different object methods
    // to be hashed into the same slot. Use cfn->m_ml for hashing the
    // Python C functions.
    it = hfind(pits, (uintptr_t)cfn->m_ml);
    if (!it) {
        _pit *pit = _create_pit();
        if (!pit)
            return NULL;
        if (!hadd(pits, (uintptr_t)cfn->m_ml, (uintptr_t)pit))
            return NULL;

        pit->builtin = 1;

        // get module name
        modname = NULL;
        mod = cfn->m_module;
#ifdef IS_PY3K
        if (mod && PyUnicode_Check(mod)) {      
#else
        if (mod && PyString_Check(mod)) {
#endif
            modname = PyStr_AS_CSTRING(mod);
        } else if (mod && PyModule_Check(mod)) {
            modname = (char *)PyModule_GetName(mod);
        } 
        
        if (modname == NULL) {
            PyErr_Clear();
            modname = "__builtin__";
        }
        
        pit->modname = PyStr_FromString(modname);
        pit->lineno = 0;
        
        // built-in method?
        if (cfn->m_self != NULL) {
            name = PyStr_FromString(cfn->m_ml->ml_name);
            if (name != NULL) {
                PyObject *mo = _PyType_Lookup((PyTypeObject *)PyObject_Type(cfn->m_self), name);
                Py_XINCREF(mo);
                Py_DECREF(name);
                if (mo != NULL) {
                    pit->name = PyObject_Repr(mo);
                    Py_DECREF(mo);
                    return pit;
                }
            }
            PyErr_Clear();
        }
        pit->name = PyStr_FromString(cfn->m_ml->ml_name);
        return pit;
    }
    return ((_pit *)it->val);
}

// maps the PyCodeObject to our internal pit item via hash table.
static _pit *
_code2pit(PyFrameObject *fobj)
{
    _hitem *it;
    PyCodeObject *cobj;
    _pit *pit;

    cobj = fobj->f_code;
    it = hfind(pits, (uintptr_t)cobj);
    if (it) {
        return ((_pit *)it->val);
    }

    pit = _create_pit();
    if (!pit)
        return NULL;
    if (!hadd(pits, (uintptr_t)cobj, (uintptr_t)pit))
        return NULL;

    pit->name = NULL;
    pit->modname = PyStr_FromString(PyStr_AS_CSTRING(cobj->co_filename));    
    pit->lineno = cobj->co_firstlineno;

    PyFrame_FastToLocals(fobj);
    if (cobj->co_argcount) {
        const char *firstarg = PyStr_AS_CSTRING(PyTuple_GET_ITEM(cobj->co_varnames, 0));

        if (!strcmp(firstarg, "self")) {
            PyObject* locals = fobj->f_locals;
            if (locals) {
                PyObject* self = PyDict_GetItemString(locals, "self");
                if (self) {
                    PyObject *as = PyObject_GetAttrString(self, "__class__");
                    as = PyObject_GetAttrString(as, "__name__");                    
                    pit->name = PyStr_FromFormat("%s.%s", PyStr_AS_CSTRING(as), PyStr_AS_CSTRING(cobj->co_name));
                }
            }
        }
    }
    if (!pit->name) {
        pit->name = PyStr_FromString(PyStr_AS_CSTRING(cobj->co_name));
    }

    PyFrame_LocalsToFast(fobj, 0);

    return pit;
}

static _pit *
_get_parent(void)
{
    _cstackitem *pi;
    pi = shead(current_ctx->cs);
    if (!pi) {
        return NULL;
    }
    return pi->ckey;
}

static _pit *
_pop_parent(void)
{
    _cstackitem *pi;
    pi = spop(current_ctx->cs);
    if (!pi) {
        return NULL;
    }
    return pi->ckey;
}

static _pit_children_info *
_get_child_info(_pit *parent, _pit *current)
{
    _pit_children_info *citem;
    
    citem = parent->children;    
    while(citem) { 
        if (citem->index == current->index) {
            break;
        }
        citem = (_pit_children_info *)citem->next;
    }
    return citem;
}

static _pit_children_info *
_add_child_info(_pit *parent, _pit *current)
{
    _pit_children_info *newci,*cit;
  
    newci = ymalloc(sizeof(_pit_children_info));
    newci->index = current->index;
    newci->callcount = 0;
    newci->nonrecursive_callcount = 0;
    newci->recursionlevel = 0;
    newci->ttotal = 0;
    newci->tsubtotal = 0;
    newci->next = NULL;
    if (!parent->children) {
        parent->children = newci;
    }
    else {
        // go till end
        cit = parent->children;
        while(cit->next) {
            cit = (_pit_children_info *)cit->next;
        }
        cit->next = (struct _pit_children_info *)newci;
    }
    
    return newci;
}

static void
_call_enter(PyObject *self, PyFrameObject *frame, PyObject *arg, int ccall)
{
    _pit *cp,*pp;
    PyObject *last_type, *last_value, *last_tb;
    _cstackitem *hci,*pi;
    _pit_children_info *pci;
    
    PyErr_Fetch(&last_type, &last_value, &last_tb);

    if (ccall) {
        cp = _ccode2pit((PyCFunctionObject *)arg);
    } else {
        cp = _code2pit(frame);
    }

    // something went wrong. No mem, or another error. we cannot find
    // a corresponding pit. just run away:)
    if (!cp) {
        yerr("pit not found"); // (defensive)
        goto err;
    }
    
    // create/update children info if we have a valid parent
    pi = shead(current_ctx->cs);
    if (pi) {
        pp = pi->ckey;
        pci = _get_child_info(pp, cp);    
        if(!pci)
        {
            pci = _add_child_info(pp, cp);
        }    
        pci->callcount++;
        pci->recursionlevel++;
    }
    
    hci = spush(current_ctx->cs, cp);
    if (!hci) { // runaway! (defensive)
        yerr("spush failed .");
        goto err;
    }

    hci->t0 = tickcount();
    cp->callcount++;
    cp->recursionlevel++;
    
    // do not show builtin pits if specified even in last_pit of the context.
    if  ((!flags.builtins) && (cp->builtin))
        ;
    else {
        current_ctx->last_pit = cp;
    }
    
    PyErr_Restore(last_type, last_value, last_tb);

err:

    PyErr_Restore(last_type, last_value, last_tb);

}

static void
_call_leave(PyObject *self, PyFrameObject *frame, PyObject *arg, int ccall)
{
    _pit *cp, *pp;
    _pit_children_info *pci;
    _cstackitem *ci;
    long long elapsed;
    PyObject *tval;

    ci = spop(current_ctx->cs);
    if (!ci) {
        return; // leaving a frame while callstack is empty, return silently for this.
    }
    cp = ci->ckey;
    
    if (test_timings) { // testing?
        tval = PyDict_GetItem(test_timings, PyStr_FromFormat("%s_%d", PyStr_AS_CSTRING(cp->name), 
            cp->callcount));
        cp->callcount--;
        if (tval) {
            elapsed = PyLong_AsLong(tval) * 10000000; // PyInt_AsLong for 2.x            
        } else {
            elapsed = 30000000;
        }
    } else {
        elapsed = tickcount() - ci->t0;
    }
    
    // is this the last function in the callstack?
    pp = _pop_parent();
    if (!pp) {
        cp->ttotal += elapsed;
        cp->nonrecursive_callcount++;
        return;
    }
    
    // get children info
    pci = _get_child_info(pp, cp);    
    if(!pci)
    {
        yerr("possible callstack corruption.#1"); //defensive
        return;
    }
    
    // are we leaving a function that is already in the callstack, in other words recursive?
    // then wait for the top-level one to calculate ttot.
    if (cp->recursionlevel == 1) {
        cp->ttotal += elapsed;
        cp->nonrecursive_callcount++;
        pci->nonrecursive_callcount++;
    }
    // wait for the top-level parent to be active to set tsub.
    if (cp->recursionlevel == 1) {
        pp->tsubtotal += elapsed;        
    }
    
    // is this parent-child occurs more than once in the callstack?
    if (pci->recursionlevel == 1) {
        pci->ttotal += elapsed;
        //ppci->tsubtotal += elapsed;
    }
    
    pci->recursionlevel--;
    cp->recursionlevel--;
    
    ci = spush(current_ctx->cs, pp);
    if (!ci) {
        yerr("spush failed #2."); // defensive
        return;
    }
}

// context will be cleared by the free list. we do not free it here.
// we only free the context call stack.
static void
_del_ctx(_ctx * ctx)
{
    sdestroy(ctx->cs);
}

static int
_yapp_callback(PyObject *self, PyFrameObject *frame, int what,
               PyObject *arg)
{
    // get current ctx
    current_ctx = _thread2ctx(frame->f_tstate);
    if (!current_ctx) {
        yerr("no context found or can be created.");
        return 0;
    }
    if (!current_ctx->class_name)
    {
        current_ctx->class_name = _get_current_thread_class_name();
    }

    switch (what) {
    case PyTrace_CALL:
        _call_enter(self, frame, arg, 0);
        break;
    case PyTrace_RETURN: // either normally or with an exception
        _call_leave(self, frame, arg, 0);
        break;

#ifdef PyTrace_C_CALL	// not defined in Python <= 2.3
    case PyTrace_C_CALL:
        if (PyCFunction_Check(arg))
            _call_enter(self, frame, arg, 1); // set ccall to true
        break;

    case PyTrace_C_RETURN:
    case PyTrace_C_EXCEPTION:
        if (PyCFunction_Check(arg))
            _call_leave(self, frame, arg, 1);
        break;
#endif
    default:
        break;
    }

    // update ctx statistics
    if (prev_ctx != current_ctx) {
        current_ctx->sched_cnt++;
    }
    prev_ctx = current_ctx;
    return 0;
}


static _ctx *
_profile_thread(PyThreadState *ts)
{
    _ctx *ctx;

    ctx = _create_ctx();
    if (!ctx) {
        return NULL;
    }

    // Use thread_id instead of ts pointer, because when we create/delete many threads, some
    // of them do not show up in the thread_stats, because ts pointers are recycled in the VM.
    // Also, we do not want to delete thread stats unless clear_stats() is called explicitly.
    // We rely on the OS to give us unique thread ids, this time.
    // thread_id -> long
    if (!hadd(contexts, (uintptr_t)ts->thread_id, (uintptr_t)ctx)) {
        _del_ctx(ctx);
        if (!flput(flctx, ctx)) {
            yerr("Context cannot be recycled. Possible memory leak.");
        }
        ydprintf("Context add failed. Already added?(%p, %ld)", ts,
                PyThreadState_GET()->thread_id);
        return NULL;
    }

    ts->use_tracing = 1;
    ts->c_profilefunc = _yapp_callback;
    ctx->id = ts->thread_id;
    return ctx;
}

static _ctx*
_unprofile_thread(PyThreadState *ts)
{
    ts->use_tracing = 0;
    ts->c_profilefunc = NULL;
    return NULL; //dummy return for enum_threads() func. prototype
}

static void
_ensure_thread_profiled(PyThreadState *ts)
{
    PyThreadState *p = NULL;

    for (p=ts->interp->tstate_head ; p != NULL; p = p->next) {
        if (ts->c_profilefunc != _yapp_callback) {
            _profile_thread(ts);
        }
    }
}

static void
_enum_threads(_ctx* (*f) (PyThreadState *))
{
    PyThreadState *p = NULL;

    for (p=PyThreadState_GET()->interp->tstate_head ; p != NULL; p = p->next) {
        f(p);
    }
}

static int
_init_profiler(void)
{
    // already initialized? only after clear_stats() and first time, this flag
    // will be unset.
    if (!yappinitialized) {
        contexts = htcreate(HT_CTX_SIZE);
        if (!contexts)
            return 0;
        pits = htcreate(HT_PIT_SIZE);
        if (!pits)
            return 0;
        flpit = flcreate(sizeof(_pit), FL_PIT_SIZE);
        if (!flpit)
            return 0;
        flctx = flcreate(sizeof(_ctx), FL_CTX_SIZE);
        if (!flctx)
            return 0;
        yappinitialized = 1;
        current_ctx = NULL;
        prev_ctx = NULL;
        ycurfuncindex = 0;
    }
    return 1;
}

static PyObject*
profile_event(PyObject *self, PyObject *args)
{
    char *ev;
    PyObject *arg;
    PyObject *event;
    PyFrameObject * frame;

    if (!PyArg_ParseTuple(args, "OOO", &frame, &event, &arg)) {
        return NULL;
    }
    
    if (flags.multithreaded) {
        _ensure_thread_profiled(PyThreadState_GET());
    }
    
    ev = PyStr_AS_CSTRING(event);

    if (strcmp("call", ev)==0)
        _yapp_callback(self, frame, PyTrace_CALL, arg);
    else if (strcmp("return", ev)==0)
        _yapp_callback(self, frame, PyTrace_RETURN, arg);
    else if (strcmp("c_call", ev)==0)
        _yapp_callback(self, frame, PyTrace_C_CALL, arg);
    else if (strcmp("c_return", ev)==0)
        _yapp_callback(self, frame, PyTrace_C_RETURN, arg);
    else if (strcmp("c_exception", ev)==0)
        _yapp_callback(self, frame, PyTrace_C_EXCEPTION, arg);

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject*
start(PyObject *self, PyObject *args)
{
    if (yapprunning) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    if (!PyArg_ParseTuple(args, "ii", &flags.builtins, &flags.multithreaded))
        return NULL;

    if (!_init_profiler()) {
        PyErr_SetString(YappiProfileError, "profiler cannot be initialized.");
        return NULL;
    }
    
    if (flags.multithreaded) {
        _enum_threads(&_profile_thread);
    } else {
        _ensure_thread_profiled(PyThreadState_GET());
    }

    yapprunning = 1;
    yapphavestats = 1;
    time (&yappstarttime);
    yappstarttick = tickcount();

    Py_INCREF(Py_None);
    return Py_None;
}


static long long
_calc_cumdiff(long long a, long long b)
{
    long long r;

    r = a - b;
    if (r < 0)
        return 0;
    return r;
}

static int
_pitenumdel(_hitem *item, void *arg)
{
    _del_pit((_pit *)item->val);
    return 0;
}

static int
_ctxenumdel(_hitem *item, void *arg)
{
    _del_ctx(((_ctx *)item->val) );
    return 0;
}

static PyObject*
stop(PyObject *self, PyObject *args)
{
    if (!yapprunning) {
        Py_INCREF(Py_None);
        return Py_None;
    }

    _enum_threads(&_unprofile_thread);

    yapprunning = 0;
    yappstoptick = tickcount();

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject*
clear_stats(PyObject *self, PyObject *args)
{
    if (yapprunning) {
        PyErr_SetString(YappiProfileError, "clear_stats cannot be called while profiler is running.");
        return NULL;
    }

    henum(pits, _pitenumdel, NULL);
    htdestroy(pits);
    henum(contexts, _ctxenumdel, NULL);
    htdestroy(contexts);

    fldestroy(flpit);
    fldestroy(flctx);
    yappinitialized = 0;
    yapphavestats = 0;
    

// check for mem leaks if DEBUG_MEM is specified
#ifdef DEBUG_MEM
    YMEMLEAKCHECK();
#endif

    Py_INCREF(Py_None);
    return Py_None;
}

static int
_ctxenumstat(_hitem *item, void *arg)
{
    PyObject *efn;
    char *tcname;
    _ctx *ctx;
    long long cumdiff;
    PyObject *exc;
    PyObject *last_func_name;
    PyObject *last_mod_name;
    unsigned long last_line_no;
    unsigned int last_builtin;

    ctx = (_ctx *)item->val;

    if(ctx->last_pit) {
        last_func_name = ctx->last_pit->name;
        last_mod_name = ctx->last_pit->modname;
        last_line_no = ctx->last_pit->lineno;
        last_builtin = ctx->last_pit->builtin;
    } else {
        last_func_name = PyStr_FromString(UNINITIALIZED_STRING_VAL);
        last_mod_name = PyStr_FromString(UNINITIALIZED_STRING_VAL);
        last_line_no = 0;
        last_builtin = 0;
    }

    tcname = ctx->class_name;
    if (tcname == NULL)
        tcname = UNINITIALIZED_STRING_VAL;
    efn = (PyObject *)arg;

    cumdiff = _calc_cumdiff(tickcount(), ctx->t0);
    
    exc = PyObject_CallFunction(efn, "((skOOkIfk))", tcname, ctx->id, last_func_name,
        last_mod_name, last_line_no, last_builtin, cumdiff * tickfactor(), ctx->sched_cnt);
    if (!exc) {
        PyErr_Print();
        return 1; // abort enumeration
    }

    return 0;

}

static PyObject*
enum_thread_stats(PyObject *self, PyObject *args)
{
    PyObject *enumfn;

    if (!yapphavestats) {
        PyErr_SetString(YappiProfileError, "profiler not started?");
        return NULL;
    }

    if (!PyArg_ParseTuple(args, "O", &enumfn)) {
        PyErr_SetString(YappiProfileError, "invalid param to enum_thread_stats");
        return NULL;
    }

    if (!PyCallable_Check(enumfn)) {
        PyErr_SetString(YappiProfileError, "enum function must be callable");
        return NULL;
    }

    henum(contexts, _ctxenumstat, enumfn);

    Py_INCREF(Py_None);
    return Py_None;
}


static int
_pitenumstat(_hitem *item, void * arg)
{
    long long cumdiff, child_cumdiff;
    PyObject *efn;
    _pit *pt;
    PyObject *exc;
    PyObject *children;
    _pit_children_info *pci;

    pt = (_pit *)item->val;
    // do not show builtin pits if specified
    if  ((!flags.builtins) && (pt->builtin)) {
        return 0;
    }

    cumdiff = _calc_cumdiff(pt->ttotal, pt->tsubtotal);
    efn = (PyObject *)arg;

    // convert children function index list to PyList
    children = PyList_New(0);
    pci = pt->children;
    while(pci) {
        child_cumdiff = _calc_cumdiff(pci->ttotal, pci->tsubtotal);
        PyList_Append(children, Py_BuildValue("Ikkff", pci->index, pci->callcount, pci->nonrecursive_callcount,
                                              pci->ttotal * tickfactor(), child_cumdiff * tickfactor()));
        pci = (_pit_children_info *)pci->next;
    }
    exc = PyObject_CallFunction(efn, "((OOkkkIffIO))", pt->name, pt->modname, pt->lineno, pt->callcount,
                        pt->nonrecursive_callcount, pt->builtin, pt->ttotal * tickfactor(), cumdiff * tickfactor(), 
                        pt->index, children);
    if (!exc) {
        PyErr_Print();
        return 1; // abort enumeration
    }
    return 0;
}

static PyObject*
enum_func_stats(PyObject *self, PyObject *args)
{
    PyObject *enumfn;

    if (!yapphavestats) {
        PyErr_SetString(YappiProfileError, "profiler not started?");
        return NULL;
    }

    if (!PyArg_ParseTuple(args, "O", &enumfn)) {
        PyErr_SetString(YappiProfileError, "invalid param to enum_func_stats");
        return NULL;
    }

    if (!PyCallable_Check(enumfn)) {
        PyErr_SetString(YappiProfileError, "enum function must be callable");
        return NULL;
    }

    henum(pits, _pitenumstat, enumfn);

    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
is_running(PyObject *self, PyObject *args)
{
    return Py_BuildValue("i", yapprunning);
}

static PyObject *
mem_usage(PyObject *self, PyObject *args)
{
    return Py_BuildValue("l", ymemusage());
}

static PyObject *
set_timings(PyObject *self, PyObject *args)
{
    if (!PyArg_ParseTuple(args, "O", &test_timings)) {
        return NULL;
    }
    
    if (!PyDict_Check(test_timings))
    {
        PyErr_SetString(YappiProfileError, "timings should be dict.");
        return NULL;
    }
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
set_clock_type(PyObject *self, PyObject *args)
{
    int clock_type;
    
    if (!PyArg_ParseTuple(args, "i", &clock_type)) {
        return NULL;
    }
    
    // return silently if same clock_type
    if (clock_type == get_timing_clock_type())
    {
        Py_INCREF(Py_None);
        return Py_None;
    }
    
    if (yapphavestats) {
        PyErr_SetString(YappiProfileError, "clock type cannot be changed previous stats are available. clear the stats first.");
        return NULL;
    }
    
    if (!set_timing_clock_type(clock_type)) {
        PyErr_SetString(YappiProfileError, "Invalid clock type.");
        return NULL;
    }   
    
    Py_INCREF(Py_None);
    return Py_None;
}

static PyObject *
get_clock_type(PyObject *self, PyObject *args)
{
    PyObject *type,*api,*result,*resolution;
    clock_type_t clk_type;

    result = PyDict_New();
    
    clk_type = get_timing_clock_type();
    if (clk_type == WALL_CLOCK) {
        type = Py_BuildValue("s", "wall");
#if defined(_WINDOWS)
        api = Py_BuildValue("s", "queryperformancecounter");
        resolution = Py_BuildValue("s", "100ns");    
#else
        api = Py_BuildValue("s", "gettimeofday");
        resolution = Py_BuildValue("s", "100ns");
#endif
    }  else {
        type = Py_BuildValue("s", "cpu");
#if defined(USE_CLOCK_TYPE_GETTHREADTIMES)
        api = Py_BuildValue("s", "getthreadtimes");
        resolution = Py_BuildValue("s", "100ns");
#elif defined(USE_CLOCK_TYPE_THREADINFO)
        api = Py_BuildValue("s", "threadinfo");
        resolution = Py_BuildValue("s", "1000ns");
#elif defined(USE_CLOCK_TYPE_CLOCKGETTIME)
        api = Py_BuildValue("s", "clockgettime");
        resolution = Py_BuildValue("s", "1ns");
#elif defined(USE_CLOCK_TYPE_RUSAGE)
        api = Py_BuildValue("s", "getrusage");
        resolution = Py_BuildValue("s", "1000ns");
#endif
    }
    
    PyDict_SetItemString(result, "type", type);
    PyDict_SetItemString(result, "api", api);
    PyDict_SetItemString(result, "resolution", resolution);

    return result;
}

static PyMethodDef yappi_methods[] = {
    {"start", start, METH_VARARGS, NULL},
    {"stop", stop, METH_VARARGS, NULL},
    {"enum_func_stats", enum_func_stats, METH_VARARGS, NULL},
    {"enum_thread_stats", enum_thread_stats, METH_VARARGS, NULL},
    {"clear_stats", clear_stats, METH_VARARGS, NULL},
    {"is_running", is_running, METH_VARARGS, NULL},
    {"get_clock_type", get_clock_type, METH_VARARGS, NULL},
    {"set_clock_type", set_clock_type, METH_VARARGS, NULL},
    {"mem_usage", mem_usage, METH_VARARGS, NULL},
    {"set_timings", set_timings, METH_VARARGS, NULL}, // for test usage. do not call this directly.
    {"profile_event", profile_event, METH_VARARGS, NULL}, // for internal usage. do not call this.
    {NULL, NULL}      /* sentinel */
};

#ifdef IS_PY3K
static struct PyModuleDef _yappi_module = {
        PyModuleDef_HEAD_INIT,
        "_yappi",
        _yappi__doc__,
        -1,
        yappi_methods,
        NULL,
        NULL,
        NULL,
        NULL
};
#endif

PyMODINIT_FUNC
#ifdef IS_PY3K
PyInit__yappi(void)
#else
init_yappi(void)
#endif
{
    PyObject *m, *d;

#ifdef IS_PY3K
    m = PyModule_Create(&_yappi_module);
    if (m == NULL)
        return NULL;
#else
    m = Py_InitModule("_yappi",  yappi_methods);
    if (m == NULL)
        return;
#endif

    d = PyModule_GetDict(m);
    YappiProfileError = PyErr_NewException("_yappi.error", NULL, NULL);
    PyDict_SetItemString(d, "error", YappiProfileError);

    // init the profiler memory and internal constants
    yappinitialized = 0;
    yapphavestats = 0;
    yapprunning = 0;

    if (!_init_profiler()) {
        PyErr_SetString(YappiProfileError, "profiler cannot be initialized.");
#ifdef IS_PY3K
        return NULL;
#else
        return;
#endif
    }

#ifdef IS_PY3K
    return m;
#endif
}
