'''
 yappi.py
 Yet Another Python Profiler

 Sumer Cip 2012
'''
import os
import sys
import threading
import _yappi
import pickle

class YappiError(Exception): pass

__all__ = ['start', 'stop', 'get_func_stats', 'get_thread_stats', 'clear_stats', 'is_running',
           'clock_type', 'mem_usage', 'thread_times']

CRLF = '\n'
COLUMN_GAP = 2
TIME_COLUMN_LEN = 8 # 0.000000, 12345.98, precision is microsecs

SORTTYPE_NAME = 0
SORTTYPE_NCALL = 3
SORTTYPE_TTOT = 4
SORTTYPE_TSUB = 5
SORTTYPE_TAVG = 8
SORTTYPE_THREAD_NAME = 0
SORTTYPE_THREAD_ID = 1
SORTTYPE_THREAD_TTOT = 3
SORTTYPE_THREAD_SCHEDCNT = 4

SORTORDER_ASC = 0
SORTORDER_DESC = 1

SHOW_ALL = 0

def _validate_func_sorttype(sort_type):
    if sort_type not in [SORTTYPE_NAME, SORTTYPE_NCALL, SORTTYPE_TTOT, SORTTYPE_TSUB, SORTTYPE_TAVG]:
        raise YappiError("Invalid SortType parameter.[%d]" % (sort_type))

def _validate_thread_sorttype(sort_type):
    if sort_type not in [SORTTYPE_THREAD_NAME, SORTTYPE_THREAD_ID, SORTTYPE_THREAD_TTOT, SORTTYPE_THREAD_SCHEDCNT]:
        raise YappiError("Invalid SortType parameter.[%d]" % (sort_type))
        
def _validate_sortorder(sort_order):
    if sort_order not in [SORTORDER_ASC, SORTORDER_DESC]:
        raise YappiError("Invalid SortOrder parameter.[%d]" % (sort_order))
        
'''
 _callback will only be called once per-thread. _yappi will detect
 the new thread and changes the profilefunc param of the ThreadState
 structure. This is an internal function please don't mess with it.
'''
def _callback(frame, event, arg):
    _yappi.profile_event(frame, event, arg)
    return _callback
    
class StatString(object):
    """
    Class to prettify/trim a profile result column.
    """
    
    _s = ""
    _TRAIL_DOT = ".."

    def __init__(self, s):
        self._s = str(s)

    def ltrim(self, length):
        if len(self._s) > length:
            self._s = self._s[-length:]
            return self._TRAIL_DOT + self._s[len(self._TRAIL_DOT):]
        else:
            return self._s + " " * (length - len(self._s))

    def rtrim(self, length):
        if len(self._s) > length:
            self._s = self._s[:length]
            return self._s[:-len(self._TRAIL_DOT)] + self._TRAIL_DOT
        else:
            return self._s + (" " * (length - len(self._s)))

class YStat(dict):
    """
    Class to hold a profile result line in a dict object, which all items can also be accessed as
    instance attributes where their attribute name is the given key. Mimicked NamedTuples.
    """
    _KEYS = () 
    
    def __init__(self, values):
        super(YStat, self).__init__()
        
        assert len(self._KEYS) == len(values)
        for i in range(len(self._KEYS)):
            setattr(self, self._KEYS[i], values[i])
            
    def __setattr__(self, name, value):
        if name in self._KEYS:
            self[self._KEYS.index(name)] = value
        super(YStat, self).__setattr__(name, value)
    
class YFuncStat(YStat):
    """
    Class holding information for function stats.
    """
    _KEYS = ('name', 'module', 'lineno', 'ncall', 'ttot', 'tsub', 'index', 'children', 'tavg', 'full_name')
    
    def __eq__(self, other):
        if other is None:
            return False
        return self.full_name == other.full_name
        
    def __add__(self, other):
    
        # do not merge if merging the same instance
        if self is other:
            return self
        
        self.ncall += other.ncall
        self.ttot += other.ttot
        self.tsub += other.tsub
        self.tavg += other.tavg
        for other_child_stat in other.children:
            # all children point to a valid entry, and we shall have merged previous entries by here.
            cur_child_stat = self.find_child_by_full_name(other_child_stat.full_name)
            if cur_child_stat is None:
                self.children.append(other_child_stat)
            else:
                cur_child_stat += other_child_stat
        
    def find_child_by_full_name(self, full_name):
        for child in self.children:
            if child.full_name == full_name:
                return child
        return None
        
