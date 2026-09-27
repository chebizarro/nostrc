/* ns-strip-isobmff.c - ISO base media file format metadata stripper
 *
 * SPDX-License-Identifier: MIT
 *
 * Bead nostrc-wu3s. MP4, QuickTime, 3GP and M4A files and HEIF / HEIC /
 * AVIF images are ISO-BMFF box trees whose sample tables (stco / co64)
 * and item locations (iloc) hold absolute file offsets. Removing bytes
 * would mean rewriting every one of them (re-muxing), so this stripper
 * never changes a box's size or position:
 *
 *   * a metadata box becomes a `free` box of the same size with its
 *     payload zeroed — udta (©xyz GPS, ©mak/©mod device, ©day, iTunes /
 *     Samsung / GoPro user data), `meta` inside moov / trak and at the top
 *     of a movie (QuickTime `mdta` keys: location.ISO6709, make, model,
 *     software, creationdate), XMP `uuid` boxes anywhere, and every
 *     unknown top-level box (camera `uuid`s, JUMBF / C2PA manifests,
 *     QuickTime previews);
 *   * existing free / skip / wide boxes are zeroed (editors leave stale
 *     metadata in them);
 *   * mvhd / tkhd / mdhd creation and modification times are zeroed;
 *   * a timed-metadata track (handler `meta`: GoPro GPMF GPS, Apple
 *     `mebx`, CAMM) has every sample zeroed in mdat and its `trak`
 *     becomes `free`;
 *   * in HEIF / AVIF, Exif items are overwritten with an empty, valid
 *     Exif block and XMP (`mime` application/rdf+xml) items with spaces,
 *     wherever iloc puts them (mdat or idat); the item structure and the
 *     image data are untouched;
 *   * bytes after the last complete top-level box of an otherwise valid
 *     file (vendor trailers such as Samsung's SEF) are dropped — the only
 *     change of length, which moves no offset.
 *
 * Anything that cannot be walked completely is MALFORMED (never a partial
 * clean); constructs it does not handle — a compressed movie header
 * (cmov), fragmented timed metadata, external or item-relative iloc data,
 * compact sample sizes in a metadata track — are UNSUPPORTED, i.e. left
 * to the user's explicit --keep-metadata.
 */
#include "ns-strip-isobmff.h"

#include <string.h>

#define FOURCC(a, b, c, d) \
  (((guint32)(a) << 24) | ((guint32)(b) << 16) | ((guint32)(c) << 8) | (guint32)(d))

static const guint8 XMP_UUID[16] = { 0xBE, 0x7A, 0xCF, 0xCB, 0x97, 0xA9, 0x42, 0xE8,
                                     0x9C, 0x71, 0x99, 0x94, 0x91, 0xE3, 0xAF, 0xAC };

static guint16 be16(const guint8 *p) { return (guint16)((p[0] << 8) | p[1]); }
static guint32 be32(const guint8 *p)
{
  return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8) | p[3];
}
static guint64 be64(const guint8 *p) { return ((guint64)be32(p) << 32) | be32(p + 4); }

static guint64
read_n(const guint8 *p, guint n)
{
  guint64 v = 0;
  for (guint i = 0; i < n; i++)
    v = (v << 8) | p[i];
  return v;
}

gboolean
ns_isobmff_mime(const gchar *mime, gboolean *is_image)
{
  static const gchar *const video[] = {
    "video/mp4", "video/quicktime", "video/3gpp", "video/3gpp2", "video/x-m4v",
    "audio/mp4", "audio/x-m4a", "audio/m4a", "audio/3gpp", NULL
  };
  static const gchar *const image[] = {
    "image/heic", "image/heif", "image/avif", "image/heic-sequence",
    "image/heif-sequence", "image/avif-sequence", NULL
  };
  if (mime == NULL)
    return FALSE;
  for (guint i = 0; video[i]; i++)
    if (g_str_equal(mime, video[i])) {
      if (is_image) *is_image = FALSE;
      return TRUE;
    }
  for (guint i = 0; image[i]; i++)
    if (g_str_equal(mime, image[i])) {
      if (is_image) *is_image = TRUE;
      return TRUE;
    }
  return FALSE;
}

