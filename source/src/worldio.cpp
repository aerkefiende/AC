// worldio.cpp: loading & saving of maps and savegames

#include "cube.h"

#define DEBUGCOND (worldiodebug==1)
VARP(worldiodebug, 0, 0, 1);

void backup(char *name, char *backupname)
{
    string backupfile;
    copystring(backupfile, findfile(backupname, "wb"));
    remove(backupfile);
    rename(findfile(name, "wb"), backupfile);
}

static string cgzname, ocgzname, bakname, cbakname, pcfname, mcfname, omcfname, mapname;

const char *setnames(const char *name)
{
    string pakname;
    const char *slash = strpbrk(name, "/\\");
    if(slash)
    {
        copystring(pakname, name, slash-name+1);
        copystring(mapname, slash+1);
    }
    else
    {
        copystring(pakname, "maps");
        copystring(mapname, name);
    }
    formatstring(cgzname)("packages/%s/%s.cgz",      pakname, mapname);
    formatstring(ocgzname)("packages/maps/official/%s.cgz",   mapname);
    formatstring(bakname)("packages/%s/%s_%s.BAK",   pakname, mapname, numtime());
    formatstring(cbakname)("packages/%s/%s.cfg_%s.BAK",   pakname, mapname, numtime());
    formatstring(pcfname)("packages/%s/package.cfg", pakname);
    formatstring(mcfname)("packages/%s/%s.cfg",      pakname, mapname);
    formatstring(omcfname)("packages/maps/official/%s.cfg",   mapname);

    path(cgzname);
    path(bakname);
    path(cbakname);
    return cgzname;
}

// the optimize routines below are here to reduce the detrimental effects of messy mapping by
// setting certain properties (vdeltas and textures) to neighbouring values wherever there is no
// visible difference. This allows the mipmapper to generate more efficient mips.
// the reason it is done on save is to reduce the amount spend in the mipmapper (as that is done
// in realtime).

inline bool nhf(sqr *s) { return s->type!=FHF && s->type!=CHF; }

void voptimize()        // reset vdeltas on non-hf cubes
{
    loop(y, ssize) loop(x, ssize)
    {
        sqr *s = S(x, y);
        if(x && y) { if(nhf(s) && nhf(S(x-1, y)) && nhf(S(x-1, y-1)) && nhf(S(x, y-1))) s->vdelta = 0; }
        else s->vdelta = 0;
    }
}

// these two are used by getmap/sendmap.. transfers compressed maps directly

void writemap(char *name, int msize, uchar *mdata)
{
    setnames(name);
    backup(cgzname, bakname);
    stream *f = openfile(cgzname, "wb");
    if(!f) { conoutf("\f3could not write map to %s", cgzname); return; }
    f->write(mdata, msize);
    delete f;
    conoutf("wrote map %s as file %s", name, cgzname);
}

uchar *readmap(char *name, int *size, int *revision)
{
    setnames(name);
    uchar *data = (uchar *)loadfile(cgzname, size);
    if(!data) { conoutf("\f3could not read map %s", cgzname); return NULL; }
    mapstats *ms = loadmapstats(cgzname, false);
    if(revision) *revision = ms->hdr.maprevision;
    return data;
}

void writecfggz(char *name, int size, int sizegz, uchar *data)
{
    if(size < 1 || !sizegz || size > MAXCFGFILESIZE) return;
    setnames(name);

    uchar *rawcfg = new uchar[size];
    uLongf rawsize = size;
    if(rawcfg && uncompress(rawcfg, &rawsize, data, sizegz) == Z_OK && rawsize - size == 0)
    {
        stream *f = openfile(mcfname, "w");
        if(f)
        {
            f->write(rawcfg, size);
            delete f;
            conoutf("wrote map config to %s", mcfname);
        }
        else
        {
            conoutf("\f3could not write config to %s", mcfname);
        }
    }
    DELETEA(rawcfg);
}

#define GZBUFSIZE ((MAXCFGFILESIZE * 11) / 10)

uchar *readmcfggz(char *name, int *size, int *sizegz)
{
    setnames(name);
    uchar *gzbuf = new uchar[GZBUFSIZE];
    uchar *data = (uchar*)loadfile(mcfname, size, "r");
    if(data && *size < MAXCFGFILESIZE)
    {
        uLongf gzbufsize = GZBUFSIZE;
        if(compress2(gzbuf, &gzbufsize, data, *size, 9) != Z_OK)
        {
            *size = 0;
            gzbufsize = 0;
            DELETEA(gzbuf);
        }
        *sizegz = (int) gzbufsize;
    }
    else
    {
        DELETEA(gzbuf);
    }
    DELETEA(data);
    return gzbuf;
}

// save map as .cgz file. uses 2 layers of compression: first does simple run-length
// encoding and leaves out data for certain kinds of cubes, then zlib removes the
// last bits of redundancy. Both passes contribute greatly to the miniscule map sizes.

