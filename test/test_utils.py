import sys
import yappi

def test_print(msg):
    sys.stdout.write(msg)

def test_passed(msg):
    test_print("[+]    TEST: %s passed.\r\n" % (msg))

def test_start():
    if yappi.is_running():
        yappi.stop()
    yappi.clear_stats()
    yappi.set_clock_type('cpu') # reset to default clock type

def _run(func):
    import __main__
    globals = locals = __main__.__dict__
    if sys.hexversion > 0x03000000:
        exec(func, globals, locals) 
    else:   
        eval(func, globals, locals)
        
def assert_raises_exception(func):
    try:
        _run(func)
        assert 0 == 1
    except:
        pass
        
def _run_with_yappi(func):
    yappi.start()
    _run(func)
    yappi.stop()

def run_and_get_func_stats(func, **kwargs):
    _run_with_yappi(func)
    return yappi.get_func_stats(**kwargs)

def run_and_get_thread_stats(func, **kwargs):
    _run_with_yappi(func)
    return yappi.get_thread_stats(**kwargs)

# both parent and child are YFuncStat objects
def get_child_stat(parent, child):
    for item in parent.children:
        if item.index == child.index:
            return item