/* ---- Boxes ---- */

typedef struct {
  gsize   off;       /* start of the box */
  guint64 size;      /* whole box */
  gsize   hdr;       /* header bytes (incl. largesize and uuid type) */
  guint32 type;
} Box;

/* Parse the box at @off inside [@off, @end). */
static gboolean
box_at(const guint8 *d, gsize off, gsize end, Box *b)
{
  if (end < off || end - off < 8)
    return FALSE;
  guint64 size = be32(d + off);
  b->type = be32(d + off + 4);
  b->hdr = 8;
  if (size == 1) {
    if (end - off < 16)
      return FALSE;
    size = be64(d + off + 8);
    b->hdr = 16;
  } else if (size == 0) {
    size = end - off;          /* "extends to the end of the container" */
  }
  if (b->type == FOURCC('u', 'u', 'i', 'd'))
    b->hdr += 16;
  if (size < b->hdr || size > end - off)
    return FALSE;
  b->off = off;
  b->size = size;
  return TRUE;
}

typedef struct {
  guint8   *d;
  gsize     len;
  gboolean  is_image;
  guint     removed;
  gboolean  unsupported;
  gboolean  have_moov, have_meta, fragmented;
} Iso;

/* The box becomes `free` of the same size, payload zeroed. */
static void
neutralize(Iso *x, const Box *b)
{
  gsize type_at = b->off + 4;
  x->d[type_at + 0] = 'f';
  x->d[type_at + 1] = 'r';
  x->d[type_at + 2] = 'e';
  x->d[type_at + 3] = 'e';
  /* A uuid box's extended type is payload now. */
  gsize keep = b->hdr - (b->type == FOURCC('u', 'u', 'i', 'd') ? 16 : 0);
  memset(x->d + b->off + keep, 0, (gsize)b->size - keep);
  x->removed++;
}

static gboolean
zero_range(Iso *x, guint64 start, guint64 length)
{
  if (start > x->len || length > x->len - start)
    return FALSE;
  memset(x->d + start, 0, (gsize)length);
  return TRUE;
}

/* mvhd / tkhd / mdhd: creation_time + modification_time follow the
 * version/flags word; 32-bit in version 0, 64-bit in version 1. */
static gboolean
zero_times(Iso *x, const Box *b)
{
  gsize p = b->off + b->hdr;
  gsize need = 4 + 8;
  if (b->size - b->hdr < need)
    return FALSE;
  guint8 version = x->d[p];
  gsize n = version == 1 ? 16 : 8;
  if (b->size - b->hdr < 4 + n)
    return FALSE;
  gboolean any = FALSE;
  for (gsize i = 0; i < n; i++)
    any |= x->d[p + 4 + i] != 0;
  memset(x->d + p + 4, 0, n);
  if (any)
    x->removed++;
  return TRUE;
}

/* ---- Timed-metadata tracks ---- */

typedef struct {
  guint32  handler;
  gboolean stz2;
  Box      stsz, stsc, stco;
  gboolean have_stsz, have_stsc, have_stco;
  gboolean co64;
} Trak;

static gboolean walk(Iso *x, gsize start, gsize end, guint depth, guint32 parent, Trak *trak);