void rlencodecubes(vector<uchar> &f, sqr *s, int len, bool preservesolids) // run-length encoding and serialisation of a series of cubes
{
    sqr *t = NULL;
    int sc = 0;
    #define spurge while(sc) { f.add(255); if(sc>255) { f.add(255); sc -= 255; } else { f.add(sc); sc = 0; } }
    #define c(f) (s->f==t->f)
    while(len-- > 0)
    {
        // 4 types of blocks, to compress a bit:
        // 255 (2): same as previous block + count
        // 254 (3): same as previous, except light // deprecated
        // SOLID (5)
        // anything else (9)

        if(SOLID(s) && !preservesolids)
        {
            if(t && c(type) && c(wtex) && c(vdelta))
            {
                sc++;
            }
            else
            {
                spurge;
                f.add(s->type);
                f.add(s->wtex);
                f.add(s->vdelta);
            }
        }
        else
        {
            if(t && c(type) && c(floor) && c(ceil) && c(ctex) && c(ftex) && c(utex) && c(wtex) && c(vdelta) && c(tag))
            {
                sc++;
            }
            else
            {
                spurge;
                f.add(s->type == SOLID ? 253 : s->type);
                f.add(s->floor);
                f.add(s->ceil);
                f.add(s->wtex);
                f.add(s->ftex);
                f.add(s->ctex);
                f.add(s->vdelta);
                f.add(s->utex);
                f.add(s->tag);
            }
        }
        t = s;
        s++;
    }
    spurge;
}

void rldecodecubes(ucharbuf &f, sqr *s, int len, int version, bool silent) // run-length decoding of a series of cubes (version is only relevant, if < 6)
{
    sqr *t = NULL, *e = s + len;
    while(s < e)
    {
        int type = f.overread() ? -1 : f.get();
        switch(type)
        {
            case -1:
            {
                if(!silent) conoutf("while reading map at %d: unexpected end of file", cubicsize - (e - s));
                f.forceoverread();
                silent = true;
                sqrdefault(s);
                break;
            }
            case 255:
            {
                if(!t) { f.forceoverread(); continue; }
                int n = f.get();
                loopi(n) memcpy(s++, t, sizeof(sqr));
                s--;
                break;
            }
            case 254: // only in MAPVERSION<=2
            {
                if(!t) { f.forceoverread(); continue; }
                memcpy(s, t, sizeof(sqr));
                f.get(); f.get();
                break;
            }
            case SOLID:
            {
                sqrdefault(s);                  // takes care of ftex, ctex, floor, ceil and tag
                s->type = SOLID;
                s->utex = s->wtex = f.get();
                s->vdelta = f.get();
                if(version<=2) { f.get(); f.get(); }
                break;
            }
            case 253: // SOLID with all textures during editing (undo)
                type = SOLID;
            default:
            {
                if(type<0 || type>=MAXTYPE)
                {
                    if(!silent) conoutf("while reading map at %d: type %d out of range", cubicsize - (e - s), type);
                    f.overread();
                    continue;
                }
                sqrdefault(s);
                s->type = type;
                s->floor = f.get();
                s->ceil = f.get();
                if(s->floor>=s->ceil) s->floor = s->ceil-1;  // for pre 12_13
                s->wtex = f.get();
                s->ftex = f.get();
                s->ctex = f.get();
                if(version<=2) { f.get(); f.get(); }
                s->vdelta = f.get();
                s->utex = (version>=2) ? f.get() : s->wtex;
                s->tag = (version>=5) ? f.get() : 0;
            }
        }
        s->defer = 0;
        t = s;
        s++;
    }
}

// headerextra stores additional data in a map file (support since format 10)
// data can be persistent or oneway
// the format and handling is explicitly designed to handle yet unknown header types to avoid further format version bumps

struct headerextra
{
    int len, flags;
    uchar *data;
    headerextra() : len(0), flags(0), data(NULL) {}
    headerextra(int l, int f, uchar *d) : len(l), flags(f), data(NULL) { if(d) { data = new uchar[len]; memcpy(data, d, len); } }
    ~headerextra() { DELETEA(data); }
    headerextra *duplicate() { return new headerextra(len, flags, data); }
};
vector<headerextra *> headerextras;

enum { HX_UNUSED = 0, HX_MAPINFO, HX_MODEINFO, HX_ARTIST, HX_EDITUNDO, HX_CONFIG, HX_NUM, HX_TYPEMASK = 0x3f, HX_FLAG_PERSIST = 0x40 };
const char *hx_names[] = { "unused", "mapinfo", "modeinfo", "artist", "editundo", "config", "unknown" };
#define addhxpacket(p, len, flags, buffer) { if(p.length() + len < MAXHEADEREXTRA) { putuint(p, len); putuint(p, flags); p.put(buffer, len); } }
#define hx_name(t) hx_names[min((t) & HX_TYPEMASK, int(HX_NUM))]

void clearheaderextras() { loopvrev(headerextras) delete headerextras.remove(i); }

void deleteheaderextra(int n) { if(headerextras.inrange(n)) delete headerextras.remove(n); }

COMMANDF(listheaderextras, "", ()
{
    loopv(headerextras) conoutf("extra header record %d: %s, %d bytes", i, hx_name(headerextras[i]->flags), headerextras[i]->len);
    if(!headerextras.length()) conoutf("no extra header records found");
});

int findheaderextra(int type)
{
    loopv(headerextras) if((headerextras[i]->flags & HX_TYPEMASK) == type) return i;
    return -1;
}

void unpackheaderextra(uchar *buf, int len)  // break the extra data from the mapheader into its pieces
{
    ucharbuf p(buf, len);
    DEBUG("unpacking " << len << " bytes");
    while(1)
    {
        int len = getuint(p), flags = getuint(p), type = flags & HX_TYPEMASK;
        if(p.overread() || len > p.remaining()) break;
        clientlogf(" found headerextra \"%s\", %d bytes%s", hx_name(type), len, flags & HX_FLAG_PERSIST ? "persistent" : "");  // debug info
        headerextras.add(new headerextra(len, flags, p.subbuf(len).buf));
    }
}

