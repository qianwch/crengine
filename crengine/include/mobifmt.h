#ifndef MOBIFMT_H
#define MOBIFMT_H
#include "epubfmt.h"
bool DetectMobi8Format(LVStreamRef stream);
bool ImportMobi8Document(LVStreamRef stream, ldomDocument *doc, LVDocViewCallback *progress,
    CacheLoadingCallback *cache, bool metadataOnly, const elem_def_t *elements,
    const attr_def_t *attributes, const ns_def_t *namespaces);
#endif