/* Zero every sample of a track from its sample tables. */
static gboolean
zero_samples(Iso *x, const Trak *t)
{
  if (t->stz2 || !t->have_stsz || !t->have_stsc || !t->have_stco) {
    x->unsupported = TRUE;
    return TRUE;
  }
  const guint8 *sz = x->d + t->stsz.off + t->stsz.hdr;
  guint64 szlen = t->stsz.size - t->stsz.hdr;
  const guint8 *sc = x->d + t->stsc.off + t->stsc.hdr;
  guint64 sclen = t->stsc.size - t->stsc.hdr;
  const guint8 *co = x->d + t->stco.off + t->stco.hdr;
  guint64 colen = t->stco.size - t->stco.hdr;
  if (szlen < 12 || sclen < 8 || colen < 8)
    return FALSE;
  guint32 fixed = be32(sz + 4), n_samples = be32(sz + 8);
  if (fixed == 0 && (szlen - 12) / 4 < n_samples)
    return FALSE;
  guint32 n_sc = be32(sc + 4);
  if ((sclen - 8) / 12 < n_sc)
    return FALSE;
  guint32 n_chunks = be32(co + 4);
  guint esz = t->co64 ? 8 : 4;
  if ((colen - 8) / esz < n_chunks)
    return FALSE;

  guint32 sample = 0;
  for (guint32 c = 1; c <= n_chunks && sample < n_samples; c++) {
    guint32 per_chunk = 0;
    for (guint32 e = 0; e < n_sc; e++) {
      guint32 first = be32(sc + 8 + 12 * e);
      if (first > c)
        break;
      per_chunk = be32(sc + 8 + 12 * e + 4);
    }
    guint64 off = t->co64 ? be64(co + 8 + 8 * (c - 1)) : be32(co + 8 + 4 * (c - 1));
    for (guint32 s = 0; s < per_chunk && sample < n_samples; s++, sample++) {
      guint64 size = fixed != 0 ? fixed : be32(sz + 12 + 4 * sample);
      if (!zero_range(x, off, size))
        return FALSE;
      off += size;
    }
  }
  return sample == n_samples;
}

/* ---- HEIF items ---- */

typedef struct {
  guint32  id;
  gboolean exif;
  gboolean xmp;
} Item;

/* infe (item info entry) → item id and whether it is Exif or XMP. */
static gboolean
parse_infe(const guint8 *d, const Box *b, Item *it)
{
  gsize p = b->off + b->hdr, end = b->off + b->size;
  if (end - p < 4)
    return FALSE;
  guint8 version = d[p];
  p += 4;
  memset(it, 0, sizeof(*it));
  if (version >= 2) {
    gsize idlen = version == 2 ? 2 : 4;
    if (end - p < idlen + 2 + 4)
      return FALSE;
    it->id = (guint32)read_n(d + p, (guint)idlen);
    p += idlen + 2;                      /* item_ID, item_protection_index */
    guint32 type = be32(d + p);
    p += 4;
    const guint8 *nul = memchr(d + p, 0, end - p);   /* item_name */
    if (nul == NULL)
      return FALSE;
    p = (gsize)(nul - d) + 1;
    if (type == FOURCC('E', 'x', 'i', 'f')) {
      it->exif = TRUE;
    } else if (type == FOURCC('m', 'i', 'm', 'e')) {
      const guint8 *ct_end = memchr(d + p, 0, end - p);
      if (ct_end == NULL)
        return FALSE;
      g_autofree gchar *ct = g_ascii_strdown((const gchar *)d + p, ct_end - (d + p));
      it->xmp = strstr(ct, "rdf+xml") != NULL || strstr(ct, "xmp") != NULL;
    }
  } else {
    /* Versions 0/1: item_ID, protection, item_name, content_type. */
    if (end - p < 4)
      return FALSE;
    it->id = be16(d + p);
    p += 4;
    const guint8 *nul = memchr(d + p, 0, end - p);
    if (nul == NULL)
      return FALSE;
    p = (gsize)(nul - d) + 1;
    const guint8 *ct_end = memchr(d + p, 0, end - p);
    if (ct_end != NULL) {
      g_autofree gchar *ct = g_ascii_strdown((const gchar *)d + p, ct_end - (d + p));
      it->xmp = strstr(ct, "rdf+xml") != NULL || strstr(ct, "xmp") != NULL;
    }
  }
  return TRUE;
}

/* Overwrite one metadata item's bytes: Exif with an empty but valid Exif
 * block (so readers see "no Exif", not a corrupt one), XMP with spaces. */