void parseheaderextra(bool clearnonpersist = true, int ignoretypes = 0)  // parse all headerextra packets, delete the nonpersistent ones (like editundos)
{
    DEBUG("parsing " << headerextras.length() << " packets");
    loopv(headerextras)
    {
        ucharbuf q(headerextras[i]->data, headerextras[i]->len);
        int type = headerextras[i]->flags & HX_TYPEMASK;
        DEBUG("packet " << i << " type " << type);
        bool deletethis = false;                               // (set deletethis for headers that persist outside headerextras and get reinserted by packheaderextras())
        if(!(ignoretypes & (1 << type))) switch(type)
        {
            case HX_EDITUNDO:
                restoreeditundo(q);
                break;

            case HX_CONFIG:
                setcontext("map", "embedded");
                execute((const char *)q.buf);
                resetcontext();
                break;

            case HX_MAPINFO:
            case HX_MODEINFO:
            case HX_ARTIST:
            default:
                break;
        }
        if(deletethis || (clearnonpersist && !(headerextras[i]->flags & HX_FLAG_PERSIST))) delete headerextras.remove(i--);
    }
}

ucharbuf packheaderextras(int ignoretypes = 0)  // serialise all extra data packets to save with the map header
{
    vector<uchar> buf, tmp;
    loopv(headerextras) if(!(ignoretypes & (1 << (headerextras[i]->flags & HX_TYPEMASK))))
    { // copy existing persistent hx packets
        addhxpacket(buf, headerextras[i]->len, headerextras[i]->flags, headerextras[i]->data);
    }
    // create non-persistent ones, if wanted
    if(!(ignoretypes & (1 << HX_EDITUNDO)))
    {
        int limit = (MAXHEADEREXTRA - buf.length()) / 3;
        backupeditundo(tmp, limit, limit);
        addhxpacket(buf, tmp.length(), HX_EDITUNDO, tmp.getbuf());
    }
    ucharbuf q(new uchar[buf.length()], buf.length());
    memcpy(q.buf, buf.getbuf(), q.maxlen);
    DEBUG("packed " << q.maxlen << " bytes");
    return q;
}

bool embedconfigfile()
{
    if(securemapcheck(getclientmap())) return false;
    setnames(getclientmap());
    int len;
    uchar *buf = (uchar *)loadfile(path(mcfname), &len);
    if(buf)
    {
        for(int n; (n = findheaderextra(HX_CONFIG)) >= 0; ) deleteheaderextra(n);
        headerextras.add(new headerextra(len + 1, HX_CONFIG|HX_FLAG_PERSIST, NULL))->data = buf;
        backup(mcfname, cbakname); // don't delete the config file, just rename
        conoutf("embedded map config file \"%s\"", mcfname);
    }
    else conoutf("\f3failed to load config file %s", mcfname);
    return buf != NULL;
}
COMMANDF(embedconfigfile, "", () { if(embedconfigfile()) save_world(getclientmap()); });

void extractconfigfile()
{
    if(securemapcheck(getclientmap())) return;
    setnames(getclientmap());
    int n = findheaderextra(HX_CONFIG);
    if(n < 0) conoutf("no embedded config found");
    else
    {
        backup(mcfname, cbakname); // don't overwrite an existing config file, just rename it
        stream *f = openfile(mcfname, "w");
        if(f)
        {
            f->write(headerextras[n]->data, headerextras[n]->len);
            delete f;
            conoutf("extracted embedded map config to file %s", mcfname);
            deleteheaderextra(n);
        }
        else conoutf("\f3failed to write config to %s", mcfname);
    }
}
COMMAND(extractconfigfile, "");

VAR(advancemaprevision, 1, 1, 100);

VARP(mapbackupsonsave, 0, 1, 1);

void save_world(char *mname, bool skipoptimise, bool addcomfort)
{
    if(!*mname) mname = getclientmap();
    if(securemapcheck(mname)) return;
    DEBUG("writing map \"" << mname << "\"");
    if(!validmapname(mname))
    {
        conoutf("\f3Invalid map name. It must only contain letters, digits, '-', '_' and be less than %d characters long", MAXMAPNAMELEN);
        return;
    }
    if(!skipoptimise)
    {
        voptimize();
        mapmrproper(false);
        addcomfort = false; // "optimized + undos" is not useful
    }
    setnames(mname);
    if(mapbackupsonsave) backup(cgzname, bakname);
    stream *f = opengzfile(cgzname, "wb");
    if(!f) { conoutf("could not write map to %s", cgzname); return; }
    strncpy(hdr.head, "ACMP", 4); // ensure map now declares itself as an AssaultCube map, even if imported as CUBE
    hdr.version = MAPVERSION;
    hdr.headersize = sizeof(header);
    hdr.numents = 0;
    loopv(ents) if(ents[i].type!=NOTUSED) hdr.numents++;
    if(hdr.numents > MAXENTITIES)
    {
        conoutf("too many map entities (%d), only %d will be written to file", hdr.numents, MAXENTITIES);
        hdr.numents = MAXENTITIES;
    }
    header tmp = hdr;
    ucharbuf hx = packheaderextras(addcomfort ? 0 : (1 << HX_EDITUNDO));   // if addcomfort -> add undos/redos
    int writeextra = 0;
    if(hx.maxlen) tmp.headersize += writeextra = clamp(hx.maxlen, 0, MAXHEADEREXTRA);
    if(writeextra || skipoptimise) tmp.version = 10;   // 9 and 10 are the same, but in 10 the headersize is reliable - if we don't need it, stick to 9
    tmp.maprevision += advancemaprevision;
    DEBUG("version " << tmp.version << " headersize " << tmp.headersize << " entities " << tmp.numents << " factor " << tmp.sfactor << " revision " << tmp.maprevision);
    lilswap(&tmp.version, 4);
    lilswap(&tmp.waterlevel, 1);
    lilswap(&tmp.maprevision, 2);
    f->write(&tmp, sizeof(header));
    if(writeextra) f->write(hx.buf, writeextra);
    delete[] hx.buf;
    int ne = hdr.numents;
    loopv(ents)
    {
        if(ents[i].type!=NOTUSED)
        {
            if(!ne--) break;
            persistent_entity tmp = ents[i];
            lilswap((short *)&tmp, 4);
            f->write(&tmp, sizeof(persistent_entity));
        }
    }

    vector<uchar> rawcubes;
    rlencodecubes(rawcubes, world, cubicsize, skipoptimise);  // if skipoptimize -> keep properties of solid cubes (forces format 10)
    f->write(rawcubes.getbuf(), rawcubes.length());
    delete f;
    unsavededits = 0;
    conoutf("wrote map file %s %s%s", cgzname, skipoptimise ? "without optimisation" : "(optimised)", addcomfort ? " (including editing history)" : "");
}
VARP(preserveundosonsave, 0, 0, 1);
COMMANDF(savemap, "s", (char *name) { save_world(name, preserveundosonsave && editmode, preserveundosonsave && editmode); } );
COMMANDF(savemapoptimised, "s", (char *name) { save_world(name, false, false); } );