class YChildFuncStat(YStat):
    """
    Class holding information for children function stats.
    """
    _KEYS = ('index', 'ncall', 'ttot', 'full_name')
    
    def __eq__(self, other):
        if other is None:
            return False
        return self.full_name == other.full_name
        
    def __add__(self, other):
        if other is None:
            return self       
        self.ncall += other.ncall
        self.ttot += other.ttot
             
class YThreadStat(YStat):
    """
    Class holding information for thread stats.
    """
    _KEYS = ('name', 'id', 'last_func_name', 'last_func_mod', 'last_line_no', 'ttot', 'sched_count', 'last_func_full_name')
            
class YStats(object):
    """
    Main Stats class where we collect the information from _yappi and apply the user filters.
    """
    def __init__(self):
        self._stats = []
        
    def get(self):
        return self
        
    def sort(self, sort_type, sort_order):
        self._stats.sort(key=lambda stat: stat[sort_type], reverse=(sort_order==SORTORDER_DESC))
        return self
        
    def limit(self, limit):
        if limit != SHOW_ALL:
            self._stats = self._stats[:limit]
        return self
        
    def clear(self):
        self._stats.clear()
    
    def __iter__(self):
        for stat in self._stats:
            yield stat

    def __repr__(self):
        return str(self._stats)
        
    def __len__(self):
        return len(self._stats)
        
    def __getitem__(self, item):
        return self._stats[item]
    