static void
scrub(guint8 *p, guint64 n, gboolean exif)
{
  static const guint8 EMPTY_EXIF[18] = {
    0, 0, 0, 0,                 /* exif_tiff_header_offset */
    'M', 'M', 0, 42, 0, 0, 0, 8,  /* TIFF header, IFD0 at 8 */
    0, 0,                       /* 0 entries */
    0, 0, 0, 0                  /* no next IFD */
  };
  if (exif) {
    memset(p, 0, (gsize)n);
    memcpy(p, EMPTY_EXIF, MIN((gsize)n, sizeof(EMPTY_EXIF)));
  } else {
    memset(p, ' ', (gsize)n);
  }
}

/* A top-level `meta`: find the Exif / XMP items (iinf) and scrub their
 * bytes wherever iloc says they are (file offsets or this meta's idat). */
static gboolean
scrub_meta_items(Iso *x, const Box *meta)
{
  gsize p = meta->off + meta->hdr, end = meta->off + meta->size;
  if (end - p < 4)
    return FALSE;
  p += 4;                                        /* FullBox version/flags */
  Box iinf = { 0 }, iloc = { 0 }, idat = { 0 };
  gboolean has_iinf = FALSE, has_iloc = FALSE, has_idat = FALSE;
  for (gsize o = p; o < end;) {
    Box c;
    if (!box_at(x->d, o, end, &c))
      return FALSE;
    if (c.type == FOURCC('i', 'i', 'n', 'f')) { iinf = c; has_iinf = TRUE; }
    else if (c.type == FOURCC('i', 'l', 'o', 'c')) { iloc = c; has_iloc = TRUE; }
    else if (c.type == FOURCC('i', 'd', 'a', 't')) { idat = c; has_idat = TRUE; }
    else if (c.type == FOURCC('u', 'u', 'i', 'd') &&
             memcmp(x->d + c.off + c.hdr - 16, XMP_UUID, 16) == 0)
      neutralize(x, &c);
    o += (gsize)c.size;
  }
  if (!has_iinf)
    return !x->is_image;                         /* an image needs its items */

  GArray *items = g_array_new(FALSE, FALSE, sizeof(Item));
  gboolean ok = FALSE;
  gsize q = iinf.off + iinf.hdr, iend = iinf.off + iinf.size;
  if (iend - q < 6)
    goto out;
  guint8 iv = x->d[q];
  q += 4 + (iv == 0 ? 2 : 4);                    /* entry_count */
  if (q > iend)
    goto out;
  while (q < iend) {
    Box e;
    if (!box_at(x->d, q, iend, &e))
      goto out;
    Item it;
    if (e.type == FOURCC('i', 'n', 'f', 'e')) {
      if (!parse_infe(x->d, &e, &it))
        goto out;
      if (it.exif || it.xmp)
        g_array_append_val(items, it);
    }
    q += (gsize)e.size;
  }
  if (items->len == 0) {
    ok = TRUE;
    goto out;
  }
  if (!has_iloc)
    goto out;

  /* iloc: find each metadata item's extents. */
  q = iloc.off + iloc.hdr;
  iend = iloc.off + iloc.size;
  if (iend - q < 8)
    goto out;
  guint8 lv = x->d[q];
  if (lv > 2)
    goto out;
  q += 4;
  guint off_sz = x->d[q] >> 4, len_sz = x->d[q] & 0x0F;
  guint base_sz = x->d[q + 1] >> 4, idx_sz = lv >= 1 ? (x->d[q + 1] & 0x0F) : 0;
  q += 2;
  for (guint s = 0; s < 4; s++) {
    guint v = s == 0 ? off_sz : s == 1 ? len_sz : s == 2 ? base_sz : idx_sz;
    if (v != 0 && v != 4 && v != 8)
      goto out;
  }
  guint32 count;
  if (lv < 2) {
    if (iend - q < 2) goto out;
    count = be16(x->d + q);
    q += 2;
  } else {
    if (iend - q < 4) goto out;
    count = be32(x->d + q);
    q += 4;
  }
  for (guint32 i = 0; i < count; i++) {
    gsize idlen = lv < 2 ? 2 : 4;
    gsize fixed = idlen + (lv >= 1 ? 2 : 0) + 2 + base_sz + 2;
    if (iend - q < fixed)
      goto out;
    guint32 id = (guint32)read_n(x->d + q, (guint)idlen);
    q += idlen;
    guint method = 0;
    if (lv >= 1) {
      method = be16(x->d + q) & 0x0F;
      q += 2;
    }
    guint16 dref = be16(x->d + q);
    q += 2;
    guint64 base = read_n(x->d + q, base_sz);
    q += base_sz;
    guint16 n_ext = be16(x->d + q);
    q += 2;
    const Item *meta_item = NULL;
    for (guint k = 0; k < items->len; k++)
      if (g_array_index(items, Item, k).id == id)
        meta_item = &g_array_index(items, Item, k);
    for (guint16 e = 0; e < n_ext; e++) {
      gsize elen = idx_sz + off_sz + len_sz;
      if (iend - q < elen)
        goto out;
      guint64 eoff = read_n(x->d + q + idx_sz, off_sz);
      guint64 elength = read_n(x->d + q + idx_sz + off_sz, len_sz);
      q += elen;
      if (meta_item == NULL)
        continue;
      if (dref != 0 || method == 2 || elength == 0) {
        x->unsupported = TRUE;             /* external / item-relative / "whole" */
        continue;
      }
      guint64 start;
      if (method == 0) {
        if (base > G_MAXUINT64 - eoff)
          goto out;
        start = base + eoff;
      } else {                               /* method 1: offset into idat */
        if (!has_idat)
          goto out;
        guint64 payload = idat.size - idat.hdr;
        if (base > G_MAXUINT64 - eoff || base + eoff > payload ||
            elength > payload - (base + eoff))
          goto out;
        start = idat.off + idat.hdr + base + eoff;
      }
      if (start > x->len || elength > x->len - start)
        goto out;
      scrub(x->d + start, elength, meta_item->exif);
      x->removed++;
    }
  }
  ok = TRUE;
out:
  g_array_unref(items);
  return ok;
}

