import sys
import yappi
import unittest

class YappiUnitTestCase(unittest.TestCase):
    def setUp(self):
        if yappi.is_running():
            yappi.stop()
        yappi.clear_stats()
        yappi.set_clock_type('cpu') # reset to default clock type

    def tearDown(self):
        fstats = yappi.get_func_stats()
        if not fstats._debug_check_sanity():
            sys.stdout.write("ERR: Duplicates found in Func stats")
            sys.stdout.write(CRLF)
            fstats.debug_print()
        for fstat in fstats:
            if not fstat.children._debug_check_sanity():
                sys.stdout.write("ERR: Duplicates found in ChildFunc stats")
                sys.stdout.write(CRLF)
                fstat.children.print_all()
        tstats = yappi.get_func_stats()
        if not tstats._debug_check_sanity():
            sys.stdout.write("ERR: Duplicates found in Thread stats")
            sys.stdout.write(CRLF)
            tstats.print_all()
            
def assert_raises_exception(func):
    try:
        _run(func)
        assert 0 == 1
    except:
        pass
        
def run_with_yappi(func, *args, **kwargs):
    yappi.start()
    func(*args, **kwargs)
    yappi.stop()

def run_and_get_func_stats(func, *args, **kwargs):
    run_with_yappi(func, *args, **kwargs)
    return yappi.get_func_stats()

def run_and_get_thread_stats(func, *args, **kwargs):
    run_with_yappi(func, *args, **kwargs)
    return yappi.get_thread_stats()
    
def is_py3x():
    return sys.version_info > (3, 0)
    
def find_stat_by_name(stats, name):
    for stat in stats:
        if stat.name == name:
            return stat
     
