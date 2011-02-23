/*
*    An Adaptive Hash Table
*    Sumer Cip 2010
*/

#ifndef YHASHTAB_H
#define YHASHTAB_H

#define HSIZE(n) (1<<n)
#define HMASK(n) (HSIZE(n)-1)
#define SWAP(a, b) (((a) ^= (b)), ((b) ^= (a)), ((a) ^= (b)))
#define HHASH(ht, a) ((a = (a ^ 61) ^ (a >> 16)), \
                      (a = a + (a << 3)), (a = a ^ (a >> 4)),  (a = a * 0x27d4eb2d), \
                      (a = a ^ (a >> 15)), ((unsigned int)(a & ht->mask)))
//#define HHASH(ht, a) ((unsigned int)(a & ht->mask))
#define HLOADFACTOR 0.75

struct _hitem {
    int key;
    int val;
    int free; // for recycling.
    struct _hitem *next;
};
typedef struct _hitem _hitem;

typedef struct {
    int realsize;
    int logsize;
    int count;
    int mask;
    int freecount;
    _hitem ** _table;
} _htab;

_htab *htcreate(int logsize);
void htdestroy(_htab *ht);
_hitem *hfind(_htab *ht, int key);
int hadd(_htab *ht, int key, int val);
void henum(_htab *ht, int (*fn) (_hitem *item, void *arg), void *arg);
int hcount(_htab *ht);
void hfree(_htab *ht, _hitem *item);

#endif