/* ---- Tree walk ---- */

static gboolean
is_container(guint32 t)
{
  switch (t) {
  case FOURCC('m', 'o', 'o', 'v'): case FOURCC('t', 'r', 'a', 'k'):
  case FOURCC('m', 'd', 'i', 'a'): case FOURCC('m', 'i', 'n', 'f'):
  case FOURCC('s', 't', 'b', 'l'): case FOURCC('e', 'd', 't', 's'):
  case FOURCC('d', 'i', 'n', 'f'): case FOURCC('m', 'v', 'e', 'x'):
  case FOURCC('m', 'o', 'o', 'f'): case FOURCC('t', 'r', 'a', 'f'):
    return TRUE;
  default:
    return FALSE;
  }
}

/* Top-level boxes a movie or HEIF file may carry; anything else is
 * someone's metadata (camera uuids, JUMBF/C2PA, previews): dropped. */
static gboolean
top_level_known(guint32 t)
{
  switch (t) {
  case FOURCC('f', 't', 'y', 'p'): case FOURCC('s', 't', 'y', 'p'):
  case FOURCC('m', 'o', 'o', 'v'): case FOURCC('m', 'd', 'a', 't'):
  case FOURCC('m', 'e', 't', 'a'): case FOURCC('m', 'o', 'o', 'f'):
  case FOURCC('m', 'f', 'r', 'a'): case FOURCC('s', 'i', 'd', 'x'):
  case FOURCC('s', 's', 'i', 'x'): case FOURCC('p', 'r', 'f', 't'):
  case FOURCC('e', 'm', 's', 'g'): case FOURCC('p', 'd', 'i', 'n'):
  case FOURCC('f', 'r', 'e', 'e'): case FOURCC('s', 'k', 'i', 'p'):
  case FOURCC('w', 'i', 'd', 'e'):
    return TRUE;
  default:
    return FALSE;
  }
}