extern int mapdims[8];     // min/max X/Y and delta X/Y and min-floor/max-ceil
void showmapdims()
{
    conoutf("  min X|Y|Z: %3d : %3d : %3d", mapdims[0], mapdims[1], mapdims[6]);
    conoutf("  max X|Y|Z: %3d : %3d : %3d", mapdims[2], mapdims[3], mapdims[7]);
    conoutf("delta X|Y|Z: %3d : %3d : %3d", mapdims[4], mapdims[5], mapdims[7]-mapdims[6]);
}
COMMAND(showmapdims, "");

extern void preparectf(bool cleanonly = false);
int numspawn[3], maploaded = 0, numflagspawn[2];
VAR(curmaprevision, 1, 0, 0);

extern char *mlayout;
extern int Mv, Ma, Hhits;
extern float Mh;

void rebuildtexlists()  // checks the texlists, if they still contain all possible textures
{
    short h[256];
    vector<uchar> missing;
    loopk(3)
    {
        missing.setsize(0);
        uchar *p = hdr.texlists[k];
        loopi(256) h[i] = 0;
        loopi(256) h[p[i]]++;
        loopi(256) if(h[i] == 0) missing.add(i);
        loopi(256) if(h[p[i]] > 1)
        {
            h[p[i]]--;
            p[i] = missing.pop();
        }
    }
}

static string lastloadedconfigfile;

