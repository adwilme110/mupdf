#include "fitz.h"
#include "mupdf.h"

void
pdf_free_link(fz_context *ctx, pdf_link *link)
{
	if (link->next)
		pdf_free_link(ctx, link->next);
		if (link->dest)
			fz_drop_obj(ctx, link->dest);
	fz_free(ctx, link);
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
	int i;
	fz_context *ctx = xref->ctx;

	head = tail = NULL;
	link = NULL;

	for (i = 0; i < fz_array_len(ctx, annots); i++)
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
	if (annot->next)
		pdf_free_annot(ctx, annot->next);
		if (annot->ap)
		pdf_drop_xobject(ctx, annot->ap);
		if (annot->obj)
		fz_drop_obj(ctx, annot->obj);
	fz_free(ctx, annot);
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
pdf_create_annot(fz_context *ctx, fz_rect rect, fz_obj *base_obj, fz_buffer *content, fz_obj *resources, int transparency)
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
	form->transparency = transparency;
	form->isolated = !transparency;
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
pdf_dict_from_string(pdf_xref *xref, char *string)
{
	fz_context *ctx = xref->ctx;
	fz_obj *result = NULL;

	fz_stream *stream = fz_open_memory(ctx, string, strlen(string));
	pdf_parse_stm_obj(&result, NULL, stream, xref->scratch, sizeof(xref->scratch));
	fz_close(stream);

	return result;
}

#define ANNOT_OC_VIEW_ONLY \
	"<< /OCGs << /Usage << /Print << /PrintState /OFF >> /Export << /ExportState /OFF >> >> >> >>"

static fz_obj *
pdf_clone_for_view_only(pdf_xref *xref, fz_obj *obj)
{
	fz_obj *ocgs = pdf_dict_from_string(xref, ANNOT_OC_VIEW_ONLY);

	obj = fz_copy_dict(xref->ctx, pdf_resolve_indirect(obj));
	fz_dict_puts(xref->ctx, obj, "OC", ocgs);
	fz_drop_obj(xref->ctx, ocgs);

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

static void
pdf_get_annot_color(fz_context *ctx, fz_obj *obj, float rgb[3])
{
	int k;
	obj = fz_dict_gets(ctx, obj, "C");
	for (k = 0; k < 3; k++)
		rgb[k] = fz_to_real(ctx, fz_array_get(ctx, obj, k));
}

/* SumatraPDF: partial support for link borders */
static pdf_annot *
pdf_create_link_annot(pdf_xref *xref, fz_obj *obj)
{
	fz_obj *border, *dashes;
	fz_buffer *content;
	fz_rect rect;
	float rgb[3];
	int i;

	border = fz_dict_gets(xref->ctx, obj, "Border");
	if (fz_to_real(xref->ctx, fz_array_get(xref->ctx, border, 2)) <= 0)
		return NULL;

	pdf_get_annot_color(xref->ctx, obj, rgb);
	dashes = fz_array_get(xref->ctx, border, 3);
	rect = pdf_to_rect(xref->ctx, fz_dict_gets(xref->ctx, obj, "Rect"));

	obj = pdf_clone_for_view_only(xref, obj);

	// TODO: draw rounded rectangles if the first two /Border values are non-zero
	content = fz_new_buffer(xref->ctx, 128);
	fz_buffer_printf(xref->ctx, content, "q %.4f w [", fz_to_real(xref->ctx, fz_array_get(xref->ctx, border, 2)));
	for (i = 0; i < fz_array_len(xref->ctx, dashes); i++)
		fz_buffer_printf(xref->ctx, content, "%.4f ", fz_to_real(xref->ctx, fz_array_get(xref->ctx, dashes, i)));
	fz_buffer_printf(xref->ctx, content, "] 0 d %.4f %.4f %.4f RG 0 0 %.4f %.4f re S Q",
		rgb[0], rgb[1], rgb[2], rect.x1 - rect.x0, rect.y1 - rect.y0);

	return pdf_create_annot(xref->ctx, rect, obj, content, NULL, 0);
}

// appearance streams adapted from Poppler's Annot.cc, licensed under GPLv2 and later
#define ANNOT_TEXT_AP_NOTE \
	"%.4f %.4f %.4f RG 1 J 1 j [] 0 d 4 M\n"                                    \
	"2 w 9 18 m 4 18 l 4 7 4 4 6 3 c 20 3 l 18 4 18 7 18 18 c 17 18 l S\n"      \
	"1.5 w 10 16 m 14 21 l S\n"                                                 \
	"1.85625 w\n"                                                               \
	"15.07 20.523 m 15.07 19.672 14.379 18.977 13.523 18.977 c 12.672 18.977\n" \
	"11.977 19.672 11.977 20.523 c 11.977 21.379 12.672 22.07 13.523 22.07 c\n" \
	"14.379 22.07 15.07 21.379 15.07 20.523 c h S\n"                            \
	"1 w 6.5 13.5 m 15.5 13.5 l S 6.5 10.5 m 13.5 10.5 l S\n"                   \
	"6.801 7.5 m 15.5 7.5 l S\n"

#define ANNOT_TEXT_AP_COMMENT \
	"%.4f %.4f %.4f RG 0 J 1 j [] 0 d 4 M 2 w\n"                                \
	"8 20 m 16 20 l 18.363 20 20 18.215 20 16 c 20 13 l 20 10.785 18.363 9\n"   \
	"16 9 c 13 9 l 8 3 l 8 9 l 8 9 l 5.637 9 4 10.785 4 13 c 4 16 l\n"          \
	"4 18.215 5.637 20 8 20 c h S\n"

#define ANNOT_TEXT_AP_KEY \
	"%.4f %.4f %.4f RG 0 J 1 j [] 0 d 4 M\n"                                    \
	"2 w 11.895 18.754 m 13.926 20.625 17.09 20.496 18.961 18.465 c 20.832\n"   \
	"16.434 20.699 13.27 18.668 11.398 c 17.164 10.016 15.043 9.746 13.281\n"   \
	"10.516 c 12.473 9.324 l 11.281 10.078 l 9.547 8.664 l 9.008 6.496 l\n"     \
	"7.059 6.059 l 6.34 4.121 l 5.543 3.668 l 3.375 4.207 l 2.938 6.156 l\n"    \
	"10.57 13.457 l 9.949 15.277 10.391 17.367 11.895 18.754 c h S\n"           \
	"1.5 w 16.059 15.586 m 16.523 15.078 17.316 15.043 17.824 15.512 c\n"       \
	"18.332 15.98 18.363 16.77 17.895 17.277 c 17.43 17.785 16.637 17.816\n"    \
	"16.129 17.352 c 15.621 16.883 15.59 16.094 16.059 15.586 c h S\n"

#define ANNOT_TEXT_AP_HELP \
	"%.4f %.4f %.4f RG 0 J 1 j [] 0 d 4 M 2.5 w\n"                              \
	"8.289 16.488 m 8.824 17.828 10.043 18.773 11.473 18.965 c 12.902 19.156\n" \
	"14.328 18.559 15.195 17.406 c 16.062 16.254 16.242 14.723 15.664 13.398\n" \
	"c S 12 8 m 12 12 16 11 16 15 c S\n"                                        \
	"q 1 0 0 -1 0 24 cm 1.539286 w\n"                                           \
	"12.684 20.891 m 12.473 21.258 12.004 21.395 11.629 21.196 c 11.254\n"      \
	"20.992 11.105 20.531 11.297 20.149 c 11.488 19.77 11.945 19.61 12.332\n"   \
	"19.789 c 12.719 19.969 12.891 20.426 12.719 20.817 c S Q\n"

#define ANNOT_TEXT_AP_PARAGRAPH \
	"%.4f %.4f %.4f RG 1 J 1 j [] 0 d 4 M 2 w\n"                                \
	"15 3 m 15 18 l 11 18 l 11 3 l S\n"                                         \
	"q 1 0 0 -1 0 24 cm 4 w\n"                                                  \
	"9.777 10.988 m 8.746 10.871 7.973 9.988 8 8.949 c 8.027 7.91 8.844\n"      \
	"7.066 9.879 7.004 c S Q\n"

#define ANNOT_TEXT_AP_NEW_PARAGRAPH \
	"%.4f %.4f %.4f RG 0 J 1 j [] 0 d 4 M 4 w\n"                                \
	"q 1 0 0 -1 0 24 cm\n"                                                      \
	"9.211 11.988 m 8.449 12.07 7.711 11.707 7.305 11.059 c 6.898 10.41\n"      \
	"6.898 9.59 7.305 8.941 c 7.711 8.293 8.449 7.93 9.211 8.012 c S Q\n"       \
	"q 1 0 0 -1 0 24 cm 1.004413 w\n"                                           \
	"18.07 11.511 m 15.113 10.014 l 12.199 11.602 l 12.711 8.323 l 10.301\n"    \
	"6.045 l 13.574 5.517 l 14.996 2.522 l 16.512 5.474 l 19.801 5.899 l\n"     \
	"17.461 8.252 l 18.07 11.511 l h S Q\n"                                     \
	"2 w 11 17 m 10 17 l 10 3 l S 14 3 m 14 13 l S\n"

#define ANNOT_TEXT_AP_INSERT \
	"%.4f %.4f %.4f RG 1 J 0 j [] 0 d 4 M 2 w\n"                                \
	"12 18.012 m 20 18 l S 9 10 m 17 10 l S 12 14.012 m 20 14 l S\n"            \
	"12 6.012 m 20 6.012 l S 4 12 m 6 10 l 4 8 l S 4 12 m 4 8 l S\n"

#define ANNOT_TEXT_AP_CROSS \
	"%.4f %.4f %.4f RG 1 J 0 j [] 0 d 4 M 2.5 w\n"                              \
	"18 5 m 6 17 l S 6 5 m 18 17 l S\n"

#define ANNOT_TEXT_AP_CIRCLE \
	"%.4f %.4f %.4f RG 1 J 1 j [] 0 d 4 M 2.5 w\n"                              \
	"19.5 11.5 m 19.5 7.359 16.141 4 12 4 c 7.859 4 4.5 7.359 4.5 11.5 c 4.5\n" \
	"15.641 7.859 19 12 19 c 16.141 19 19.5 15.641 19.5 11.5 c h S\n"

/* SumatraPDF: partial support for text icons */
static pdf_annot *
pdf_create_text_annot(pdf_xref *xref, fz_obj *obj)
{
	fz_context *ctx = xref->ctx;
	fz_buffer *content = fz_new_buffer(ctx, 512);
	fz_rect rect = pdf_to_rect(ctx, fz_dict_gets(ctx, obj, "Rect"));
	char *icon_name = fz_to_name(ctx, fz_dict_gets(ctx, obj, "Name"));
	char *content_ap = ANNOT_TEXT_AP_NOTE;
	float rgb[3];

	rect.x1 = rect.x0 + 24;
	rect.y0 = rect.y1 - 24;
	pdf_get_annot_color(ctx, obj, rgb);

	if (!strcmp(icon_name, "Comment"))
		content_ap = ANNOT_TEXT_AP_COMMENT;
	else if (!strcmp(icon_name, "Key"))
		content_ap = ANNOT_TEXT_AP_KEY;
	else if (!strcmp(icon_name, "Help"))
		content_ap = ANNOT_TEXT_AP_HELP;
	else if (!strcmp(icon_name, "Paragraph"))
		content_ap = ANNOT_TEXT_AP_PARAGRAPH;
	else if (!strcmp(icon_name, "NewParagraph"))
		content_ap = ANNOT_TEXT_AP_NEW_PARAGRAPH;
	else if (!strcmp(icon_name, "Insert"))
		content_ap = ANNOT_TEXT_AP_INSERT;
	else if (!strcmp(icon_name, "Cross"))
		content_ap = ANNOT_TEXT_AP_CROSS;
	else if (!strcmp(icon_name, "Circle"))
		content_ap = ANNOT_TEXT_AP_CIRCLE;

	// TODO: make icons semi-transparent (cf. pdf_create_highlight_annot)?
	fz_buffer_printf(ctx, content, "q ");
	fz_buffer_printf(ctx, content, content_ap, 0.5, 0.5, 0.5);
	fz_buffer_printf(ctx, content, " 1 0 0 1 0 1 cm ");
	fz_buffer_printf(ctx, content, content_ap, rgb[0], rgb[1], rgb[2]);
	fz_buffer_printf(ctx, content, " Q", content_ap);

	obj = pdf_clone_for_view_only(xref, obj);
	return pdf_create_annot(ctx, rect, obj, content, NULL, 0);
}

// appearance streams adapted from Poppler's Annot.cc, licensed under GPLv2 and later
#define ANNOT_FILE_ATTACHMENT_AP_PUSHPIN \
	"%.4f %.4f %.4f RG 1 J 1 j [] 0 d 4 M\n"                                    \
	"2 w 5 4 m 6 5 l S\n"                                                       \
	"11 14 m 9 12 l 6 12 l 13 5 l 13 8 l 15 10 l 18 11 l 20 11 l 12 19 l 12\n"  \
	"17 l 11 14 l h\n"                                                          \
	"3 w 6 5 m 9 8 l S\n"

#define ANNOT_FILE_ATTACHMENT_AP_PAPERCLIP \
	"%.4f %.4f %.4f RG 1 J 1 j [] 0 d 4 M 2 w\n"                                \
	"16.645 12.035 m 12.418 7.707 l 10.902 6.559 6.402 11.203 8.09 12.562 c\n"  \
	"14.133 18.578 l 14.949 19.387 16.867 19.184 17.539 18.465 c 20.551\n"      \
	"15.23 l 21.191 14.66 21.336 12.887 20.426 12.102 c 13.18 4.824 l 12.18\n"  \
	"3.82 6.25 2.566 4.324 4.461 c 3 6.395 3.383 11.438 4.711 12.801 c 9.648\n" \
	"17.887 l S\n"

#define ANNOT_FILE_ATTACHMENT_AP_GRAPH \
	"%.4f %.4f %.4f RG 1 J 1 j [] 0 d 4 M\n"                                    \
	"1 w 18.5 15.5 m 18.5 13.086 l 16.086 15.5 l 18.5 15.5 l h\n"               \
	"7 7 m 10 11 l 13 9 l 18 15 l S\n"                                          \
	"2 w 3 19 m 3 3 l 21 3 l S\n"

#define ANNOT_FILE_ATTACHMENT_AP_TAG \
	"%.4f %.4f %.4f RG 1 J 1 j [] 0 d 4 M\n"                                    \
	"1 w q 1 0 0 -1 0 24 cm\n"                                                  \
	"8.492 8.707 m 8.492 9.535 7.82 10.207 6.992 10.207 c 6.164 10.207 5.492\n" \
	"9.535 5.492 8.707 c 5.492 7.879 6.164 7.207 6.992 7.207 c 7.82 7.207\n"    \
	"8.492 7.879 8.492 8.707 c h S Q\n"                                         \
	"2 w\n"                                                                     \
	"2 w 20.078 11.414 m 20.891 10.602 20.785 9.293 20.078 8.586 c 14.422\n"    \
	"2.93 l 13.715 2.223 12.301 2.223 11.594 2.93 c 3.816 10.707 l 3.109\n"     \
	"11.414 2.402 17.781 3.816 19.195 c 5.23 20.609 11.594 19.902 12.301\n"     \
	"19.195 c 20.078 11.414 l h S\n"                                            \
	"1 w 11.949 13.184 m 16.191 8.941 l S 14.07 6.82 m 9.828 11.062 l S\n"      \
	"6.93 15.141 m 8 20 14.27 20.5 16 20.5 c 18.094 20.504 19.5 20 19.5 18 c\n" \
	"19.5 16.699 20.91 16.418 22.5 16.5 c S\n"

/* SumatraPDF: partial support for file attachment icons */
static pdf_annot *
pdf_create_file_annot(pdf_xref *xref, fz_obj *obj)
{
	fz_buffer *content = fz_new_buffer(xref->ctx, 512);
	fz_rect rect = pdf_to_rect(xref->ctx, fz_dict_gets(xref->ctx, obj, "Rect"));
	char *icon_name = fz_to_name(xref->ctx, fz_dict_gets(xref->ctx, obj, "Name"));
	char *content_ap = ANNOT_FILE_ATTACHMENT_AP_PUSHPIN;
	float rgb[3];

	pdf_get_annot_color(xref->ctx, obj, rgb);

	if (!strcmp(icon_name, "Graph"))
		content_ap = ANNOT_FILE_ATTACHMENT_AP_GRAPH;
	else if (!strcmp(icon_name, "Paperclip"))
		content_ap = ANNOT_FILE_ATTACHMENT_AP_PAPERCLIP;
	else if (!strcmp(icon_name, "Tag"))
		content_ap = ANNOT_FILE_ATTACHMENT_AP_TAG;

	fz_buffer_printf(xref->ctx, content, "q %.4f 0 0 %.4f 0 0 cm ",
		(rect.x1 - rect.x0) / 24, (rect.y1 - rect.y0) / 24);
	fz_buffer_printf(xref->ctx, content, content_ap, 0.5, 0.5, 0.5);
	fz_buffer_printf(xref->ctx, content, " 1 0 0 1 0 1 cm ");
	fz_buffer_printf(xref->ctx, content, content_ap, rgb[0], rgb[1], rgb[2]);
	fz_buffer_printf(xref->ctx, content, " Q", content_ap);

	obj = pdf_clone_for_view_only(xref, obj);
	return pdf_create_annot(xref->ctx, rect, obj, content, NULL, 0);
}

/* SumatraPDF: partial support for text markup annotations */

/* a: top/left to bottom/right; b: bottom/left to top/right */
static void
pdf_get_quadrilaterals(fz_context *ctx, fz_obj *quad_points, int i, fz_rect *a, fz_rect *b)
{
	a->x0 = fz_to_real(ctx, fz_array_get(ctx, quad_points, i * 8 + 0));
	a->y0 = fz_to_real(ctx, fz_array_get(ctx, quad_points, i * 8 + 1));
	b->x1 = fz_to_real(ctx, fz_array_get(ctx, quad_points, i * 8 + 2));
	b->y1 = fz_to_real(ctx, fz_array_get(ctx, quad_points, i * 8 + 3));
	b->x0 = fz_to_real(ctx, fz_array_get(ctx, quad_points, i * 8 + 4));
	b->y0 = fz_to_real(ctx, fz_array_get(ctx, quad_points, i * 8 + 5));
	a->x1 = fz_to_real(ctx, fz_array_get(ctx, quad_points, i * 8 + 6));
	a->y1 = fz_to_real(ctx, fz_array_get(ctx, quad_points, i * 8 + 7));
}

#define ANNOT_HIGHLIGHT_AP_RESOURCES \
	"<< /ExtGState << /GS << /Type/ExtGState /ca 0.8 /AIS false /BM /Multiply >> >> >>"

static pdf_annot *
pdf_create_highlight_annot(pdf_xref *xref, fz_obj *obj)
{
	fz_context *ctx = xref->ctx;
	fz_buffer *content = fz_new_buffer(ctx, 512);
	fz_rect rect = pdf_to_rect(ctx, fz_dict_gets(ctx, obj, "Rect"));
	fz_obj *quad_points = fz_dict_gets(ctx, obj, "QuadPoints");
	fz_obj *resources = pdf_dict_from_string(xref, ANNOT_HIGHLIGHT_AP_RESOURCES);
	fz_rect a, b;
	float rgb[3];
	float skew;
	int i;

	for (i = 0; i < fz_array_len(ctx, quad_points) / 8; i++)
	{
		pdf_get_quadrilaterals(ctx, quad_points, i, &a, &b);
		skew = 0.15 * fabs(a.y0 - b.y0);
		b.x0 -= skew; b.x1 += skew;
		rect = fz_union_rect(rect, fz_union_rect(a, b));
	}
	pdf_get_annot_color(ctx, obj, rgb);

	fz_buffer_printf(ctx, content, "q /GS gs %.4f %.4f %.4f rg 1 0 0 1 -%.4f -%.4f cm ",
		rgb[0], rgb[1], rgb[2], rect.x0, rect.y0);
	for (i = 0; i < fz_array_len(ctx, quad_points) / 8; i++)
	{
		pdf_get_quadrilaterals(ctx, quad_points, i, &a, &b);
		skew = 0.15 * fabs(a.y0 - b.y0);
		fz_buffer_printf(ctx, content, "%.4f %.4f m %.4f %.4f l %.4f %.4f l %.4f %.4f l h ",
			a.x0, a.y0, b.x1 + skew, b.y1, a.x1, a.y1, b.x0 - skew, b.y0);
	}
	fz_buffer_printf(ctx, content, "f Q");

	return pdf_create_annot(ctx, rect, fz_keep_obj(obj), content, resources, 1);
}

static pdf_annot *
pdf_create_markup_annot(pdf_xref *xref, fz_obj *obj, char *type)
{
	fz_context *ctx = xref->ctx;
	fz_buffer *content = fz_new_buffer(ctx, 512);
	fz_rect rect = pdf_to_rect(ctx, fz_dict_gets(ctx, obj, "Rect"));
	fz_obj *quad_points = fz_dict_gets(ctx, obj, "QuadPoints");
	fz_rect a, b;
	float rgb[3];
	int i;

	for (i = 0; i < fz_array_len(ctx, quad_points) / 8; i++)
	{
		pdf_get_quadrilaterals(ctx, quad_points, i, &a, &b);
		b.y0 -= 0.25; a.y1 += 0.25;
		rect = fz_union_rect(rect, fz_union_rect(a, b));
	}
	pdf_get_annot_color(ctx, obj, rgb);

	fz_buffer_printf(ctx, content, "q %.4f %.4f %.4f RG 1 0 0 1 -%.4f -%.4f cm 0.5 w ",
		rgb[0], rgb[1], rgb[2], rect.x0, rect.y0);
	if (!strcmp(type, "Squiggly"))
		fz_buffer_printf(ctx, content, "[1 1] d ");
	for (i = 0; i < fz_array_len(ctx, quad_points) / 8; i++)
	{
		pdf_get_quadrilaterals(ctx, quad_points, i, &a, &b);
		if (!strcmp(type, "StrikeOut"))
			fz_buffer_printf(ctx, content, "%.4f %.4f m %.4f %.4f l ",
				(a.x0 + b.x0) / 2, (a.y0 + b.y0) / 2, (a.x1 + b.x1) / 2, (a.y1 + b.y1) / 2);
		else
			fz_buffer_printf(ctx, content, "%.4f %.4f m %.4f %.4f l ", b.x0, b.y0, a.x1, a.y1);
	}
	fz_buffer_printf(ctx, content, "S Q");

	return pdf_create_annot(ctx, rect, fz_keep_obj(obj), content, NULL, 0);
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
			if (*end == '\n' || *end == '\r' && *(end + 1) != '\n')
				break;

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
	fz_drop_buffer(ctx, base_ap);

	rect = fz_transform_rect(fz_rotate(-rotate), rect);
	return pdf_create_annot(ctx, rect, fz_keep_obj(obj), content, res ? fz_keep_obj(res) : NULL, 0);
}

/* SumatraPDF: partial support for freetext annotations */

#define ANNOT_FREETEXT_AP_RESOURCES \
	"<< /Font << /Default << /Type /Font /BaseFont /Helvetica /Subtype /Type1 >> >> >>"

static pdf_annot *
pdf_create_freetext_annot(pdf_xref *xref, fz_obj *obj)
{
	fz_context *ctx = xref->ctx;
	fz_buffer *content = fz_new_buffer(ctx, 256);
	fz_buffer *base_ap = fz_new_buffer(ctx, 256);
	fz_obj *ap = fz_dict_gets(ctx, obj, "DA");
	fz_obj *value = fz_dict_gets(ctx, obj, "Contents");
	fz_rect rect = pdf_to_rect(ctx, fz_dict_gets(ctx, obj, "Rect"));
	int align = fz_to_int(ctx, fz_dict_gets(ctx, obj, "Q"));
	fz_obj *res = pdf_dict_from_string(xref, ANNOT_FREETEXT_AP_RESOURCES);
	unsigned short *ucs2, *rest;
	float x;

	char *font_name = NULL;
	float font_size = pdf_extract_font_size(xref, fz_to_str_buf(ctx, ap), &font_name);
	if (!font_size)
		font_size = 10;
	/* TODO: what resource dictionary does this font name refer to? */
	if (font_name)
	{
		fz_obj *font = fz_dict_gets(ctx, res, "Font");
		fz_dict_puts(ctx, font, font_name, fz_dict_gets(ctx, font, "Default"));
		fz_free(ctx, font_name);
	}

	fz_buffer_printf(ctx, content, "q 1 1 %.4f %.4f re W n BT %s ",
		rect.x1 - rect.x0 - 2.0f, rect.y1 - rect.y0 - 2.0f, fz_to_str_buf(ctx, ap));
	fz_buffer_printf(ctx, base_ap, "q BT %s ", fz_to_str_buf(ctx, ap));
	fz_buffer_printf(ctx, content, "/Default %.4f Tf ", font_size);
	fz_buffer_printf(ctx, base_ap, "/Default %.4f Tf ", font_size);
	fz_buffer_printf(ctx, content, "1 0 0 1 2 %.4f Tm ", rect.y1 - rect.y0 - 2);

	/* Adobe Reader seems to consider "[1 0 0] r" and "1 0 0 rg" to mean the same(?) */
	if (strchr(base_ap->data, '['))
	{
		float r, g, b;
		if (sscanf(strchr(base_ap->data, '['), "[%f %f %f] r", &r, &g, &b) == 3)
			fz_buffer_printf(ctx, content, "%.4f %.4f %.4f rg ", r, g, b);
	}

	ucs2 = pdf_to_ucs2(ctx, value);
	for (rest = ucs2; *rest; rest++)
		if (*rest > 0xFF)
			*rest = '?';

	x = 0;
	rest = ucs2;
	while (*rest)
		rest = pdf_append_line(xref, res, content, base_ap, rest, font_size, align, rect.x1 - rect.x0 - 4.0f, 1, &x);

	fz_free(ctx, ucs2);
	fz_buffer_printf(ctx, content, "ET Q");
	fz_drop_buffer(ctx, base_ap);

	return pdf_create_annot(ctx, rect, fz_keep_obj(obj), content, res, 0);
}

static pdf_annot *
pdf_create_annot_with_appearance(pdf_xref *xref, fz_obj *obj)
{
	fz_context *ctx = xref->ctx;
	char *type = fz_to_name(ctx, fz_dict_gets(ctx, obj, "Subtype"));

	if (!strcmp(type, "Link"))
		return pdf_create_link_annot(xref, obj);
	if (!strcmp(type, "Text"))
		return pdf_create_text_annot(xref, obj);
	if (!strcmp(type, "FileAttachment"))
		return pdf_create_file_annot(xref, obj);
	/* TODO: Adobe Reader seems to sometimes ignore the appearance stream for highlights(?) */
	if (!strcmp(type, "Highlight"))
		return pdf_create_highlight_annot(xref, obj);
	if (!strcmp(type, "Underline") || !strcmp(type, "StrikeOut") || !strcmp(type, "Squiggly"))
		return pdf_create_markup_annot(xref, obj, type);
	if (!strcmp(type, "FreeText"))
		return pdf_create_freetext_annot(xref, obj);

	return NULL;
}

void
pdf_load_annots(pdf_annot **annotp, pdf_xref *xref, fz_obj *annots)
{
	pdf_annot *annot, *head, *tail;
	fz_obj *obj, *ap, *as, *n, *rect;
	pdf_xobject *form;
	fz_error error;
	int i;
	fz_context *ctx = xref->ctx;

	head = tail = NULL;
	annot = NULL;

	for (i = 0; i < fz_array_len(ctx, annots); i++)
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