class YFuncStats(YStats):

    _idx_max = 0
    _SUPPORTED_LOAD_FORMATS = ['YSTAT']
    _SUPPORTED_SAVE_FORMATS = ['YSTAT', 'CALLGRIND']
        
    def get(self):
        _yappi.enum_func_stats(self._enumerator)
        
        # convert the children info from tuple to YChildFuncStat
        for stat in self._stats:
            _childs = []
            for child_tpl in stat.children:
                rstat = self.find_by_index(child_tpl[0])
                
                # sometimes even the profile results does not contain the result because of filtering 
                # or timing(call_leave called but call_enter is not), with this we ensure that the children
                # index always point to a valid stat.
                if rstat is None:
                    continue 
                    
                cfstat = YChildFuncStat(child_tpl+(rstat.full_name,))
                _childs.append(cfstat)
            stat.children = _childs
        
        return super(YFuncStats, self).get()
    
    def _enumerator(self, stat_entry):
        tavg = stat_entry[4]/stat_entry[3]
        full_name = "%s:%s:%d" % (stat_entry[1], stat_entry[0], stat_entry[2])
        fstat = YFuncStat(stat_entry + (tavg,full_name))
        
        # do not show profile stats of yappi itself. 
        if os.path.basename(fstat.module) == ("%s.py" % __name__) or fstat.module == "_yappi": 
            return
            
        self._stats.append(fstat)
        # hold the max idx number for merging new entries
        if self._idx_max < fstat.index:
            self._idx_max = fstat.index
        
    def _add_from_YSTAT(self, file):
        saved_stats = pickle.load(file)
        
        # add 'not present' previous entries with unique indexes
        for saved_stat in saved_stats:
            if saved_stat not in self._stats:
                self._idx_max += 1
                saved_stat.index = self._idx_max
                self._stats.append(saved_stat)                
                
        # fix children's index values
        for saved_stat in saved_stats:
            for saved_child_stat in saved_stat.children:
                # we know for sure child's index is pointing to a valid stat in saved_stats
                # so as saved_stat is already in sync. (in above loop), we can safely assume
                # that we shall point to a valid stat in current_stats with the child's full_name                
                saved_child_stat.index = self.find_by_full_name(saved_child_stat.full_name).index
                                
        # merge stats
        for saved_stat in saved_stats:
            saved_stat_in_curr = self.find_by_full_name(saved_stat.full_name)
            saved_stat_in_curr += saved_stat
    
    def _save_as_YSTAT(self, path):
        file = open(path, "wb")        
        pickle.dump(self._stats, file)
        
    def _save_as_CALLGRIND(self, path):
        """
        Writes all the function stats in a callgrind-style format to the given
        file. (stdout by default)
        """
        file = open(path, "w")
            
        header = """version: 1\ncreator: %s\npid: %d\ncmd:  %s\npart: 1\n\nevents: Ticks""" % \
            ('yappi', os.getpid(), ' '.join(sys.argv))

        lines = [header]

        # add function definitions
        file_ids = ['']
        func_ids = ['']
        for func_stat in self:
            file_ids += [ 'fl=(%d) %s' % (func_stat.index, func_stat.module) ]
            func_ids += [ 'fn=(%d) %s %s:%s' % (func_stat.index, func_stat.name, func_stat.module, func_stat.lineno) ]

        lines += file_ids + func_ids

        # add stats for each function we have a record of
        for func_stat in self:
            func_stats = [ '',
                           'fl=(%d)' % func_stat.index,
                           'fn=(%d)' % func_stat.index]
            func_stats += [ '%s %s' % (func_stat.lineno, int(func_stat.tsub * 1e6)) ]

            # children functions stats
            for child in func_stat.children:
                func_stats += [ 'cfl=(%d)' % child.index,
                                'cfn=(%d)' % child.index,
                                'calls=%d 0' % child.ncall,
                                '0 %d' % int(child.ttot * 1e6)
                                ]
            lines += func_stats
        file.write('\n'.join(lines))                
                
    def find_by_index(self, index):
        for stat in self._stats:
            if stat.index == index:
                return stat
        return None
        
    def find_by_full_name(self, full_name):
        for stat in self._stats:
            if stat.full_name == full_name:
                return stat
        return None
        
    def find_by_name(self, name):
        for stat in self._stats:
            if stat.name == name:
                return stat
        return None
      
    def add(self, path, type="ystat"):
    
        type = type.upper()
        if type not in self._SUPPORTED_LOAD_FORMATS:
            raise NotImplementedError('Loading from (%s) format is not possible currently.')
        
        f = open(path, "rb")
        try:
            add_func = getattr(self, "_add_from_%s" % (type))
            add_func(file=f)
        finally:
            f.close()
            
        return self
        
    def save(self, path, type="ystat"):
        
        type = type.upper()
        if type not in self._SUPPORTED_SAVE_FORMATS:
            raise NotImplementedError('Saving in "%s" format is not possible currently.' % (type))
    
        save_func = getattr(self, "_save_as_%s" % (type))
        save_func(path=path)
        
    def print_all(self, out=sys.stdout):
        """
        Prints all of the function profiler results to a given file. (stdout by default)
        """
        FUNC_NAME_LEN = 38
        CALLCOUNT_LEN = 9
        
        out.write(CRLF)
        out.write("name                                    #n         tsub      ttot      tavg")
        out.write(CRLF)
        for stat in self:
            out.write(StatString(stat.full_name).ltrim(FUNC_NAME_LEN))
            out.write(" " * COLUMN_GAP)
            out.write(StatString(stat.ncall).rtrim(CALLCOUNT_LEN))
            out.write(" " * COLUMN_GAP)
            out.write(StatString("%0.6f" % stat.tsub).rtrim(TIME_COLUMN_LEN))
            out.write(" " * COLUMN_GAP)
            out.write(StatString("%0.6f" % stat.ttot).rtrim(TIME_COLUMN_LEN))
            out.write(" " * COLUMN_GAP)
            out.write(StatString("%0.6f" % stat.tavg).rtrim(TIME_COLUMN_LEN))
            out.write(CRLF)
            
    def sort(self, sort_type=SORTTYPE_NCALL, sort_order=SORTORDER_DESC):
        _validate_func_sorttype(sort_type)
        _validate_sortorder(sort_order)

        return super(YFuncStats, self).sort(sort_type, sort_order)
        
    def debug_print(self):
        console = sys.stdout
        CHILD_STATS_LEFT_MARGIN = 5
        for stat in self:
            console.write("index: %d" % stat.index)
            console.write(CRLF)
            console.write("full_name: %s" % stat.full_name)
            console.write(CRLF)
            console.write("ncall: %d" % stat.ncall)
            console.write(CRLF)
            console.write("ttot: %0.6f" % stat.ttot)
            console.write(CRLF)
            console.write("tsub: %0.6f" % stat.tsub)
            console.write(CRLF)
            console.write("children: ")
            console.write(CRLF)
            for child_stat in stat.children:
                console.write(CRLF)
                console.write(" " * CHILD_STATS_LEFT_MARGIN)
                console.write("index: %d" % child_stat.index)
                console.write(CRLF)
                console.write(" " * CHILD_STATS_LEFT_MARGIN)
                console.write("full_name: %s" % child_stat.full_name)
                console.write(CRLF)
                console.write(" " * CHILD_STATS_LEFT_MARGIN)
                console.write("ncall: %d" % child_stat.ncall)
                console.write(CRLF)
                console.write(" " * CHILD_STATS_LEFT_MARGIN)
                console.write("ttot: %0.6f" % child_stat.ttot)
                console.write(CRLF)                
            console.write(CRLF)
        