bool load_world(char *mname)        // still supports all map formats that have existed since the earliest cube betas!
{
    const int sizeof_header = sizeof(header), sizeof_baseheader = sizeof_header - sizeof(int) * 16;
    stopwatch watch;
    watch.start();

    advancemaprevision = 1;
    setnames(mname);
    maploaded = getfilesize(ocgzname);
    if(maploaded > 0)
    {
        copystring(cgzname, ocgzname);
        copystring(mcfname, omcfname);
    }
    else maploaded = getfilesize(cgzname);
    if(!validmapname(mapname))
    {
        conoutf("\f3Invalid map name. It must only contain letters, digits, '-', '_' and be less than %d characters long", MAXMAPNAMELEN);
        return false;
    }
    stream *f = opengzfile(cgzname, "rb");
    if(!f) { conoutf("\f3could not read map %s", cgzname); return false; }
    if(unsavededits) xmapbackup("load_map_", mname);
    unsavededits = 0;
    DEBUG("reading map \"" << cgzname << "\"");
    header tmp;
    memset(&tmp, 0, sizeof_header);
    if(f->read(&tmp, sizeof_baseheader) != sizeof_baseheader ||
       (strncmp(tmp.head, "CUBE", 4)!=0 && strncmp(tmp.head, "ACMP",4)!=0)) { conoutf("\f3while reading map: header malformatted (1)"); delete f; return false; }
    lilswap(&tmp.version, 4); // version, headersize, sfactor, numents
    if(tmp.version > MAXMAPVERSION) { conoutf("\f3this map requires a newer version of AssaultCube"); delete f; return false; }
    if(tmp.sfactor<SMALLEST_FACTOR || tmp.sfactor>LARGEST_FACTOR || tmp.numents > MAXENTITIES) { conoutf("\f3illegal map size"); delete f; return false; }
    tmp.headersize = fixmapheadersize(tmp.version, tmp.headersize);
    int restofhead = min(tmp.headersize, sizeof_header) - sizeof_baseheader;
    if(f->read(&tmp.waterlevel, restofhead) != restofhead) { conoutf("\f3while reading map: header malformatted (2)"); delete f; return false; }
    DEBUG("version " << tmp.version << " headersize " << tmp.headersize << " headerextrasize " << tmp.headersize - int(sizeof(header)) << " entities " << tmp.numents << " factor " << tmp.sfactor << " revision " << tmp.maprevision);
    clearheaderextras();
    if(tmp.headersize > sizeof_header)
    {
        int extrasize = tmp.headersize - sizeof_header;
        if(tmp.version < 9) extrasize = 0;  // throw away mediareq...
        else if(extrasize > MAXHEADEREXTRA) extrasize = MAXHEADEREXTRA;
        if(extrasize)
        { // map file actually has extra header data that we want too preserve
            uchar *extrabuf = new uchar[extrasize];
            if(f->read(extrabuf, extrasize) != extrasize) { conoutf("\f3while reading map: header malformatted (3)"); delete f; return false; }
            unpackheaderextra(extrabuf, extrasize);
            delete[] extrabuf;
        }
        f->seek(tmp.headersize, SEEK_SET);
    }
    hdr = tmp;
    rebuildtexlists();
    loadingscreen("%s", hdr.maptitle);
    resetmap();
    if(hdr.version>=4)
    {
        lilswap(&hdr.waterlevel, 1);
        if(!hdr.watercolor[3]) setwatercolor();
        lilswap(&hdr.maprevision, 2);
        curmaprevision = hdr.maprevision;
    }
    else
    {
        hdr.waterlevel = -100000;
        hdr.ambient = 0;
    }
    setvar("waterlevel", hdr.waterlevel);
    ents.shrink(0);
    loopi(3) numspawn[i] = 0;
    loopi(2) numflagspawn[i] = 0;
    loopi(hdr.numents)
    {
        entity &e = ents.add();
        f->read(&e, sizeof(persistent_entity));
        lilswap((short *)&e, 4);
        e.spawned = false;
        if(e.type == LIGHT)
        {
            if(!e.attr2) e.attr2 = 255; // needed for MAPVERSION<=2
            if(e.attr1>32) e.attr1 = 32; // 12_03 and below
        }
        transformoldentities(hdr.version, e.type);
        if(e.type == PLAYERSTART && (e.attr2 == 0 || e.attr2 == 1 || e.attr2 == 100))
        {
            if(e.attr2 == 100)
                numspawn[2]++;
            else
                numspawn[e.attr2]++;
        }
        if(e.type == CTF_FLAG && (e.attr2 == 0 || e.attr2 == 1)) numflagspawn[e.attr2]++;
    }
    delete[] world;
    setupworld(hdr.sfactor);
    if(!mapinfo.numelems || (mapinfo.access(mname) && !cmpf(cgzname, mapinfo[mname]))) world = (sqr *)ents.getbuf();
    c2skeepalive();

    vector<uchar> rawcubes; // fetch whole file into buffer
    loopi(9)
    {
        ucharbuf q = rawcubes.reserve(cubicsize);
        q.len = f->read(q.buf, cubicsize);
        rawcubes.addbuf(q);
        if(q.len < cubicsize) break;
    }
    ucharbuf uf(rawcubes.getbuf(), rawcubes.length());
    rldecodecubes(uf, world, cubicsize, hdr.version, false); // decode file
    c2skeepalive();

    // calculate map statistics
    DELETEA(mlayout);
    mlayout = new char[cubicsize + 256];
    memset(mlayout, 0, cubicsize * sizeof(char));
    Mv = Ma = Hhits = 0;
    char texuse[256];
    loopi(256) texuse[i] = 0;
    loopk(8) mapdims[k] = k < 2 ? ssize : 0;
    loopk(cubicsize)
    {
        sqr *s = &world[k];
        if(SOLID(s)) mlayout[k] = 127;
        else
        {
            mlayout[k] = s->floor; // FIXME
            int diff = s->ceil - s->floor;
            if(diff > 6)
            {
                if(diff > MAXMHEIGHT) Hhits += diff - MAXMHEIGHT;
                Ma += 1;
                Mv += diff;
            }
            texuse[s->utex] = texuse[s->ftex] = texuse[s->ctex] = 1;
            int cwx = k%ssize,
                cwy = k/ssize;
            if(cwx < mapdims[0]) mapdims[0] = cwx;
            if(cwy < mapdims[1]) mapdims[1] = cwy;
            if(cwx > mapdims[2]) mapdims[2] = cwx;
            if(cwy > mapdims[3]) mapdims[3] = cwy;
            if(s->floor != -128 && s->floor < mapdims[6]) mapdims[6] = s->floor;
            if(s->ceil  > mapdims[7]) mapdims[7] = s->ceil;
        }
        texuse[s->wtex] = 1;
    }
    Mh = Ma ? (float)Mv/Ma : 0;
    loopk(2) mapdims[k+4] = mapdims[k+2] + 1 - mapdims[k]; // 8..15 ^= 8 cubes - minimal X/Y == 2 - != 0 !!
    c2skeepalive();

    calclight();
    conoutf("read map %s rev %d (%d milliseconds)", cgzname, hdr.maprevision, watch.stop());
    conoutf("%s", hdr.maptitle);
    pushscontext(IEXC_MAPCFG); // untrusted altogether
    per_idents = false;
    neverpersist = true;
    execfile("config/default_map_settings.cfg");
    execfile(pcfname);
    execfile(mcfname);
    copystring(lastloadedconfigfile, mcfname);
    parseheaderextra();
    neverpersist = false;
    per_idents = true;
    popscontext();

    c2skeepalive();

    watch.start();
    loopi(256) if(texuse[i]) lookupworldtexture(i, autodownload ? true : false);
    int texloadtime = watch.stop();

    c2skeepalive();

    watch.start();
    preload_mapmodels(autodownload ? true : false);
    int mdlloadtime = watch.stop();

    c2skeepalive();

    watch.start();
    audiomgr.preloadmapsounds(autodownload ? true : false);
    int audioloadtime = watch.stop();

    c2skeepalive();

    watch.start();
    int downloaded = downloadpackages();
    if(downloaded > 0) clientlogf("downloaded content (%d KB in %d seconds)", downloaded/1000, watch.stop()/1000);

    c2skeepalive();

    loadsky(NULL, true);

    watch.start();
    loopi(256) if(texuse[i]) lookupworldtexture(i, false);
    clientlogf("loaded textures (%d milliseconds)", texloadtime+watch.stop());
    c2skeepalive();
    watch.start();
    preload_mapmodels(false);
    clientlogf("loaded mapmodels (%d milliseconds)", mdlloadtime+watch.stop());
    c2skeepalive();
    watch.start();
    audiomgr.preloadmapsounds(false);
    clientlogf("loaded mapsounds (%d milliseconds)", audioloadtime+watch.stop());
    c2skeepalive();

    defformatstring(startmillis)("%d", millis_());
    alias("gametimestart", startmillis);
    startmap(mname);
    return true;
}

