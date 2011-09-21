#include "fitz.h"
#include "mupdf.h"

void
pdf_free_link(fz_context *ctx, pdf_link *link)
{
	pdf_link *next;

	do
	{
		next = link->next;
		if (link->dest)
			fz_drop_obj(ctx, link->dest);
		fz_free(ctx, link);
		link = next;
	}
	while(link != NULL);
}

static fz_obj *
resolve_dest(pdf_xref *xref, fz_obj *dest)
{
	if (fz_is_name(xref->ctx, dest) || fz_is_string(xref->ctx, dest))
	{
		dest = pdf_lookup_dest(xref, dest);
		return resolve_dest(xref, dest);
	}

	else if (fz_is_array(xref->ctx, dest))
	{
		return dest;
	}

	else if (fz_is_dict(xref->ctx, dest))
	{
		dest = fz_dict_gets(xref->ctx, dest, "D");
		return resolve_dest(xref, dest);
	}

	else if (fz_is_indirect(dest))
		return dest;

	return NULL;
}

pdf_link *
pdf_load_link(pdf_xref *xref, fz_obj *dict)
{
	fz_obj *dest;
	fz_obj *action;
	fz_obj *obj;
	fz_rect bbox;
	pdf_link_kind kind;
	fz_context *ctx = xref->ctx;

	dest = NULL;

	obj = fz_dict_gets(ctx, dict, "Rect");
	if (obj)
		bbox = pdf_to_rect(ctx, obj);
	else
		bbox = fz_empty_rect;

	obj = fz_dict_gets(ctx, dict, "Dest");
	if (obj)
	{
		kind = PDF_LINK_GOTO;
		dest = resolve_dest(xref, obj);
	}

	action = fz_dict_gets(ctx, dict, "A");

	/* fall back to additional action button's down/up action */
	if (!action)
		action = fz_dict_getsa(ctx, fz_dict_gets(ctx, dict, "AA"), "U", "D");

	if (action)
	{
		obj = fz_dict_gets(ctx, action, "S");
		if (fz_is_name(ctx, obj) && !strcmp(fz_to_name(ctx, obj), "GoTo"))
		{
			kind = PDF_LINK_GOTO;
			dest = resolve_dest(xref, fz_dict_gets(ctx, action, "D"));
		}
		else if (fz_is_name(ctx, obj) && !strcmp(fz_to_name(ctx, obj), "URI"))
		{
			kind = PDF_LINK_URI;
			dest = fz_dict_gets(ctx, action, "URI");
		}
		else if (fz_is_name(ctx, obj) && !strcmp(fz_to_name(ctx, obj), "Launch"))
		{
			kind = PDF_LINK_LAUNCH;
			dest = fz_dict_gets(ctx, action, "F");
		}
		else if (fz_is_name(ctx, obj) && !strcmp(fz_to_name(ctx, obj), "Named"))
		{
			kind = PDF_LINK_NAMED;
			dest = fz_dict_gets(ctx, action, "N");
		}
		else if (fz_is_name(ctx, obj) && (!strcmp(fz_to_name(ctx, obj), "GoToR")))
		{
			kind = PDF_LINK_ACTION;
			dest = action;
		}
		else
		{
			dest = NULL;
		}
	}

	if (dest)
	{
		pdf_link *link = fz_malloc(ctx, sizeof(pdf_link));
		link->kind = kind;
		link->rect = bbox;
		link->dest = fz_keep_obj(dest);
		link->next = NULL;
		return link;
	}

	return NULL;
}

void
pdf_load_links(pdf_link **linkp, pdf_xref *xref, fz_obj *annots)
{
	pdf_link *link, *head, *tail;
	fz_obj *obj;
	int i, n;
	fz_context *ctx = xref->ctx;

	head = tail = NULL;
	link = NULL;

	n = fz_array_len(ctx, annots);
	for (i = 0; i < n; i++)
	{
		obj = fz_array_get(ctx, annots, i);
		link = pdf_load_link(xref, obj);
		if (link)
		{
			if (!head)
				head = tail = link;
			else
			{
				tail->next = link;
				tail = link;
			}
		}
	}

	*linkp = head;
}