class YThreadStats(YStats):
        
    def get(self):
        _yappi.enum_thread_stats(self._enumerator)
        
        return super(YThreadStats, self).get()
        
    def _enumerator(self, stat_entry):
        last_func_full_name = "%s:%s:%d" % (stat_entry[3], stat_entry[2], stat_entry[4])
        tstat = YThreadStat(stat_entry + (last_func_full_name, ))
        self._stats.append(tstat)
        
    def sort(self, sort_type=SORTTYPE_THREAD_NAME, sort_order=SORTORDER_DESC):
        _validate_thread_sorttype(sort_type)
        _validate_sortorder(sort_order)

        return super(YThreadStats, self).sort(sort_type, sort_order)
        
    def print_all(self, out=sys.stdout):
        """
        Prints all of the thread profiler results to a given file. (stdout by default)
        """
        THREAD_FUNC_NAME_LEN = 25
        THREAD_NAME_LEN = 13
        THREAD_ID_LEN = 15
        THREAD_SCHED_CNT_LEN = 10

        out.write(CRLF)
        out.write("name           tid              fname                      ttot      scnt")
        out.write(CRLF)
        for stat in stats:
            out.write(StatString(stat.name).ltrim(THREAD_NAME_LEN))
            out.write(" " * COLUMN_GAP)
            out.write(StatString(stat.id).rtrim(THREAD_ID_LEN))
            out.write(" " * COLUMN_GAP)
            out.write(StatString(stat.last_func_full_name).ltrim(THREAD_FUNC_NAME_LEN))
            out.write(" " * COLUMN_GAP)
            out.write(StatString("%0.6f" % stat.ttot).rtrim(TIME_COLUMN_LEN))
            out.write(" " * COLUMN_GAP)
            out.write(StatString(stat.sched_count).rtrim(THREAD_SCHED_CNT_LEN))
            out.write(CRLF)

def is_running():
    return bool(_yappi.is_running())

def start(builtins=False, profile_threads=True):
    """
    Start profiler.
    """
    if profile_threads:
        threading.setprofile(_callback)
    _yappi.start(builtins, profile_threads)

def get_func_stats():
    """
    Gets the function profiler results with given filters and returns an iterable.
    """
    
    stats = YFuncStats().get()
    return stats

def get_thread_stats():
    """
    Gets the thread profiler results with given filters and returns an iterable.
    """
    
    stats = YThreadStats().get()
    return stats

def stop():
    """
    Stop profiler.
    """
    _yappi.stop()
    threading.setprofile(None)    

def clear_stats():
    """
    Clears all of the profile results.
    """
    _yappi.clear_stats()

def clock_type():
    """
    Returns the internal native(OS dependant) API used to retrieve per-thread cputime and
    its resolution.
    """
    return _yappi.clock_type()

def thread_times():
    """
    Returns the total CPU time of the calling thread as a float.(in secs) Precision is OS dependent.
    """
    return _yappi.thread_times()

def mem_usage():
    """
    Returns the memory usage of the profiler itself.
    """
    return _yappi.mem_usage()
 
def main():
    from optparse import OptionParser
    usage = "yappi.py [-b] [scriptfile] args ..."
    parser = OptionParser(usage=usage)
    parser.allow_interspersed_args = False
    parser.add_option("-b", "--builtins",
                  action="store_true", dest="profile_builtins", default=False,
                  help="Profiles builtin functions when set. [default: False]")
    parser.add_option("-m", "--profile_threads",
                  action="store_true", dest="profile_threads", default=True,
                  help="Profiles all of the threads. [default: True]")
    if not sys.argv[1:]:
        parser.print_usage()
        sys.exit(2)

    (options, args) = parser.parse_args()
    sys.argv[:] = args

    if (len(sys.argv) > 0):
        sys.path.insert(0, os.path.dirname(sys.argv[0]))
        start(options.profile_builtins, options.profile_multithreaded)
        if sys.version_info >= (3, 0):
            exec(compile(open(sys.argv[0]).read(), sys.argv[0], 'exec'),
               sys._getframe(1).f_globals, sys._getframe(1).f_locals)
        else:
            execfile(sys.argv[0], sys._getframe(1).f_globals, sys._getframe(1).f_locals)
        stop()
        # we will currently use default params for these
        get_func_stats().print_all()
        get_thread_stats().print_all()
    else:
        parser.print_usage()

if __name__ == "__main__":
    main()