// support reading and writing binary data in config files

static bool hexbinenabled = false;
static vector<uchar> hexbin;

void hexbinwrite(stream *f, void *data, int len, bool ascii = true)   // write block of binary data as hex values with up to 24 bytes per line
{
    string asc;
    uchar *s = (uchar *) data;
    while(len > 0)
    {
        int chunk = min(len, 24);
        f->printf("hexbinchunk");
        loopi(chunk)
        {
            asc[i] = isalnum(*s) ? *s : '.'; asc[i + 1] = '\0';
            f->printf(" %02x", int(*s++));
        }
        f->printf(ascii ? "   // %s\n" : "\n", asc);
        len -= chunk;
    }
}

COMMANDF(hexbinchunk, "v", (char **args, int numargs)      // read up to 24 bytes from the command's arguments to the hexbin buffer
{
    if(!hexbinenabled) { conoutf("hexbinchunk out of context"); return; }
    loopi(numargs) hexbin.add(strtol(args[i], NULL, 16));
});


// multi-map editmode extensions (xmap)
//
// keep several (versions of) maps in memory at the same time
// compare maps and visualise differences
//
// the general assumption is, that ram is cheap, and that a copy of a size-9 map should be below 10MB (including undo data)

#define XMAPVERSION  1001001

VARP(persistentxmaps, 0, 1, 1);    // save all xmaps on exit, restore them at game start

struct xmap
{
    string nick;      // unique handle
    string name;
    vector<headerextra *> headerextras;
    vector<persistent_entity> ents;
    sqr *world;
    header hdr;
    int ssize, cubicsize, numundo;
    string mcfname;
    short position[5];

    xmap() : world(NULL), ssize(0), cubicsize(0), numundo(0) { *nick = *name = *mcfname = '\0'; }

    xmap(const char *nnick)    // take the current map and store it in an xmap
    {
        copystring(nick, nnick);
        copystring(name, getclientmap());
        hdr = ::hdr; ssize = ::ssize; cubicsize = ::cubicsize;
        loopv(::headerextras) headerextras.add(::headerextras[i]->duplicate());
        world = new sqr[cubicsize];
        memcpy(world, ::world, cubicsize * sizeof(sqr));
        loopv(::ents) if(::ents[i].type != NOTUSED) ents.add() = ::ents[i];
        copystring(mcfname, lastloadedconfigfile);   // may have been "official" or not
        storeposition(position);
        vector<uchar> tmp;  // add undo/redo data in compressed form as temporary headerextra
        numundo = backupeditundo(tmp, MAXHEADEREXTRA, MAXHEADEREXTRA);
        headerextras.add(new headerextra(tmp.length(), HX_EDITUNDO, tmp.getbuf()));
    }

    ~xmap()
    {
        headerextras.deletecontents();
        delete world;
        ents.setsize(0);
    }

    void restoreent(int i)
    {
        entity &e = ::ents.add();
        memcpy(&e, &ents[i], sizeof(persistent_entity));
        e.spawned = true;
    }

    void restore()      // overwrite the current map with the contents of an xmap
    {
        setnames(name);
        copystring(lastloadedconfigfile, mcfname);
        ::hdr = hdr;
        clearheaderextras();
        resetmap(true);
        curmaprevision = hdr.maprevision;
        setvar("waterlevel", hdr.waterlevel);
        ::ents.shrink(0);
        loopv(ents) restoreent(i);
        delete[] ::world;
        setupworld(hdr.sfactor);
        memcpy(::world, world, cubicsize * sizeof(sqr));
        calclight();  // includes full remip()
        loopv(headerextras) ::headerextras.add(headerextras[i]->duplicate());
        pushscontext(IEXC_MAPCFG); // untrusted altogether
        per_idents = false;
        neverpersist = true;
        execfile("config/default_map_settings.cfg");
        execfile(pcfname);
        execfile(lastloadedconfigfile);
        parseheaderextra();
        neverpersist = false;
        per_idents = true;
        popscontext();
        loadsky(NULL, true);
        startmap(name, false, true);      // "start" but don't respawn: basically just set clientmap
        restoreposition(position);
    }