void
pdf_free_annot(fz_context *ctx, pdf_annot *annot)
{
	pdf_annot *next;

	do
	{
		next = annot->next;
		if (annot->ap)
			pdf_drop_xobject(ctx, annot->ap);
		if (annot->obj)
			fz_drop_obj(ctx, annot->obj);
		fz_free(ctx, annot);
		annot = next;
	}
	while (annot != NULL);
}

static void
pdf_transform_annot(pdf_annot *annot)
{
	fz_matrix matrix = annot->ap->matrix;
	fz_rect bbox = annot->ap->bbox;
	fz_rect rect = annot->rect;
	float w, h, x, y;

	bbox = fz_transform_rect(matrix, bbox);
	w = (rect.x1 - rect.x0) / (bbox.x1 - bbox.x0);
	h = (rect.y1 - rect.y0) / (bbox.y1 - bbox.y0);
	x = rect.x0 - bbox.x0;
	y = rect.y0 - bbox.y0;

	annot->matrix = fz_concat(fz_scale(w, h), fz_translate(x, y));
}

/* SumatraPDF: synthesize appearance streams for a few more annotations */
static pdf_annot *
pdf_create_annot(fz_context *ctx, fz_rect rect, fz_obj *base_obj, fz_buffer *content, fz_obj *resources)
{
	pdf_annot *annot;
	pdf_xobject *form;
	int rotate = fz_to_int(ctx, fz_dict_gets(ctx, fz_dict_gets(ctx, base_obj, "MK"), "R")); 

	form = fz_malloc(ctx, sizeof(pdf_xobject));
	memset(form, 0, sizeof(pdf_xobject));
	form->refs = 1;
	form->matrix = fz_rotate(rotate);
	form->bbox.x1 = (rotate % 180 == 0) ? rect.x1 - rect.x0 : rect.y1 - rect.y0;
	form->bbox.y1 = (rotate % 180 == 0) ? rect.y1 - rect.y0 : rect.x1 - rect.x0;
	form->isolated = 1;
	form->contents = content;
	form->resources = resources;

	annot = fz_malloc(ctx, sizeof(pdf_annot));
	annot->obj = base_obj;
	annot->rect = rect;
	annot->ap = form;
	annot->next = NULL;

	pdf_transform_annot(annot);

	return annot;
}

static fz_obj *
pdf_clone_for_view_only(pdf_xref *xref, fz_obj *obj)
{
	fz_context *ctx = xref->ctx;

	char *string = "<< /OCGs << /Usage << /Print << /PrintState /OFF >> /Export << /ExportState /OFF >> >> >> >>";
	fz_stream *stream = fz_open_memory(ctx, string, strlen(string));
	fz_obj *tmp = NULL;
	pdf_parse_stm_obj(&tmp, NULL, stream, xref->scratch, sizeof(xref->scratch));
	fz_close(stream);

	obj = fz_copy_dict(ctx, pdf_resolve_indirect(obj));
	fz_dict_puts(ctx, obj, "OC", tmp);
	fz_drop_obj(ctx, tmp);

	return obj;
}

static void
fz_buffer_printf(fz_context *ctx, fz_buffer *buffer, char *fmt, ...)
{
	int count;
	va_list args;
	va_start(args, fmt);
retry_larger_buffer:
	count = _vsnprintf(buffer->data + buffer->len, buffer->cap - buffer->len, fmt, args);
	if (count < 0 || count >= buffer->cap - buffer->len)
	{
		fz_grow_buffer(ctx, buffer);
		goto retry_larger_buffer;
	}
	buffer->len += count;
	va_end(args);
}