static gboolean
walk(Iso *x, gsize start, gsize end, guint depth, guint32 parent, Trak *trak)
{
  if (depth > 16)
    return FALSE;
  for (gsize off = start; off < end;) {
    Box b;
    if (!box_at(x->d, off, end, &b))
      return FALSE;
    gsize next = off + (gsize)b.size;
    gsize payload = b.off + b.hdr;

    switch (b.type) {
    case FOURCC('f', 'r', 'e', 'e'):
    case FOURCC('s', 'k', 'i', 'p'):
    case FOURCC('w', 'i', 'd', 'e'):
      memset(x->d + payload, 0, (gsize)b.size - b.hdr);
      break;
    case FOURCC('u', 'd', 't', 'a'):
      neutralize(x, &b);
      break;
    case FOURCC('c', 'm', 'o', 'v'):
      x->unsupported = TRUE;               /* zlib'd movie header */
      break;
    case FOURCC('u', 'u', 'i', 'd'):
      if (depth == 0 || memcmp(x->d + b.off + b.hdr - 16, XMP_UUID, 16) == 0)
        neutralize(x, &b);
      break;
    case FOURCC('m', 'e', 't', 'a'):
      if (depth == 0) {
        x->have_meta = TRUE;
        if (!scrub_meta_items(x, &b))
          return FALSE;
        if (!x->is_image)
          neutralize(x, &b);               /* a movie's top-level tags */
      } else {
        neutralize(x, &b);                 /* moov/trak meta: mdta keys, ilst */
      }
      break;
    case FOURCC('m', 'v', 'h', 'd'):
    case FOURCC('t', 'k', 'h', 'd'):
    case FOURCC('m', 'd', 'h', 'd'):
      if (!zero_times(x, &b))
        return FALSE;
      break;
    case FOURCC('h', 'd', 'l', 'r'):
      if (trak != NULL && parent == FOURCC('m', 'd', 'i', 'a')) {
        if (b.size - b.hdr < 12)
          return FALSE;
        trak->handler = be32(x->d + payload + 8);
      }
      break;
    case FOURCC('s', 't', 's', 'z'):
      if (trak) { trak->stsz = b; trak->have_stsz = TRUE; }
      break;
    case FOURCC('s', 't', 'z', '2'):
      if (trak) trak->stz2 = TRUE;
      break;
    case FOURCC('s', 't', 's', 'c'):
      if (trak) { trak->stsc = b; trak->have_stsc = TRUE; }
      break;
    case FOURCC('s', 't', 'c', 'o'):
    case FOURCC('c', 'o', '6', '4'):
      if (trak) {
        trak->stco = b;
        trak->have_stco = TRUE;
        trak->co64 = b.type == FOURCC('c', 'o', '6', '4');
      }
      break;
    case FOURCC('m', 'o', 'o', 'v'):
      x->have_moov = TRUE;
      if (!walk(x, payload, next, depth + 1, b.type, NULL))
        return FALSE;
      break;
    case FOURCC('m', 'o', 'o', 'f'):
      if (!walk(x, payload, next, depth + 1, b.type, trak))
        return FALSE;
      break;
    case FOURCC('t', 'r', 'a', 'k'): {
      Trak t = { 0 };
      if (!walk(x, payload, next, depth + 1, b.type, &t))
        return FALSE;
      if (t.handler == FOURCC('m', 'e', 't', 'a')) {
        /* Timed metadata (GPS streams): zero the samples, drop the track. */
        if (x->fragmented) {
          x->unsupported = TRUE;
        } else {
          if (!zero_samples(x, &t))
            return FALSE;
          neutralize(x, &b);
        }
      }
      break;
    }
    default:
      if (depth == 0 && !top_level_known(b.type))
        neutralize(x, &b);
      else if (is_container(b.type) &&
               !walk(x, payload, next, depth + 1, b.type, trak))
        return FALSE;
      break;
    }
    off = next;
  }
  return TRUE;
}

