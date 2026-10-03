#include "../include/mobifmt.h"
#include <cstdio>
#include <climits>
#include <cstring>
extern "C" {
#include <mobi.h>
}

// Parts are owned for the entire DOM/container lifetime, including lazy image
// and font loading. Streams take copies so they remain valid independently.
class MobiPartItem : public LVCommonContainerItemInfo {
public:
    MOBIPart *part;
    MobiPartItem(lString32 name, MOBIPart *value) : part(value) {
        SetItemInfo(name, value->size, 0);
    }
};

class MobiResourceContainer : public LVContainer {
    MOBIData *_mobi;
    MOBIRawml *_raw = NULL;
    MOBIRawml *_binary = NULL;
    LVPtrVector<MobiPartItem> _items;
    LVPtrVector<MobiPartItem> _binaryItems;
    bool _attempted = false;
    bool _ready = false;
    bool _binaryAttempted = false;
    bool _binaryReady = false;

    bool addParts(MOBIPart *part, const char *prefix, bool skipFirst,
            LVPtrVector<MobiPartItem> &items) {
        if (skipFirst && part) part = part->next;
        for (; part; part = part->next) {
            if (!part->size) continue;
            if (!part->data || part->size > INT_MAX) return false;
            MOBIFileMeta meta = mobi_get_filemeta_by_type(part->type);
            char name[128];
            if (part->type == T_OPF)
                snprintf(name, sizeof(name), "content.opf");
            else
                snprintf(name, sizeof(name), "%s%05zu.%s", prefix, part->uid, meta.extension);
            items.add(new MobiPartItem(Utf8ToUnicode(name), part));
        }
        return true;
    }
    bool ensureParts() {
        if (_attempted) return _ready;
        _attempted = true;
        _raw = mobi_init_rawml(_mobi);
        if (!_raw) return false;
        MOBI_RET result = mobi_parse_rawml(_raw, _mobi);
        if (result != MOBI_SUCCESS) {
            CRLog::error("KF8: parsing failed (%d)", result);
            return false;
        }
        _ready = addParts(_raw->markup, "part", false, _items)
            && addParts(_raw->flow, "flow", true, _items)
            && addParts(_raw->resources, "resource", false, _items);
        return _ready;
    }
    bool ensureBinaryResources() {
        if (_binaryAttempted) return _binaryReady;
        _binaryAttempted = true;
        _binary = mobi_init_rawml(_mobi);
        if (!_binary || mobi_parse_resources(_binary, _mobi) != MOBI_SUCCESS) return false;
        _binaryReady = addParts(_binary->resources, "resource", false, _binaryItems);
        return _binaryReady;
    }
    LVStreamRef openPartStream(LVPtrVector<MobiPartItem> &items, const lChar32 *name) {
        for (int i = 0; i < items.length(); i++) {
            if (lString32(name) == items[i]->GetName()) {
                MOBIPart *part = items[i]->part;
                LVStreamRef stream = LVCreateMemoryStream(part->data, (int)part->size, true);
                stream->SetName(name);
                return stream;
            }
        }
        return LVStreamRef();
    }
public:
    MobiResourceContainer(MOBIData *mobi) : _mobi(mobi) {}
    ~MobiResourceContainer() {
        mobi_free_rawml(_binary);
        mobi_free_rawml(_raw);
        mobi_free(_mobi);
    }
    LVContainer *GetParentContainer() { return NULL; }
    int GetObjectCount() const {
        return const_cast<MobiResourceContainer *>(this)->ensureParts() ? _items.length() : 0;
    }
    lverror_t GetSize(lvsize_t *size) {
        if (!ensureParts()) return LVERR_FAIL;
        *size = _items.length(); return LVERR_OK;
    }
    const LVContainerItemInfo *GetObjectInfo(int index) {
        if (!ensureParts()) return NULL;
        return index >= 0 && index < _items.length() ? _items[index] : NULL;
    }
    const LVContainerItemInfo *GetObjectInfo(lString32 name) {
        if (!ensureParts()) return NULL;
        for (int i = 0; i < _items.length(); i++)
            if (name == _items[i]->GetName()) return _items[i];
        return NULL;
    }
    LVStreamRef OpenStream(const lChar32 *name, lvopen_mode_t mode) {
        if (mode != LVOM_READ) return LVStreamRef();
        // Cached layout still asks for cover images and embedded fonts. These
        // records can be decoded without inflating the entire book text.
        if (!_attempted && lString32(name).startsWith(U"resource") && ensureBinaryResources()) {
            LVStreamRef stream = openPartStream(_binaryItems, name);
            if (!stream.isNull()) return stream;
        }
        if (!ensureParts()) return LVStreamRef();
        return openPartStream(_items, name);
    }
};

bool DetectMobi8Format(LVStreamRef stream) {
    lString32 name(stream->GetName());
    name.lowercase();
    if (name.endsWith(U".azw3")) return true;
    // Pure KF8 files can also use .mobi/.azw. Leave legacy MOBI to its importer.
    unsigned char header[86];
    stream->SetPos(0);
    lvsize_t count = 0;
    if (stream->Read(header, sizeof(header), &count) != LVERR_OK || count != sizeof(header)
        || memcmp(header + 60, "BOOKMOBI", 8)) return false;
    unsigned long record = ((unsigned long)header[78] << 24) | ((unsigned long)header[79] << 16)
        | ((unsigned long)header[80] << 8) | header[81];
    unsigned char version[4];
    stream->SetPos(record + 36);
    if (stream->Read(version, 4, &count) != LVERR_OK || count != 4) return false;
    return version[0] == 0 && version[1] == 0 && version[2] == 0 && version[3] == 8;
}

bool ImportMobi8Document(LVStreamRef stream, ldomDocument *doc, LVDocViewCallback *progress,
    CacheLoadingCallback *cache, bool metadataOnly, const elem_def_t *elements,
    const attr_def_t *attributes, const ns_def_t *namespaces) {
    // libmobi reads the original seekable file. No temporary output or exporter.
    FILE *file = fopen(UnicodeToUtf8(lString32(stream->GetName())).c_str(), "rb");
    if (!file) return false;
    MOBIData *mobi = mobi_init();
    if (!mobi) { fclose(file); return false; }
    MOBI_RET result = mobi_load_file(mobi, file);
    fclose(file);
    if (result != MOBI_SUCCESS || mobi_is_encrypted(mobi) || !mobi_is_kf8(mobi)) {
        CRLog::error("KF8: invalid, encrypted or unsupported source (%d)", result);
        mobi_free(mobi);
        return false;
    }
    LVContainerRef resources(new MobiResourceContainer(mobi));
    doc->setContainer(resources);
    int previewFragments = doc->getProps()->getIntDef("azw3.preview.fragments", 0);
    // Restore the DOM and page layout first. Most text-only cache hits need no
    // KF8 reconstruction at all; fonts and images trigger it lazily if accessed.
    if (!metadataOnly && doc->openFromCache(cache, progress)) return true;
    doc->getProps()->setBool("azw3.preview.active", !metadataOnly && previewFragments > 0);
    doc->getProps()->setInt("azw3.preview.fragments", previewFragments);
    doc->setContainer(resources);
    return ImportPublicationDocument(resources, cs32("content.opf"), doc, progress, cache,
        metadataOnly, elements, attributes, namespaces);
}