/* SumatraPDF: partial support for link borders */
static pdf_annot *
pdf_create_link_annot(pdf_xref *xref, fz_obj *obj)
{
	fz_obj *border, *color, *dashes;
	fz_rect rect;
	fz_buffer *content;
	fz_context *ctx = xref->ctx;
	int i;

	border = fz_dict_gets(ctx, obj, "Border");
	if (fz_to_real(ctx, fz_array_get(ctx, border, 2)) <= 0)
		return NULL;

	color = fz_dict_gets(ctx, obj, "C");
	dashes = fz_array_get(ctx, border, 3);
	rect = pdf_to_rect(ctx, fz_dict_gets(ctx, obj, "Rect"));

	obj = pdf_clone_for_view_only(xref, obj);

	// TODO: draw rounded rectangles if the first two /Border values are non-zero
	content = fz_new_buffer(ctx, 128);
	fz_buffer_printf(ctx, content, "q %.4f w [", fz_to_real(ctx, fz_array_get(ctx, border, 2)));
	for (i = 0; i < fz_array_len(ctx, dashes); i++)
		fz_buffer_printf(ctx, content, "%.4f ", fz_to_real(ctx, fz_array_get(ctx, dashes, i)));
	fz_buffer_printf(ctx, content, "] 0 d %.4f %.4f %.4f RG 0 0 %.4f %.4f re S Q",
		fz_to_real(ctx, fz_array_get(ctx, color, 0)), fz_to_real(ctx, fz_array_get(ctx, color, 1)),
		fz_to_real(ctx, fz_array_get(ctx, color, 2)), rect.x1 - rect.x0, rect.y1 - rect.y0);

	return pdf_create_annot(ctx, rect, obj, content, NULL);
}

// content stream adapted from Poppler's Annot.cc, licensed under GPLv2 and later
#define ANNOT_TEXT_AP_COMMENT \
	"0.533333 0.541176 0.521569 RG 2 w\n"                                         \
	"0 J 1 j [] 0 d\n"                                                            \
	"4 M 8 20 m 16 20 l 18.363 20 20 18.215 20 16 c 20 13 l 20 10.785 18.363 9\n" \
	"16 9 c 13 9 l 8 3 l 8 9 l 8 9 l 5.637 9 4 10.785 4 13 c 4 16 l 4 18.215\n"   \
	"5.637 20 8 20 c h\n"                                                         \
	"8 20 m S\n"                                                                  \
	"0.729412 0.741176 0.713725 RG 8 21 m 16 21 l 18.363 21 20 19.215 20 17\n"    \
	"c 20 14 l 20 11.785 18.363 10\n"                                             \
	"16 10 c 13 10 l 8 4 l 8 10 l 8 10 l 5.637 10 4 11.785 4 14 c 4 17 l 4\n"     \
	"19.215 5.637 21 8 21 c h\n"                                                  \
	"8 21 m S\n"

/* SumatraPDF: partial support for text icons */
static pdf_annot *
pdf_create_text_annot(pdf_xref *xref, fz_obj *obj)
{
	fz_context *ctx = xref->ctx;
	fz_buffer *content = fz_new_buffer(ctx, 512);
	fz_rect rect = pdf_to_rect(ctx, fz_dict_gets(ctx, obj, "Rect"));
	rect.x1 = rect.x0 + 24;
	rect.y1 = rect.y0 + 24;

	obj = pdf_clone_for_view_only(xref, obj);
	// TODO: support other icons by /Name: Note, Key, Help, Paragraph, NewParagraph, Insert
	// TODO: make icon semi-transparent(?)
	fz_buffer_printf(ctx, content, "q %s Q", ANNOT_TEXT_AP_COMMENT);

	return pdf_create_annot(ctx, rect, obj, content, NULL);
}

/* cf. http://bugs.ghostscript.com/show_bug.cgi?id=692078 */
static fz_obj *
pdf_dict_get_inheritable(pdf_xref *xref, fz_obj *obj, char *key)
{
	fz_context *ctx = xref->ctx;
	while (obj)
	{
		fz_obj *val = fz_dict_gets(ctx, obj, key);
		if (val)
			return val;
		obj = fz_dict_gets(ctx, obj, "Parent");
	}
	return fz_dict_gets(ctx, fz_dict_gets(ctx, fz_dict_gets(ctx, xref->trailer, "Root"), "AcroForm"), key);
}

static float
pdf_extract_font_size(pdf_xref *xref, char *appearance, char **font_name)
{
	fz_context *ctx = xref->ctx;
	fz_stream *stream = fz_open_memory(ctx, appearance, strlen(appearance));
	float font_size = 0;
	int tok, len;

	*font_name = NULL;
	do
	{
		fz_error error = pdf_lex(&tok, stream, xref->scratch, sizeof(xref->scratch), &len);
		if (error || tok == PDF_TOK_EOF)
		{
			fz_free(ctx, *font_name);
			*font_name = NULL;
			break;
		}
		if (tok == PDF_TOK_NAME)
		{
			fz_free(ctx, *font_name);
			*font_name = fz_strdup(ctx, xref->scratch);
		}
		else if (tok == PDF_TOK_REAL || tok == PDF_TOK_INT)
		{
			font_size = fz_atof(xref->scratch);
		}
	} while (tok != PDF_TOK_KEYWORD || strcmp(xref->scratch, "Tf") != 0);
	fz_close(stream);
	return font_size;
}