    void write(stream *f)   // write xmap as cubescript to a file
    {
        f->printf("restorexmap version %d %d %d\n", XMAPVERSION, isbigendian() ? 1 : 0, sizeof(world));  // it is a non-portable binary file format, so we have to check...
        f->printf("restorexmap names \"%s\" \"%s\"\n", name, mcfname);
        f->printf("restorexmap sizes %d %d %d\n", ssize, cubicsize, numundo);
        hexbinwrite(f, &hdr, sizeof(header));
        f->printf("restorexmap header\n");
        uLongf worldsize = cubicsize * sizeof(sqr), gzbufsize = (worldsize * 11) / 10;     // gzip the world, so the file will only be big instead of huge
        uchar *gzbuf = new uchar[gzbufsize];
        if(compress2(gzbuf, &gzbufsize, (uchar *)world, worldsize, 9) != Z_OK) gzbufsize = 0;
        hexbinwrite(f, gzbuf, gzbufsize, false);
        f->printf("restorexmap world\n");
        DELETEA(gzbuf);
        loopv(headerextras)
        {
            hexbinwrite(f, headerextras[i]->data, headerextras[i]->len);
            f->printf("restorexmap headerextra %d  // %s\n", headerextras[i]->flags, hx_name(headerextras[i]->flags));
        }
        loopv(ents) // entities are stored as plain text - you may edit them
        {
            persistent_entity &e = ents[i];
            f->printf("restorexmap ent %d  %d %d %d  %d %d %d %d // %s\n", e.type, e.x, e.y, e.z, e.attr1, e.attr2, e.attr3, e.attr4, e.type >= 0 && e.type < MAXENTTYPES ? entnames[e.type] : "unknown");
        }
        f->printf("restorexmap position %d %d %d %d %d  // EOF, don't touch this\n\n", int(position[0]), int(position[1]), int(position[2]), int(position[3]), int(position[4]));
    }
};

static vector<xmap *> xmaps;
static xmap *bak, *xmjigsaw;                               // only bak needs to be deleted before reuse

#define SPEDIT if(noteditmode("xmap") || multiplayer()) return    // only allowed in non-coop editmode
#define SPEDITDIFF if(noteditmode("xmap") || multiplayer() || nodiff()) return    // only allowed in non-coop editmode after xmap_diff

bool validxmapname(const char *nick) { if(validmapname(nick)) return true; conoutf("sry, %s is not a valid xmap nickname", nick); return false; }

void xmapdelete(xmap *&xm)  // make sure, we don't point to deleted xmaps
{
    DELETEP(xm);
}

void xmapbackup(const char *nickprefix, const char *nick)   // throw away existing backup and make a new one
{
    xmapdelete(bak);
    defformatstring(text)("%s%s", nickprefix, nick);
    bak = new xmap(text);
    if(unsavededits) conoutf("\f3stored backup of unsaved edits on map '%s'; to restore, type \"/xmap_restore\"", bak->name);
}

const char *xmapdescstring(xmap *xm, bool shortform = false)
{
    static string s[2];
    static int toggle;
    toggle = !toggle;
    formatstring(s[toggle])("\"%s\": %s rev %d, %d ents%c size %d, hdrs %d, %d undo steps", xm->nick, xm->name, xm->hdr.maprevision, xm->ents.length(),
                        shortform ? '\0' : ',', xm->hdr.sfactor, xm->headerextras.length() - 1, xm->numundo);
    return s[toggle];
}

xmap *getxmapbynick(const char *nick, int *index = NULL, bool errmsg = true)
{
    if(*nick) loopvrev(xmaps)
    {
        if(!strcmp(nick, xmaps[i]->nick))
        {
            if(index) *index = i;
            return xmaps[i];
        }
    }
    if(errmsg) conoutf("xmap \"%s\" not found", nick);
    return NULL;
}

COMMANDF(xmap_list, "", ()                       // list xmaps (and status)
{
    loopv(xmaps) conoutf("xmap %d %s", i, xmapdescstring(xmaps[i]));
    if(xmaps.length() == 0) conoutf("no xmaps in memory");
    if(bak) conoutf("backup stored before %s", xmapdescstring(bak));
} );

COMMANDF(xmap_store, "s", (const char *nick)     // store current map in an xmap buffer
{
    if(noteditmode("xmap_store")) return;   // (may also be used in coopedit)
    if(!*nick || !validxmapname(nick)) return;
    int i;
    if(getxmapbynick(nick, &i, false)) { xmap *xm = xmaps.remove(i); xmapdelete(xm); }   // overwrite existing same nick
    xmaps.add(new xmap(nick));
    unsavededits = 0;
    conoutf("stored xmap %s", xmapdescstring(xmaps.last()));
} );

COMMANDF(xmap_delete, "s", (const char *nick)     // delete xmap buffer
{
    if(!*nick) return;
    int i;
    if(getxmapbynick(nick, &i))
    {
        xmap *xm = xmaps.remove(i);
        xmapdelete(xm);
        conoutf("deleted xmap \"%s\"", nick);
    }
} );

COMMANDF(xmap_delete_backup, "", () { if(bak) { xmapdelete(bak); conoutf("deleted backup xmap"); }} );

COMMANDF(xmap_keep_backup, "s", (const char *nick)     // move backup xmap to more permanent position
{
    if(!bak) { conoutf("no backup xmap available"); return ; }
    defformatstring(bakdesc)("%s", bak->nick);
    if(*nick)
    {
        if(validxmapname(nick)) copystring(bak->nick, nick);
        else return;
    }
    loopi(42) if(getxmapbynick(bak->nick, NULL, false)) concatstring(bak->nick, "_");  // evade existing nicknames
    xmaps.add(bak);
    bak = NULL;
    conoutf("stored backup xmap \"%s\" as xmap %s", bakdesc, xmapdescstring(xmaps.last()));
} );

COMMANDF(xmap_rename, "ss", (const char *oldnick, const char *newnick)     // rename xmap buffer
{
    if(!*oldnick || !*newnick) return;
    xmap *xm = getxmapbynick(oldnick);
    if(!xm || !validxmapname(newnick) || getxmapbynick(newnick, NULL, false)) return;
    copystring(xm->nick, newnick);
} );