NsStripResult
ns_strip_isobmff(gboolean is_image, const guint8 *data, gsize len, GBytes **out,
                 guint *n_removed)
{
  Box first;
  if (!box_at(data, 0, len, &first))
    return NS_STRIP_MALFORMED;
  Iso x = { 0 };
  x.d = g_memdup2(data, len);
  x.len = len;
  x.is_image = is_image;

  /* Top level: a box parse failure after a complete movie / image is a
   * vendor trailer (dropped); anywhere else the file is malformed. */
  gsize end = len;
  for (gsize off = 0; off < len;) {
    Box b;
    if (!box_at(x.d, off, len, &b)) {
      if ((is_image ? x.have_meta : x.have_moov) && off > 0) {
        end = off;
        x.removed++;
        break;
      }
      goto malformed;
    }
    if (b.type == FOURCC('m', 'o', 'o', 'v'))
      x.have_moov = TRUE;
    if (b.type == FOURCC('m', 'e', 't', 'a'))
      x.have_meta = TRUE;
    /* Known before any trak is judged: a fragmented movie's samples live
     * in moof/mdat pairs that the moov sample tables do not describe. */
    if (b.type == FOURCC('m', 'o', 'o', 'f'))
      x.fragmented = TRUE;
    off += (gsize)b.size;
  }
  x.have_moov = x.have_meta = FALSE;
  if (!walk(&x, 0, end, 0, 0, NULL))
    goto malformed;
  if (is_image ? !x.have_meta : !x.have_moov)
    goto malformed;
  if (x.unsupported) {
    g_free(x.d);
    return NS_STRIP_UNSUPPORTED;
  }
  if (n_removed)
    *n_removed = x.removed;
  *out = g_bytes_new_take(x.d, end);
  return NS_STRIP_OK;

malformed:
  g_free(x.d);
  return NS_STRIP_MALFORMED;
}

/* ---- Dimensions ---- */

static void
max_dims(const guint8 *d, gsize start, gsize end, guint depth, gboolean is_image,
         guint *w, guint *h)
{
  if (depth > 8)
    return;
  for (gsize off = start; off < end;) {
    Box b;
    if (!box_at(d, off, end, &b))
      return;
    gsize p = b.off + b.hdr;
    gsize n = (gsize)b.size - b.hdr;
    guint cw = 0, ch = 0;
    if (is_image && b.type == FOURCC('i', 's', 'p', 'e') && n >= 12) {
      cw = be32(d + p + 4);
      ch = be32(d + p + 8);
    } else if (!is_image && b.type == FOURCC('t', 'k', 'h', 'd') && n >= 84) {
      gsize at = d[p] == 1 ? 88 : 76;       /* 16.16 fixed width, height */
      if (n >= at + 8) {
        cw = be32(d + p + at) >> 16;
        ch = be32(d + p + at + 4) >> 16;
      }
    } else if (b.type == FOURCC('m', 'e', 't', 'a') && n >= 4) {
      max_dims(d, p + 4, off + (gsize)b.size, depth + 1, is_image, w, h);
    } else if (is_container(b.type) || b.type == FOURCC('i', 'p', 'r', 'p') ||
               b.type == FOURCC('i', 'p', 'c', 'o')) {
      max_dims(d, p, off + (gsize)b.size, depth + 1, is_image, w, h);
    }
    if ((guint64)cw * ch > (guint64)*w * *h) {
      *w = cw;
      *h = ch;
    }
    off += (gsize)b.size;
  }
}

gboolean
ns_isobmff_dimensions(gboolean is_image, const guint8 *data, gsize len, guint *width,
                      guint *height)
{
  guint w = 0, h = 0;
  max_dims(data, 0, len, 0, is_image, &w, &h);
  if (w == 0 || h == 0)
    return FALSE;
  *width = w;
  *height = h;
  return TRUE;
}