static fz_obj *
pdf_get_ap_stream(pdf_xref *xref, fz_obj *obj)
{
	fz_obj *ap = fz_dict_gets(xref->ctx, obj, "AP");
	if (!fz_is_dict(xref->ctx, ap))
		return NULL;

	ap = fz_dict_gets(xref->ctx, ap, "N");
	if (!pdf_is_stream(xref, fz_to_num(ap), fz_to_gen(ap)))
		ap = fz_dict_get(xref->ctx, ap, fz_dict_gets(xref->ctx, obj, "AS"));
	if (!pdf_is_stream(xref, fz_to_num(ap), fz_to_gen(ap)))
		return NULL;

	return ap;
}

static void
pdf_prepend_ap_background(fz_buffer *content, pdf_xref *xref, fz_obj *obj)
{
	fz_context *ctx = xref->ctx;
	pdf_xobject *form;
	int i;
	fz_obj *ap = pdf_get_ap_stream(xref, obj);
	if (!ap)
		return;
	if (pdf_load_xobject(&form, xref, ap) != fz_okay)
		return;

	for (i = 0; i < form->contents->len - 3 && memcmp(form->contents->data + i, "/Tx", 3) != 0; i++);
	if (i == form->contents->len - 3)
		i = form->contents->len;
	if (content->cap < content->len + i)
		fz_resize_buffer(ctx, content, content->len + i);
	memcpy(content->data + content->len, form->contents->data, i);
	content->len += i;

	pdf_drop_xobject(ctx, form);
}

static void
pdf_string_to_Tj(fz_context *ctx, fz_buffer *content, unsigned short *ucs2, unsigned short *end)
{
	fz_buffer_printf(ctx, content, "(");
	for (; ucs2 < end; ucs2++)
	{
		// TODO: convert to CID(?)
		if (*ucs2 < 0x20 || *ucs2 == '(' || *ucs2 == ')' || *ucs2 == '\\')
			fz_buffer_printf(ctx, content, "\\%03o", *ucs2);
		else
			fz_buffer_printf(ctx, content, "%c", *ucs2);
	}
	fz_buffer_printf(ctx, content, ") Tj ");
}

static int
pdf_get_string_width(pdf_xref *xref, fz_obj *res, fz_buffer *base, unsigned short *string, unsigned short *end)
{
	fz_bbox bbox;
	fz_error error;
	int width, old_len = base->len;
	fz_device *dev = fz_new_bbox_device(xref->ctx, &bbox);

	pdf_string_to_Tj(xref->ctx, base, string, end);
	fz_buffer_printf(xref->ctx, base, "ET Q EMC");
	error = pdf_run_glyph(xref, res, base, dev, fz_identity);
	width = error ? -1 : bbox.x1 - bbox.x0;
	base->len = old_len;
	fz_free_device(dev);

	return width;
}

#define iswspace(c) ((c) == 32 || 9 <= (c) && (c) <= 13)

static unsigned short *
pdf_append_line(pdf_xref *xref, fz_obj *res, fz_buffer *content, fz_buffer *base_ap,
	unsigned short *ucs2, float font_size, int align, float width, int is_multiline, float *x)
{
	unsigned short *end, *keep;
	float x1 = 0;
	int w;

	if (is_multiline)
	{
		end = ucs2;
		do
		{
			for (keep = end + 1; *keep && !iswspace(*keep); keep++);
			w = pdf_get_string_width(xref, res, base_ap, ucs2, keep);
			if (w <= width || end == ucs2)
				end = keep;
		} while (w <= width && *end);
	}
	else
		end = ucs2 + wcslen(ucs2);

	if (align != 0)
	{
		w = pdf_get_string_width(xref, res, base_ap, ucs2, end);
		if (w < 0)
			fz_error_handle(xref->ctx, -1, "can't change the text's alignment");
		else if (align == 1 /* centered */)
			x1 = (width - w) / 2;
		else if (align == 2 /* right-aligned */)
			x1 = width - w;
		else
			fz_warn(xref->ctx, "ignoring unknown quadding value %d", align);
	}

	fz_buffer_printf(xref->ctx, content, "%.4f %.4f Td ", x1 - *x, -font_size);
	pdf_string_to_Tj(xref->ctx, content, ucs2, end);
	*x = x1;

	return end + (*end ? 1 : 0);
}

