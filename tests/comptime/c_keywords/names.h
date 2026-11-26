// C, where Typed C's own keywords are ordinary names (as in <linux/atm.h>)
typedef struct {
    int pub;
    int import;
    int trait;
    int impl;
} Keywords;

static inline int keywords_sum(Keywords k) {
    return k.pub + k.import + k.trait + k.impl;
}

static inline int twice(int impl) {
    return impl * 2;
}

#define KEYWORDS_INIT {1, 2, 3, 4}