COMMANDF(xmap_restore, "s", (const char *nick)     // use xmap as current map
{
    SPEDIT;
    if(*nick)
    {
        xmap *xm = getxmapbynick(nick);
        if(!xm) return;
        xmapbackup("restore_xmap_", nick);  // keep backup, so we can restore from the restore :)
        xm->restore();
        unsavededits = 0;
        conoutf("restored xmap %s", xmapdescstring(xm));
    }
    else if(bak)
    {
        bak->restore();
        unsavededits = 0;
        conoutf("restored backup created before %s", xmapdescstring(bak));
    }
} );

void restorexmap(char **args, int numargs)   // read an xmap from a cubescript file
{
    const char *cmdnames[] = { "version", "names", "sizes", "header", "world", "headerextra", "ent", "position" };
    const char cmdnumarg[] = {         3,       2,       3,        0,       0,             1,     8,          5 };

    if(!xmjigsaw || numargs < 1) return; // { conoutf("restorexmap out of context"); return; }
    bool abort = false;
    int cmd = -1;
    loopi(sizeof(cmdnames)/sizeof(cmdnames[0])) if(!strcmp(cmdnames[i], args[0])) { cmd = i; break; }
    if(cmd < 0 || numargs != cmdnumarg[cmd] + 1) { conoutf("restorexmap error"); return; }
    switch(cmd)
    {
        case 0:     // version
            if(ATOI(args[1]) != XMAPVERSION || ATOI(args[2]) != (isbigendian() ? 1 : 0) || ATOI(args[3]) != sizeof(world))
            {
                conoutf("restorexmap: file is from different game version");
                abort = true;
            }
            break;
        case 1:     // names
            copystring(xmjigsaw->name, args[1]);
            copystring(xmjigsaw->mcfname, args[2]);
            break;
        case 2:     // sizes
            xmjigsaw->ssize = ATOI(args[1]);
            xmjigsaw->cubicsize = ATOI(args[2]);
            xmjigsaw->numundo = ATOI(args[3]);
            break;
        case 3:     // header
            if(hexbin.length() == sizeof(header)) memcpy(&xmjigsaw->hdr, hexbin.getbuf(), sizeof(header));
            else abort = true;
            break;
        case 4:     // world
        {
            int rawworldsize = xmjigsaw->cubicsize * sizeof(sqr);
            xmjigsaw->world = new sqr[rawworldsize];
            uLongf rawsize = rawworldsize;
            if(uncompress((uchar *)xmjigsaw->world, &rawsize, hexbin.getbuf(), hexbin.length()) != Z_OK || rawsize - rawworldsize != 0) abort = true;
            break;
        }
        case 5:     // headerextra
            xmjigsaw->headerextras.add(new headerextra(hexbin.length(), ATOI(args[1]), hexbin.getbuf()));
            break;
        case 6:     // ent
        {
            persistent_entity &e = xmjigsaw->ents.add();
            int a[8];
            loopi(8) a[i] = ATOI(args[i + 1]);
            e.type = a[0]; e.x = a[1]; e.y = a[2]; e.z = a[3]; e.attr1 = a[4]; e.attr2 = a[5]; e.attr3 = a[6]; e.attr4 = a[7];
            break;
        }
        case 7:     // position - this is also the last command and will finish the xmap
        {
            loopi(5) xmjigsaw->position[i] = ATOI(args[i + 1]);
            int i;
            if(getxmapbynick(xmjigsaw->nick, &i, false)) { xmap *xm = xmaps.remove(i); xmapdelete(xm); }   // overwrite existing same nick
            if(!xmjigsaw->world) abort = true;
            else
            {
                xmaps.add(xmjigsaw);
                xmjigsaw = NULL;  // no abort!
            }
        }
    }
    hexbin.setsize(0);
    if(abort) DELETEP(xmjigsaw);
}
COMMAND(restorexmap, "v");

static const char *bakprefix = "(backup)", *xmapspath = "mapediting/xmaps";
char *xmapfilename(const char *nick, const char *prefix = "") { static defformatstring(fn)("%s/%s%s.xmap", xmapspath, prefix, nick); return path(fn); }

void writexmap(xmap *xm, const char *prefix = "")
{
    stream *f = openfile(xmapfilename(xm->nick, prefix), "w");
    if(f)
    {
        xm->write(f);
        delete f;
    }
}

void writeallxmaps()   // at game exit, write all xmaps to cubescript files in mapediting/xmaps
{
    if(!persistentxmaps) return;
    loopv(xmaps) writexmap(xmaps[i]);
    if(unsavededits) xmapbackup("gameend", "");
    if(bak) writexmap(bak, bakprefix);
}

void loadxmap(const char *nick)
{
    DELETEP(xmjigsaw);
    xmjigsaw = new xmap();
    copystring(xmjigsaw->nick, nick);
    hexbinenabled = true;
    execfile(xmapfilename(nick));
    hexbinenabled = false;
    hexbin.setsize(0);    // just to make sure
}

int loadallxmaps()     // at game start, load all xmaps from mapediting/xmaps
{
    vector<char *> xmapnames;
    string filename;
    listfiles(xmapspath, "xmap", xmapnames);
    xmapnames.sort(stringsort);
    loopv(xmapnames)
    {
        loadxmap(xmapnames[i]);
        copystring(filename, xmapfilename(xmapnames[i]));
        backup(filename, xmapfilename(xmapnames[i], "backups/"));   // move to backup folder - we will write new files at exit
        delete[] xmapnames[i];
    }
    loopv(xmaps) if(!strncmp(xmaps[i]->nick, bakprefix, strlen(bakprefix)))
    {
        xmapdelete(bak);
        bak = xmaps.remove(i);
        copystring(bak->nick, bak->nick + strlen(bakprefix));
        break;
    }
    return xmapnames.length();
}