static void
pdf_append_combed_line(pdf_xref *xref, fz_obj *res, fz_buffer *content, fz_buffer *base_ap,
	unsigned short *ucs2, float font_size, float width, int max_len)
{
	float comb_width = max_len > 0 ? width / max_len : 0;
	unsigned short c[2] = { 0 };
	float x = -2.0f;
	int i;

	fz_buffer_printf(xref->ctx, content, "0 %.4f Td ", -font_size);
	for (i = 0; i < max_len && ucs2[i]; i++)
	{
		*c = ucs2[i];
		pdf_append_line(xref, res, content, base_ap, c, 0, 1 /* centered */, comb_width, 0, &x);
		x -= comb_width;
	}
}

static pdf_annot *
pdf_update_tx_widget_annot(pdf_xref *xref, fz_obj *obj)
{
	fz_obj *ap, *res, *value;
	fz_rect rect;
	fz_buffer *content, *base_ap;
	int flags, align, rotate, is_multiline;
	float font_size, x, y;
	char *font_name;
	unsigned short *ucs2, *rest;

	fz_context *ctx = xref->ctx;

	if (strcmp(fz_to_name(ctx, fz_dict_gets(ctx, obj, "Subtype")), "Widget") != 0)
		return NULL;
	if (!fz_to_bool(ctx, pdf_dict_get_inheritable(xref, NULL, "NeedAppearances")) && pdf_get_ap_stream(xref, obj))
		return NULL;
	value = pdf_dict_get_inheritable(xref, obj, "FT");
	if (strcmp(fz_to_name(ctx, value), "Tx") != 0)
		return NULL;

	ap = pdf_dict_get_inheritable(xref, obj, "DA");
	value = pdf_dict_get_inheritable(xref, obj, "V");
	if (!ap || !value)
		return NULL;

	res = pdf_dict_get_inheritable(xref, obj, "DR");
	rect = pdf_to_rect(ctx, fz_dict_gets(ctx, obj, "Rect"));
	rotate = fz_to_int(ctx, fz_dict_gets(ctx, fz_dict_gets(ctx, obj, "MK"), "R"));
	rect = fz_transform_rect(fz_rotate(rotate), rect);

	flags = fz_to_int(ctx, fz_dict_gets(ctx, obj, "Ff"));
	is_multiline = (flags & (1 << 12)) != 0;
	if ((flags & (1 << 25) /* richtext */))
		fz_warn(ctx, "missing support for richtext fields");
	align = fz_to_int(ctx, fz_dict_gets(ctx, obj, "Q"));

	font_size = pdf_extract_font_size(xref, fz_to_str_buf(ctx, ap), &font_name);
	if (!font_size || !font_name)
		font_size = is_multiline ? 10 /* FIXME */ : floor(rect.y1 - rect.y0 - 2);

	content = fz_new_buffer(ctx, 256);
	base_ap = fz_new_buffer(ctx, 256);
	pdf_prepend_ap_background(content, xref, obj);
	fz_buffer_printf(ctx, content, "/Tx BMC q 1 1 %.4f %.4f re W n BT %s ",
		rect.x1 - rect.x0 - 2.0f, rect.y1 - rect.y0 - 2.0f, fz_to_str_buf(ctx, ap));
	fz_buffer_printf(ctx, base_ap, "/Tx BMC q BT %s ", fz_to_str_buf(ctx, ap));
	if (font_name)
	{
		fz_buffer_printf(ctx, content, "/%s %.4f Tf ", font_name, font_size);
		fz_buffer_printf(ctx, base_ap, "/%s %.4f Tf ", font_name, font_size);
		fz_free(ctx, font_name);
	}
	y = 0.5f * (rect.y1 - rect.y0) + 0.6f * font_size;
	if (is_multiline)
		y = rect.y1 - rect.y0 - 2;
	fz_buffer_printf(ctx, content, "1 0 0 1 2 %.4f Tm ", y);

	ucs2 = pdf_to_ucs2(ctx, value);
	for (rest = ucs2; *rest; rest++)
		if (*rest > 0xFF)
			*rest = '?';
	if ((flags & (1 << 13) /* password */))
		for (rest = ucs2; *rest; rest++)
			*rest = '*';

	x = 0;
	rest = ucs2;
	if ((flags & (1 << 24) /* comb */))
	{
		pdf_append_combed_line(xref, res, content, base_ap, ucs2, font_size, rect.x1 - rect.x0, fz_to_int(ctx, pdf_dict_get_inheritable(xref, obj, "MaxLen")));
		rest = L"";
	}
	while (*rest)
		rest = pdf_append_line(xref, res, content, base_ap, rest, font_size, align, rect.x1 - rect.x0 - 4.0f, is_multiline, &x);

	fz_free(ctx, ucs2);
	fz_buffer_printf(ctx, content, "ET Q EMC");

	rect = fz_transform_rect(fz_rotate(-rotate), rect);
	return pdf_create_annot(ctx, rect, fz_keep_obj(obj), content, res ? fz_keep_obj(res) : NULL);
}

static pdf_annot *
pdf_create_annot_with_appearance(pdf_xref *xref, fz_obj *obj)
{
	fz_context *ctx = xref->ctx;

	if (!strcmp(fz_to_name(ctx, fz_dict_gets(ctx, obj, "Subtype")), "Link"))
		return pdf_create_link_annot(xref, obj);
	if (!strcmp(fz_to_name(ctx, fz_dict_gets(ctx, obj, "Subtype")), "Text"))
		return pdf_create_text_annot(xref, obj);
	return NULL;
}

void
pdf_load_annots(pdf_annot **annotp, pdf_xref *xref, fz_obj *annots)
{
	pdf_annot *annot, *head, *tail;
	fz_obj *obj, *ap, *as, *n, *rect;
	pdf_xobject *form;
	fz_error error;
	int i, len;
	fz_context *ctx = xref->ctx;

	head = tail = NULL;
	annot = NULL;

	len = fz_array_len(ctx, annots);
	for (i = 0; i < len; i++)
	{
		obj = fz_array_get(ctx, annots, i);
		/* cf. http://bugs.ghostscript.com/show_bug.cgi?id=692078 */
		if ((annot = pdf_update_tx_widget_annot(xref, obj)))
		{
			if (!head)
				head = tail = annot;
			else
			{
				tail->next = annot;
				tail = annot;
			}
			continue;
		}

		rect = fz_dict_gets(ctx, obj, "Rect");
		ap = fz_dict_gets(ctx, obj, "AP");
		as = fz_dict_gets(ctx, obj, "AS");
		if (fz_is_dict(ctx, ap))
		{
			n = fz_dict_gets(ctx, ap, "N"); /* normal state */

			/* lookup current state in sub-dictionary */
			if (!pdf_is_stream(xref, fz_to_num(n), fz_to_gen(n)))
				n = fz_dict_get(ctx, n, as);

			if (pdf_is_stream(xref, fz_to_num(n), fz_to_gen(n)))
			{
				error = pdf_load_xobject(&form, xref, n);
				if (error)
				{
					fz_error_handle(ctx, error, "ignoring broken annotation");
					continue;
				}

				annot = fz_malloc(ctx, sizeof(pdf_annot));
				annot->obj = fz_keep_obj(obj);
				annot->rect = pdf_to_rect(ctx, rect);
				annot->ap = form;
				annot->next = NULL;

				pdf_transform_annot(annot);

				if (annot)
				{
					if (!head)
						head = tail = annot;
					else
					{
						tail->next = annot;
						tail = annot;
					}
				}
			}
		}
		/* SumatraPDF: synthesize appearance streams for a few more annotations */
		else if ((annot = pdf_create_annot_with_appearance(xref, obj)))
		{
			if (!head)
				head = tail = annot;
			else
			{
				tail->next = annot;
				tail = annot;
			}
		}
	}

	*annotp = head;
}
